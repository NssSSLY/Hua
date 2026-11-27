#include "hua/http_backend.hpp"
#include "hua/tasks.hpp"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>
#endif

namespace hua {
namespace {
using SteadyClock = std::chrono::steady_clock;

std::string to_lower_ascii(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool has_crlf(std::string_view s) {
    for (char c : s) if (c == '\r' || c == '\n' || c == '\0') return true;
    return false;
}

std::string resolve_redirect_url(const ParsedUrl& base, std::string_view loc) {
    if (loc.find("://") != std::string_view::npos) return std::string(loc);
    std::string prefix = base.scheme + "://" + base.host;
    bool def_port = (base.scheme == "https" && base.port == 443) || (base.scheme == "http" && base.port == 80);
    if (!def_port) prefix += ":" + std::to_string(base.port);
    if (!loc.empty() && loc.front() == '/') return prefix + std::string(loc);
    std::string dir = base.path;
    auto slash = dir.rfind('/');
    if (slash == std::string::npos) dir = "/";
    else dir.resize(slash + 1);
    return prefix + dir + std::string(loc);
}

#ifdef _WIN32
std::wstring utf8_to_wide(std::string_view s) {
    if (s.empty()) return {};
    int need = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (need <= 0) return {};
    std::wstring out(static_cast<std::size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), need);
    return out;
}

std::string wide_to_utf8(std::wstring_view w) {
    if (w.empty()) return {};
    int need = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out(static_cast<std::size_t>(need), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), need, nullptr, nullptr);
    return out;
}

struct WinHttpApi {
    bool loaded{};
    HMODULE mod{};
    HINTERNET(WINAPI* open)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD){};
    BOOL(WINAPI* close_handle)(HINTERNET){};
    HINTERNET(WINAPI* connect)(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD){};
    HINTERNET(WINAPI* open_request)(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR*, DWORD){};
    BOOL(WINAPI* set_timeouts)(HINTERNET, int, int, int, int){};
    BOOL(WINAPI* set_option)(HINTERNET, DWORD, LPVOID, DWORD){};
    BOOL(WINAPI* query_option)(HINTERNET, DWORD, LPVOID, LPDWORD){};
    WINHTTP_STATUS_CALLBACK(WINAPI* set_status_callback)(HINTERNET, WINHTTP_STATUS_CALLBACK, DWORD, DWORD_PTR){};
    BOOL(WINAPI* add_request_headers)(HINTERNET, LPCWSTR, DWORD, DWORD){};
    BOOL(WINAPI* send_request)(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR){};
    BOOL(WINAPI* receive_response)(HINTERNET, LPVOID){};
    BOOL(WINAPI* query_headers)(HINTERNET, DWORD, LPCWSTR, LPVOID, LPDWORD, LPDWORD){};
    BOOL(WINAPI* read_data)(HINTERNET, LPVOID, DWORD, LPDWORD){};

    // Optional crypt32 for custom ca_cert_pem verification
    HMODULE crypt_mod{};
    BOOL(WINAPI* crypt_string_to_binary)(LPCSTR, DWORD, DWORD, BYTE*, DWORD*, DWORD*, DWORD*){};
    HCERTSTORE(WINAPI* cert_open_store)(LPCSTR, DWORD, HCRYPTPROV_LEGACY, DWORD, const void*){};
    BOOL(WINAPI* cert_close_store)(HCERTSTORE, DWORD){};
    BOOL(WINAPI* cert_add_encoded)(HCERTSTORE, DWORD, const BYTE*, DWORD, DWORD, PCCERT_CONTEXT*){};
    BOOL(WINAPI* cert_free_context)(PCCERT_CONTEXT){};
    BOOL(WINAPI* cert_create_engine)(PCERT_CHAIN_ENGINE_CONFIG, HCERTCHAINENGINE*){};
    VOID(WINAPI* cert_free_engine)(HCERTCHAINENGINE){};
    BOOL(WINAPI* cert_get_chain)(HCERTCHAINENGINE, PCCERT_CONTEXT, LPFILETIME, HCERTSTORE, PCERT_CHAIN_PARA, DWORD, LPVOID, PCCERT_CHAIN_CONTEXT*){};
    VOID(WINAPI* cert_free_chain)(PCCERT_CHAIN_CONTEXT){};
    BOOL(WINAPI* cert_verify_policy)(LPCSTR, PCCERT_CHAIN_CONTEXT, PCERT_CHAIN_POLICY_PARA, PCERT_CHAIN_POLICY_STATUS){};
};

WinHttpApi& winhttp_api() {
    static WinHttpApi api = []() {
        WinHttpApi a{};
        a.mod = LoadLibraryExW(L"winhttp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!a.mod) return a;
#define LOAD(field, sym) a.field = reinterpret_cast<decltype(a.field)>(GetProcAddress(a.mod, sym)); if (!a.field) return a;
        LOAD(open, "WinHttpOpen");
        LOAD(close_handle, "WinHttpCloseHandle");
        LOAD(connect, "WinHttpConnect");
        LOAD(open_request, "WinHttpOpenRequest");
        LOAD(set_timeouts, "WinHttpSetTimeouts");
        LOAD(set_option, "WinHttpSetOption");
        LOAD(query_option, "WinHttpQueryOption");
        LOAD(set_status_callback, "WinHttpSetStatusCallback");
        LOAD(add_request_headers, "WinHttpAddRequestHeaders");
        LOAD(send_request, "WinHttpSendRequest");
        LOAD(receive_response, "WinHttpReceiveResponse");
        LOAD(query_headers, "WinHttpQueryHeaders");
        LOAD(read_data, "WinHttpReadData");
#undef LOAD
        a.loaded = true;
        a.crypt_mod = LoadLibraryExW(L"crypt32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (a.crypt_mod) {
#define LOAD_C(field, sym) a.field = reinterpret_cast<decltype(a.field)>(GetProcAddress(a.crypt_mod, sym));
            LOAD_C(crypt_string_to_binary, "CryptStringToBinaryA");
            LOAD_C(cert_open_store, "CertOpenStore");
            LOAD_C(cert_close_store, "CertCloseStore");
            LOAD_C(cert_add_encoded, "CertAddEncodedCertificateToStore");
            LOAD_C(cert_free_context, "CertFreeCertificateContext");
            LOAD_C(cert_create_engine, "CertCreateCertificateChainEngine");
            LOAD_C(cert_free_engine, "CertFreeCertificateChainEngine");
            LOAD_C(cert_get_chain, "CertGetCertificateChain");
            LOAD_C(cert_free_chain, "CertFreeCertificateChain");
            LOAD_C(cert_verify_policy, "CertVerifyCertificateChainPolicy");
#undef LOAD_C
        }
        return a;
    }();
    return api;
}

StreamError map_winhttp_error(DWORD err, std::string fallback) {
    const char* code = "connect";
    switch (err) {
        case ERROR_WINHTTP_NAME_NOT_RESOLVED:
            code = "dns";
            break;
        case ERROR_WINHTTP_TIMEOUT:
            code = "timeout";
            break;
        case ERROR_WINHTTP_OPERATION_CANCELLED:
            code = "canceled";
            break;
        case ERROR_WINHTTP_SECURE_FAILURE:
        case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
        case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
        case ERROR_WINHTTP_SECURE_INVALID_CA:
        case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
        case ERROR_WINHTTP_SECURE_CHANNEL_ERROR:
        case ERROR_WINHTTP_SECURE_INVALID_CERT:
        case ERROR_WINHTTP_SECURE_CERT_REVOKED:
            code = "tls";
            break;
        case ERROR_WINHTTP_INVALID_SERVER_RESPONSE:
        case ERROR_WINHTTP_HEADER_NOT_FOUND:
            code = "protocol";
            break;
        default:
            code = "connect";
            break;
    }
    return StreamError{"http", code, std::move(fallback), "winhttp", static_cast<std::int64_t>(err)};
}

bool verify_server_cert_against_pem(WinHttpApi& api, HINTERNET req, const std::string& pem_path,
                                    const std::wstring& server_host, std::string& err_out) {
    if (!api.crypt_string_to_binary || !api.cert_open_store || !api.cert_add_encoded ||
        !api.cert_create_engine || !api.cert_get_chain || !api.cert_verify_policy) {
        err_out = "crypt32 API unavailable for ca_cert_pem verification";
        return false;
    }
    std::ifstream in(std::filesystem::path(std::u8string(pem_path.begin(), pem_path.end())), std::ios::binary);
    if (!in) {
        err_out = "cannot open ca_cert_pem file";
        return false;
    }
    std::string pem((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    HCERTSTORE store = api.cert_open_store(CERT_STORE_PROV_MEMORY, 0, 0, CERT_STORE_CREATE_NEW_FLAG, nullptr);
    if (!store) {
        err_out = "cannot create memory certificate store";
        return false;
    }
    struct StoreGuard {
        WinHttpApi& a;
        HCERTSTORE s;
        ~StoreGuard() { if (s) a.cert_close_store(s, 0); }
    } sg{api, store};

    std::size_t added = 0;
    const std::string begin_mark = "-----BEGIN CERTIFICATE-----";
    const std::string end_mark = "-----END CERTIFICATE-----";
    std::size_t pos = 0;
    while ((pos = pem.find(begin_mark, pos)) != std::string::npos) {
        auto end = pem.find(end_mark, pos + begin_mark.size());
        if (end == std::string::npos) break;
        std::string block = pem.substr(pos, (end + end_mark.size()) - pos);
        pos = end + end_mark.size();
        DWORD der_len = 0;
        if (!api.crypt_string_to_binary(block.c_str(), static_cast<DWORD>(block.size()),
                                        CRYPT_STRING_BASE64HEADER, nullptr, &der_len, nullptr, nullptr) || der_len == 0) {
            continue;
        }
        std::vector<BYTE> der(der_len);
        if (!api.crypt_string_to_binary(block.c_str(), static_cast<DWORD>(block.size()),
                                        CRYPT_STRING_BASE64HEADER, der.data(), &der_len, nullptr, nullptr)) {
            continue;
        }
        if (api.cert_add_encoded(store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, der.data(), der_len,
                                 CERT_STORE_ADD_REPLACE_EXISTING, nullptr)) {
            ++added;
        }
    }
    if (added == 0) {
        err_out = "ca_cert_pem contains no valid X.509 certificates";
        return false;
    }
    PCCERT_CONTEXT server_cert = nullptr;
    DWORD sz = sizeof(server_cert);
    if (!api.query_option(req, WINHTTP_OPTION_SERVER_CERT_CONTEXT, &server_cert, &sz) || !server_cert) {
        err_out = "failed to retrieve TLS server certificate";
        return false;
    }
    struct CertGuard {
        WinHttpApi& a;
        PCCERT_CONTEXT c;
        ~CertGuard() { if (c) a.cert_free_context(c); }
    } cg{api, server_cert};

    CERT_CHAIN_ENGINE_CONFIG cfg{};
    cfg.cbSize = sizeof(cfg);
    cfg.hExclusiveRoot = store;
    HCERTCHAINENGINE engine = nullptr;
    if (!api.cert_create_engine(&cfg, &engine) || !engine) {
        err_out = "failed to create certificate chain engine";
        return false;
    }
    struct EngGuard {
        WinHttpApi& a;
        HCERTCHAINENGINE e;
        ~EngGuard() { if (e) a.cert_free_engine(e); }
    } eg{api, engine};

    CERT_CHAIN_PARA chain_para{};
    chain_para.cbSize = sizeof(chain_para);
    PCCERT_CHAIN_CONTEXT chain = nullptr;
    if (!api.cert_get_chain(engine, server_cert, nullptr, server_cert->hCertStore, &chain_para, 0, nullptr, &chain) || !chain) {
        err_out = "failed to build TLS certificate chain";
        return false;
    }
    struct ChainGuard {
        WinHttpApi& a;
        PCCERT_CHAIN_CONTEXT ch;
        ~ChainGuard() { if (ch) a.cert_free_chain(ch); }
    } chg{api, chain};

    SSL_EXTRA_CERT_CHAIN_POLICY_PARA ssl_para{};
    ssl_para.cbSize = sizeof(ssl_para);
    ssl_para.dwAuthType = AUTHTYPE_SERVER;
    ssl_para.pwszServerName = const_cast<LPWSTR>(server_host.c_str());
    CERT_CHAIN_POLICY_PARA policy_para{};
    policy_para.cbSize = sizeof(policy_para);
    policy_para.pvExtraPolicyPara = &ssl_para;
    CERT_CHAIN_POLICY_STATUS status{};
    status.cbSize = sizeof(status);
    if (!api.cert_verify_policy(CERT_CHAIN_POLICY_SSL, chain, &policy_para, &status) || status.dwError != 0) {
        err_out = "TLS server certificate failed verification against ca_cert_pem";
        return false;
    }
    return true;
}

struct WinHttpAsyncContext {
    std::mutex mutex;
    std::condition_variable cv;
    bool op_done{};
    DWORD win_err{};
    DWORD bytes_read{};
    bool handle_closed{};
    bool close_initiated{};
    HINTERNET session{};
    HINTERNET connect{};
    HINTERNET request{};

    static void CALLBACK status_callback(HINTERNET, DWORD_PTR ctx_ptr, DWORD status,
                                         LPVOID info, DWORD info_len) {
        auto* self = reinterpret_cast<WinHttpAsyncContext*>(ctx_ptr);
        if (!self) return;
        std::lock_guard<std::mutex> lock(self->mutex);
        switch (status) {
            case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
            case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
                self->op_done = true;
                self->cv.notify_all();
                break;
            case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
                self->bytes_read = info_len;
                self->op_done = true;
                self->cv.notify_all();
                break;
            case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
                if (info) {
                    self->win_err = reinterpret_cast<const WINHTTP_ASYNC_RESULT*>(info)->dwError;
                } else {
                    self->win_err = ERROR_WINHTTP_OPERATION_CANCELLED;
                }
                self->op_done = true;
                self->cv.notify_all();
                break;
            case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
                self->handle_closed = true;
                self->op_done = true;
                self->cv.notify_all();
                break;
            default:
                break;
        }
    }

    void reset_op() {
        std::lock_guard<std::mutex> lock(mutex);
        op_done = false;
        win_err = 0;
        bytes_read = 0;
    }

    void close_handles_and_retire(WinHttpApi& api) {
        HINTERNET req = nullptr;
        HINTERNET conn = nullptr;
        HINTERNET sess = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!close_initiated) {
                close_initiated = true;
                req = request;
                request = nullptr;
            }
            conn = connect;
            connect = nullptr;
            sess = session;
            session = nullptr;
        }
        if (req) {
            api.close_handle(req);
            std::unique_lock<std::mutex> lock(mutex);
            cv.wait(lock, [&] { return handle_closed; });
        }
        if (conn) api.close_handle(conn);
        if (sess) api.close_handle(sess);
    }

    // Returns 0 on success, or StreamError on timeout/cancel/winhttp error.
    std::optional<StreamError> wait_op(WinHttpApi& api, SteadyClock::time_point deadline,
                                       RuntimeContext* context, const char* stage) {
        std::unique_lock<std::mutex> lock(mutex);
        while (!op_done) {
            if (context && !context->cleanup && context->cancellation && context->cancellation->stopped()) {
                lock.unlock();
                close_handles_and_retire(api);
                return StreamError{"http", "canceled", std::string("HTTP ") + stage + " canceled", "", {}};
            }
            auto now = SteadyClock::now();
            if (now >= deadline) {
                lock.unlock();
                close_handles_and_retire(api);
                return StreamError{"http", "timeout", std::string("HTTP ") + stage + " timed out", "", {}};
            }
            auto wake = std::min(deadline, now + std::chrono::milliseconds(20));
            cv.wait_until(lock, wake);
        }
        DWORD err = win_err;
        if (err != 0) {
            lock.unlock();
            close_handles_and_retire(api);
            return map_winhttp_error(err, std::string("HTTP ") + stage + " failed");
        }
        return std::nullopt;
    }
};

class WinHttpStreamReader final : public StreamReaderBackend {
    std::shared_ptr<WinHttpAsyncContext> ctx_;
    SteadyClock::time_point total_deadline_;
    std::int64_t read_timeout_ms_;
    bool eof_{false};
public:
    WinHttpStreamReader(std::shared_ptr<WinHttpAsyncContext> ctx,
                        SteadyClock::time_point total_deadline,
                        std::int64_t read_timeout_ms)
        : ctx_(std::move(ctx)), total_deadline_(total_deadline), read_timeout_ms_(read_timeout_ms) {}
    ~WinHttpStreamReader() override { (void)close(); }

    StreamReadResult read(std::size_t max_bytes, RuntimeContext* context) override {
        if (eof_ || !ctx_) return {std::nullopt, std::nullopt};
        auto& api = winhttp_api();
        HINTERNET req = nullptr;
        {
            std::lock_guard<std::mutex> lock(ctx_->mutex);
            if (ctx_->close_initiated || !ctx_->request) {
                return {std::nullopt, StreamError{"http", "closed", "HTTP stream is closed", "", {}}};
            }
            req = ctx_->request;
        }
        if (context && !context->cleanup && context->cancellation && context->cancellation->stopped()) {
            ctx_->close_handles_and_retire(api);
            return {std::nullopt, StreamError{"http", "canceled", "HTTP stream read canceled", "", {}}};
        }
        std::size_t want = std::min<std::size_t>(max_bytes, 64 * 1024);
        std::string buf(want, '\0');
        ctx_->reset_op();
        if (!api.read_data(req, buf.data(), static_cast<DWORD>(want), nullptr)) {
            DWORD err = GetLastError();
            ctx_->close_handles_and_retire(api);
            return {std::nullopt, map_winhttp_error(err, "HTTP stream read failed")};
        }
        auto read_dl = SteadyClock::now() + std::chrono::milliseconds(std::max<std::int64_t>(1, read_timeout_ms_));
        auto dl = std::min(total_deadline_, read_dl);
        if (auto err = ctx_->wait_op(api, dl, context, "read")) {
            return {std::nullopt, std::move(*err)};
        }
        DWORD got = 0;
        {
            std::lock_guard<std::mutex> lock(ctx_->mutex);
            got = ctx_->bytes_read;
        }
        if (got == 0) {
            eof_ = true;
            ctx_->close_handles_and_retire(api);
            return {std::nullopt, std::nullopt};
        }
        buf.resize(got);
        return {std::move(buf), std::nullopt};
    }

    std::optional<StreamError> close() override {
        if (ctx_) {
            ctx_->close_handles_and_retire(winhttp_api());
            ctx_.reset();
        }
        eof_ = true;
        return std::nullopt;
    }
};

std::map<std::string, std::string> parse_raw_headers(WinHttpApi& api, HINTERNET req) {
    std::map<std::string, std::string> out;
    DWORD bytes = 0;
    api.query_headers(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                      nullptr, &bytes, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0) return out;
    std::wstring raw(bytes / sizeof(wchar_t), L'\0');
    if (!api.query_headers(req, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                           raw.data(), &bytes, WINHTTP_NO_HEADER_INDEX)) {
        return out;
    }
    std::string utf8 = wide_to_utf8(raw);
    std::istringstream iss(utf8);
    std::string line;
    bool first = true;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (first) {
            first = false;
            continue; // status line
        }
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = to_lower_ascii(std::string_view(line).substr(0, colon));
        std::string val = line.substr(colon + 1);
        std::size_t start = val.find_first_not_of(" \t");
        val = (start == std::string::npos) ? "" : val.substr(start);
        auto it = out.find(key);
        if (it == out.end()) out.emplace(std::move(key), std::move(val));
        else it->second += ", " + val;
    }
    return out;
}

class WinHttpBackend final : public HttpBackend {
public:
    HttpOpenStreamResult open_stream(const HttpRequest& request, RuntimeContext* context) override {
        auto& api = winhttp_api();
        if (!api.loaded) {
            return {{}, StreamError{"http", "connect", "winhttp.dll unavailable", "", {}}};
        }
        if (request.method.empty() || has_crlf(request.method)) {
            return {{}, StreamError{"http", "invalid_argument", "invalid HTTP method", "", {}}};
        }
        for (const auto& [k, v] : request.headers) {
            if (k.empty() || has_crlf(k) || has_crlf(v)) {
                return {{}, StreamError{"http", "invalid_argument", "HTTP header contains invalid characters or CRLF", "", {}}};
            }
        }
        auto total_deadline = SteadyClock::now() + std::chrono::milliseconds(std::max<std::int64_t>(1, request.options.total_deadline_ms));
        std::string cur_url = request.url;
        std::string cur_method = request.method;
        std::string cur_body = request.body;
        auto cur_headers = request.headers;
        std::int64_t redirects = 0;

        for (;;) {
            if (context && !context->cleanup && context->cancellation && context->cancellation->stopped()) {
                return {{}, StreamError{"http", "canceled", "HTTP request canceled", "", {}}};
            }
            std::string url_err;
            auto parsed = parse_url_strict(cur_url, url_err);
            if (!parsed) {
                return {{}, StreamError{"http", "invalid_argument", url_err, "", {}}};
            }
            auto ctx = std::make_shared<WinHttpAsyncContext>();
            ctx->session = api.open(L"HuaHTTP/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
            if (!ctx->session) {
                return {{}, map_winhttp_error(GetLastError(), "WinHttpOpen failed")};
            }
            int conn_ms = static_cast<int>(std::min<std::int64_t>(request.options.connect_timeout_ms, 300000));
            int read_ms = static_cast<int>(std::min<std::int64_t>(request.options.read_timeout_ms, 300000));
            api.set_timeouts(ctx->session, conn_ms, conn_ms, read_ms, read_ms);

            std::wstring whost = utf8_to_wide(parsed->host);
            ctx->connect = api.connect(ctx->session, whost.c_str(), parsed->port, 0);
            if (!ctx->connect) {
                DWORD e = GetLastError();
                ctx->close_handles_and_retire(api);
                return {{}, map_winhttp_error(e, "WinHttpConnect failed")};
            }
            std::string target = parsed->path;
            if (!parsed->query.empty()) target += "?" + parsed->query;
            std::wstring wmethod = utf8_to_wide(cur_method);
            std::wstring wtarget = utf8_to_wide(target);
            DWORD req_flags = (parsed->scheme == "https") ? WINHTTP_FLAG_SECURE : 0;
            ctx->request = api.open_request(ctx->connect, wmethod.c_str(), wtarget.c_str(),
                                            nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, req_flags);
            if (!ctx->request) {
                DWORD e = GetLastError();
                ctx->close_handles_and_retire(api);
                return {{}, map_winhttp_error(e, "WinHttpOpenRequest failed")};
            }
            // Disable automatic redirects so we enforce max_redirects and strip auth headers cross-origin.
            DWORD disable_redir = WINHTTP_DISABLE_REDIRECTS;
            api.set_option(ctx->request, WINHTTP_OPTION_DISABLE_FEATURE, &disable_redir, sizeof(disable_redir));

            if (parsed->scheme == "https") {
                if (!request.options.tls_verify) {
                    DWORD sec_flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                                      SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
                    api.set_option(ctx->request, WINHTTP_OPTION_SECURITY_FLAGS, &sec_flags, sizeof(sec_flags));
                } else if (!request.options.ca_cert_pem.empty()) {
                    // Allow unknown system CA during handshake, then strictly verify against ca_cert_pem below.
                    DWORD sec_flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA;
                    api.set_option(ctx->request, WINHTTP_OPTION_SECURITY_FLAGS, &sec_flags, sizeof(sec_flags));
                }
            }

            api.set_status_callback(ctx->request, &WinHttpAsyncContext::status_callback,
                                    WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES, 0);

            for (const auto& [k, v] : cur_headers) {
                std::wstring hdr = utf8_to_wide(k + ": " + v + "\r\n");
                api.add_request_headers(ctx->request, hdr.c_str(), static_cast<DWORD>(hdr.size()),
                                        WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
            }

            ctx->reset_op();
            LPVOID body_ptr = cur_body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(cur_body.data());
            DWORD body_len = static_cast<DWORD>(cur_body.size());
            if (!api.send_request(ctx->request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                  body_ptr, body_len, body_len, reinterpret_cast<DWORD_PTR>(ctx.get()))) {
                DWORD e = GetLastError();
                ctx->close_handles_and_retire(api);
                return {{}, map_winhttp_error(e, "WinHttpSendRequest failed")};
            }
            auto send_dl = std::min(total_deadline, SteadyClock::now() + std::chrono::milliseconds(std::max<std::int64_t>(1, request.options.connect_timeout_ms)));
            if (auto err = ctx->wait_op(api, send_dl, context, "send")) {
                return {{}, std::move(*err)};
            }

            ctx->reset_op();
            if (!api.receive_response(ctx->request, nullptr)) {
                DWORD e = GetLastError();
                ctx->close_handles_and_retire(api);
                return {{}, map_winhttp_error(e, "WinHttpReceiveResponse failed")};
            }
            auto hdr_dl = std::min(total_deadline, SteadyClock::now() + std::chrono::milliseconds(std::max<std::int64_t>(1, request.options.read_timeout_ms)));
            if (auto err = ctx->wait_op(api, hdr_dl, context, "response")) {
                return {{}, std::move(*err)};
            }

            if (parsed->scheme == "https" && request.options.tls_verify && !request.options.ca_cert_pem.empty()) {
                std::string cert_err;
                if (!verify_server_cert_against_pem(api, ctx->request, request.options.ca_cert_pem, whost, cert_err)) {
                    ctx->close_handles_and_retire(api);
                    return {{}, StreamError{"http", "tls", cert_err, "winhttp", {}}};
                }
            }

            DWORD status_code = 0;
            DWORD status_sz = sizeof(status_code);
            if (!api.query_headers(ctx->request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                   WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &status_sz, WINHTTP_NO_HEADER_INDEX)) {
                DWORD e = GetLastError();
                ctx->close_handles_and_retire(api);
                return {{}, map_winhttp_error(e, "failed to read HTTP status code")};
            }
            auto resp_headers = parse_raw_headers(api, ctx->request);

            bool is_redir = (status_code == 301 || status_code == 302 || status_code == 303 ||
                             status_code == 307 || status_code == 308);
            auto loc_it = resp_headers.find("location");
            if (is_redir && loc_it != resp_headers.end() && !loc_it->second.empty() && request.options.max_redirects > 0) {
                if (redirects >= request.options.max_redirects) {
                    ctx->close_handles_and_retire(api);
                    return {{}, StreamError{"http", "too_many_redirects", "exceeded max_redirects limit", "", {}}};
                }
                std::string next_url = resolve_redirect_url(*parsed, loc_it->second);
                std::string next_err;
                auto next_parsed = parse_url_strict(next_url, next_err);
                if (!next_parsed) {
                    ctx->close_handles_and_retire(api);
                    return {{}, StreamError{"http", "protocol", "invalid redirect Location URL: " + next_err, "", {}}};
                }
                if (url_origin(*next_parsed) != url_origin(*parsed)) {
                    std::vector<std::pair<std::string, std::string>> filtered;
                    for (const auto& kv : cur_headers) {
                        auto lk = to_lower_ascii(kv.first);
                        if (lk == "authorization" || lk == "cookie" || lk == "proxy-authorization") continue;
                        filtered.push_back(kv);
                    }
                    cur_headers = std::move(filtered);
                }
                if (status_code == 303 || ((status_code == 301 || status_code == 302) && cur_method == "POST")) {
                    cur_method = "GET";
                    cur_body.clear();
                }
                ctx->close_handles_and_retire(api);
                cur_url = std::move(next_url);
                ++redirects;
                continue;
            }

            auto reader_backend = std::make_shared<WinHttpStreamReader>(ctx, total_deadline, request.options.read_timeout_ms);
            auto stream_data = std::make_shared<HttpStreamData>();
            stream_data->status = static_cast<std::int64_t>(status_code);
            stream_data->headers = std::move(resp_headers);
            stream_data->reader = std::make_shared<StreamReaderData>(std::move(reader_backend));
            return {std::move(stream_data), std::nullopt};
        }
    }

    HttpFetchResult fetch(const HttpRequest& request, RuntimeContext* context) override {
        auto opened = open_stream(request, context);
        if (opened.error) return {{}, std::move(opened.error)};
        auto& st = *opened.stream;
        std::size_t limit = static_cast<std::size_t>(std::max<std::int64_t>(0, request.options.max_response_bytes));
        std::string body;
        for (;;) {
            auto rd = st.reader->read(16384, context);
            if (rd.error) {
                (void)st.reader->close();
                return {{}, std::move(rd.error)};
            }
            if (!rd.chunk || rd.chunk->empty()) break;
            if (body.size() + rd.chunk->size() > limit) {
                (void)st.reader->close();
                return {{}, StreamError{"http", "body_too_large", "HTTP response exceeded max_response_bytes", "", {}}};
            }
            body.append(*rd.chunk);
        }
        (void)st.reader->close();
        auto resp = std::make_shared<HttpResponseData>();
        resp->status = st.status;
        resp->headers = std::move(st.headers);
        resp->body = std::make_shared<const std::string>(std::move(body));
        return {std::move(resp), std::nullopt};
    }
};
#else
class UnsupportedHttpBackend final : public HttpBackend {
public:
    HttpOpenStreamResult open_stream(const HttpRequest&, RuntimeContext*) override {
        return {{}, StreamError{"http", "unsupported", "HTTP backend is not yet implemented on this platform", "", {}}};
    }
    HttpFetchResult fetch(const HttpRequest&, RuntimeContext*) override {
        return {{}, StreamError{"http", "unsupported", "HTTP backend is not yet implemented on this platform", "", {}}};
    }
};
#endif
} // namespace

HttpBackend& default_http_backend() {
#ifdef _WIN32
    static WinHttpBackend backend;
#else
    static UnsupportedHttpBackend backend;
#endif
    return backend;
}
} // namespace hua
