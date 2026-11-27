#pragma once
#include "hua/stream.hpp"
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
namespace hua {
struct ParsedUrl {
    std::string scheme;
    std::string host;
    std::uint16_t port{};
    std::string path;
    std::string query;
    std::string fragment;
};
std::optional<ParsedUrl> parse_url_strict(std::string_view raw, std::string& err_msg);
std::string url_origin(const ParsedUrl& u);

struct HttpOptions {
    std::int64_t connect_timeout_ms{10000};
    std::int64_t read_timeout_ms{30000};
    std::int64_t total_deadline_ms{60000};
    std::int64_t max_response_bytes{16 * 1024 * 1024};
    std::int64_t max_redirects{5};
    bool tls_verify{true};
    std::string ca_cert_pem;
};
struct HttpRequest {
    std::string method;
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    HttpOptions options;
};
struct HttpResponseData {
    std::int64_t status{};
    std::map<std::string, std::string> headers; // lowercase keys
    std::shared_ptr<const std::string> body;
};
struct HttpStreamData {
    std::int64_t status{};
    std::map<std::string, std::string> headers; // lowercase keys
    std::shared_ptr<StreamReaderData> reader;
};
struct HttpOpenStreamResult {
    std::shared_ptr<HttpStreamData> stream;
    std::optional<StreamError> error;
};
struct HttpFetchResult {
    std::shared_ptr<const HttpResponseData> response;
    std::optional<StreamError> error;
};
class HttpBackend {
public:
    virtual ~HttpBackend() = default;
    virtual HttpOpenStreamResult open_stream(const HttpRequest& request, RuntimeContext* context) = 0;
    virtual HttpFetchResult fetch(const HttpRequest& request, RuntimeContext* context) = 0;
};
HttpBackend& default_http_backend();
Value invoke_url_standard(const StandardFunction& function, const std::vector<Value>& arguments,
                          const SourceSpan& span);
Value invoke_http_standard(const StandardFunction& function, const std::vector<Value>& arguments,
                           const SourceSpan& span, RuntimeContext* context);
} // namespace hua
