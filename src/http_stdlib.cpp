#include "hua/http_backend.hpp"
#include "hua/json.hpp"
#include <algorithm>
#include <cctype>
namespace hua {
namespace {
std::string lower_ascii(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}
Value ok_http(Value payload, std::string success_type) {
    return Value(ResultValue{true, result_box(std::move(payload)), std::move(success_type), "std.error.Value", true, false});
}
Value ok_http_void() {
    return Value(ResultValue{true, {}, "void", "std.error.Value", true, false});
}
Value make_headers_map(const std::map<std::string, std::string>& headers) {
    auto data = managed<MapData>();
    data->key_type = "string";
    data->item_type = "string";
    for (const auto& [k, v] : headers) {
        data->entries.emplace(MapKey(k), Value(v));
    }
    return Value(MapValue{std::move(data), true});
}
std::vector<std::pair<std::string, std::string>> read_headers_arg(const Value& arg, const SourceSpan& span) {
    auto* m = get_if<MapValue>(&arg.data);
    if (!m || !m->data) runtime_error(span, "HTTP headers must be map[string]string", "E4003");
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& [k, v] : m->data->entries) {
        auto* ks = std::get_if<std::string>(&k);
        auto* vs = string_data(&v.data);
        if (!ks || !vs) runtime_error(span, "HTTP headers must be map[string]string", "E4003");
        out.emplace_back(*ks, *vs);
    }
    return out;
}
bool parse_options_map(const Value& arg, HttpOptions& opt, std::string& err, const SourceSpan& span) {
    auto* m = get_if<MapValue>(&arg.data);
    if (!m || !m->data) runtime_error(span, "HTTP options must be map[string]Json", "E4003");
    for (const auto& [k, v] : m->data->entries) {
        auto* ks = std::get_if<std::string>(&k);
        auto* jv = get_if<JsonValue>(&v.data);
        if (!ks || !jv || !jv->data) runtime_error(span, "HTTP options must be map[string]Json", "E4003");
        const auto& jd = *jv->data;
        auto get_int = [&](std::int64_t& dst) -> bool {
            if (auto* iv = std::get_if<std::int64_t>(&jd.data)) { dst = *iv; return true; }
            err = "HTTP option `" + *ks + "` must be an integer";
            return false;
        };
        if (*ks == "connect_timeout_ms") { if (!get_int(opt.connect_timeout_ms) || opt.connect_timeout_ms <= 0) { if (err.empty()) err = "connect_timeout_ms must be > 0"; return false; } }
        else if (*ks == "read_timeout_ms") { if (!get_int(opt.read_timeout_ms) || opt.read_timeout_ms <= 0) { if (err.empty()) err = "read_timeout_ms must be > 0"; return false; } }
        else if (*ks == "total_deadline_ms") { if (!get_int(opt.total_deadline_ms) || opt.total_deadline_ms <= 0) { if (err.empty()) err = "total_deadline_ms must be > 0"; return false; } }
        else if (*ks == "max_response_bytes") { if (!get_int(opt.max_response_bytes) || opt.max_response_bytes < 0) { if (err.empty()) err = "max_response_bytes must be >= 0"; return false; } }
        else if (*ks == "max_redirects") { if (!get_int(opt.max_redirects) || opt.max_redirects < 0) { if (err.empty()) err = "max_redirects must be >= 0"; return false; } }
        else if (*ks == "tls_verify") {
            if (auto* bv = std::get_if<bool>(&jd.data)) opt.tls_verify = *bv;
            else { err = "HTTP option `tls_verify` must be bool"; return false; }
        } else if (*ks == "insecure_skip_verify") {
            if (auto* bv = std::get_if<bool>(&jd.data)) opt.tls_verify = !(*bv);
            else { err = "HTTP option `insecure_skip_verify` must be bool"; return false; }
        } else if (*ks == "ca_cert_pem") {
            if (auto* sv = std::get_if<std::string>(&jd.data)) opt.ca_cert_pem = *sv;
            else { err = "HTTP option `ca_cert_pem` must be string"; return false; }
        } else {
            err = "unknown HTTP option `" + *ks + "`";
            return false;
        }
    }
    return true;
}
} // namespace

Value invoke_http_standard(const StandardFunction& function, const std::vector<Value>& args,
                           const SourceSpan& span, RuntimeContext* context) {
    const auto& op = function.name;
    std::string origin = "std.http." + op;
    if (op == "get") {
        auto* u = string_data(&args.at(0).data);
        auto* t = get_if<std::int64_t>(&args.at(2).data);
        if (!u || !t) runtime_error(span, "std.http.get expects (string, map[string]string, int)", "E4003");
        if (*t <= 0) return stream_error_result({"http", "invalid_argument", "timeout_ms must be > 0", "", {}}, "std.http.Response", origin);
        HttpRequest req;
        req.method = "GET";
        req.url = *u;
        req.headers = read_headers_arg(args.at(1), span);
        req.options.connect_timeout_ms = std::min<std::int64_t>(*t, 10000);
        req.options.read_timeout_ms = *t;
        req.options.total_deadline_ms = *t;
        auto res = default_http_backend().fetch(req, context);
        if (res.error) return stream_error_result(*res.error, "std.http.Response", origin);
        return ok_http(Value(HttpResponseValue{std::move(res.response)}), "std.http.Response");
    }
    if (op == "post") {
        auto* u = string_data(&args.at(0).data);
        auto* b = get_if<BytesValue>(&args.at(2).data);
        auto* t = get_if<std::int64_t>(&args.at(3).data);
        if (!u || !b || !t) runtime_error(span, "std.http.post expects (string, map[string]string, Bytes, int)", "E4003");
        if (*t <= 0) return stream_error_result({"http", "invalid_argument", "timeout_ms must be > 0", "", {}}, "std.http.Response", origin);
        HttpRequest req;
        req.method = "POST";
        req.url = *u;
        req.headers = read_headers_arg(args.at(1), span);
        req.body = b->data ? *b->data : std::string{};
        req.options.connect_timeout_ms = std::min<std::int64_t>(*t, 10000);
        req.options.read_timeout_ms = *t;
        req.options.total_deadline_ms = *t;
        auto res = default_http_backend().fetch(req, context);
        if (res.error) return stream_error_result(*res.error, "std.http.Response", origin);
        return ok_http(Value(HttpResponseValue{std::move(res.response)}), "std.http.Response");
    }
    if (op == "request") {
        auto* m = string_data(&args.at(0).data);
        auto* u = string_data(&args.at(1).data);
        auto* b = get_if<BytesValue>(&args.at(3).data);
        if (!m || !u || !b) runtime_error(span, "std.http.request expects (string, string, map[string]string, Bytes, map[string]Json)", "E4003");
        HttpRequest req;
        req.method = *m;
        req.url = *u;
        req.headers = read_headers_arg(args.at(2), span);
        req.body = b->data ? *b->data : std::string{};
        std::string opt_err;
        if (!parse_options_map(args.at(4), req.options, opt_err, span)) {
            return stream_error_result({"http", "invalid_argument", opt_err, "", {}}, "std.http.Response", origin);
        }
        auto res = default_http_backend().fetch(req, context);
        if (res.error) return stream_error_result(*res.error, "std.http.Response", origin);
        return ok_http(Value(HttpResponseValue{std::move(res.response)}), "std.http.Response");
    }
    if (op == "status") {
        auto* r = get_if<HttpResponseValue>(&args.at(0).data);
        if (!r || !r->data) runtime_error(span, "std.http.status expects std.http.Response", "E4003");
        return Value(r->data->status);
    }
    if (op == "header") {
        auto* r = get_if<HttpResponseValue>(&args.at(0).data);
        auto* name = string_data(&args.at(1).data);
        if (!r || !r->data || !name) runtime_error(span, "std.http.header expects (Response, string)", "E4003");
        auto it = r->data->headers.find(lower_ascii(*name));
        if (it == r->data->headers.end()) return Value{};
        return Value(it->second);
    }
    if (op == "headers") {
        auto* r = get_if<HttpResponseValue>(&args.at(0).data);
        if (!r || !r->data) runtime_error(span, "std.http.headers expects std.http.Response", "E4003");
        return make_headers_map(r->data->headers);
    }
    if (op == "body") {
        auto* r = get_if<HttpResponseValue>(&args.at(0).data);
        if (!r || !r->data) runtime_error(span, "std.http.body expects std.http.Response", "E4003");
        auto copy = std::make_shared<std::string>(r->data->body ? *r->data->body : std::string{});
        return Value(BytesValue{std::move(copy)});
    }
    if (op == "text") {
        auto* r = get_if<HttpResponseValue>(&args.at(0).data);
        if (!r || !r->data) runtime_error(span, "std.http.text expects std.http.Response", "E4003");
        std::string s = r->data->body ? *r->data->body : std::string{};
        if (!text_utf8_error(s).empty()) {
            return stream_error_result({"http", "invalid_utf8", "HTTP response body is not valid UTF-8", "", {}}, "string", origin);
        }
        return ok_http(Value(std::move(s)), "string");
    }
    if (op == "open_stream") {
        auto* m = string_data(&args.at(0).data);
        auto* u = string_data(&args.at(1).data);
        auto* b = get_if<BytesValue>(&args.at(3).data);
        if (!m || !u || !b) runtime_error(span, "std.http.open_stream expects (string, string, map[string]string, Bytes, map[string]Json)", "E4003");
        HttpRequest req;
        req.method = *m;
        req.url = *u;
        req.headers = read_headers_arg(args.at(2), span);
        req.body = b->data ? *b->data : std::string{};
        std::string opt_err;
        if (!parse_options_map(args.at(4), req.options, opt_err, span)) {
            return stream_error_result({"http", "invalid_argument", opt_err, "", {}}, "std.http.StreamResponse", origin);
        }
        auto res = default_http_backend().open_stream(req, context);
        if (res.error) return stream_error_result(*res.error, "std.http.StreamResponse", origin);
        return ok_http(Value(HttpStreamValue{std::move(res.stream)}), "std.http.StreamResponse");
    }
    if (op == "stream_status") {
        auto* r = get_if<HttpStreamValue>(&args.at(0).data);
        if (!r || !r->data) runtime_error(span, "std.http.stream_status expects std.http.StreamResponse", "E4003");
        return Value(r->data->status);
    }
    if (op == "stream_header") {
        auto* r = get_if<HttpStreamValue>(&args.at(0).data);
        auto* name = string_data(&args.at(1).data);
        if (!r || !r->data || !name) runtime_error(span, "std.http.stream_header expects (StreamResponse, string)", "E4003");
        auto it = r->data->headers.find(lower_ascii(*name));
        if (it == r->data->headers.end()) return Value{};
        return Value(it->second);
    }
    if (op == "stream_headers") {
        auto* r = get_if<HttpStreamValue>(&args.at(0).data);
        if (!r || !r->data) runtime_error(span, "std.http.stream_headers expects std.http.StreamResponse", "E4003");
        return make_headers_map(r->data->headers);
    }
    if (op == "stream_body") {
        auto* r = get_if<HttpStreamValue>(&args.at(0).data);
        if (!r || !r->data) runtime_error(span, "std.http.stream_body expects std.http.StreamResponse", "E4003");
        return Value(ReaderValue{r->data->reader});
    }
    if (op == "stream_close") {
        auto* r = get_if<HttpStreamValue>(&args.at(0).data);
        if (!r || !r->data) runtime_error(span, "std.http.stream_close expects std.http.StreamResponse", "E4003");
        if (r->data->reader) {
            auto err = r->data->reader->close();
            if (err) return stream_error_result(*err, "void", origin);
        }
        return ok_http_void();
    }
    runtime_error(span, "unknown std.http function `" + op + "`", "E4001");
}
} // namespace hua
