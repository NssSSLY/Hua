#include "hua/http_backend.hpp"
#include <algorithm>
#include <cctype>
#include <sstream>
namespace hua {
namespace {
bool is_unreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
}
int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
std::string pct_encode(std::string_view s) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (is_unreserved(c)) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(kHex[(c >> 4) & 0xF]);
            out.push_back(kHex[c & 0xF]);
        }
    }
    return out;
}
std::optional<std::string> pct_decode(std::string_view s, std::string& err) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%') {
            if (i + 2 >= s.size()) {
                err = "incomplete percent-encoding sequence";
                return std::nullopt;
            }
            int hi = hex_val(s[i + 1]);
            int lo = hex_val(s[i + 2]);
            if (hi < 0 || lo < 0) {
                err = "invalid hex digits in percent-encoding";
                return std::nullopt;
            }
            out.push_back(static_cast<char>((hi << 4) | lo));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    if (!text_utf8_error(out).empty()) {
        err = "decoded URL component is not valid UTF-8";
        return std::nullopt;
    }
    return out;
}
Value ok_url(Value payload, std::string success_type) {
    return Value(ResultValue{true, result_box(std::move(payload)), std::move(success_type), "std.error.Value", true, false});
}
Value err_url(std::string code, std::string msg, std::string success_type, std::string origin) {
    return stream_error_result(StreamError{"url", std::move(code), std::move(msg), "", {}}, std::move(success_type), std::move(origin));
}
} // namespace

std::optional<ParsedUrl> parse_url_strict(std::string_view raw, std::string& err_msg) {
    if (raw.empty() || raw.size() > 65536) {
        err_msg = "URL is empty or exceeds maximum length";
        return std::nullopt;
    }
    for (unsigned char c : raw) {
        if (c < 0x20 || c == 0x7F) {
            err_msg = "URL contains control characters";
            return std::nullopt;
        }
    }
    auto sep = raw.find("://");
    if (sep == std::string_view::npos || sep == 0) {
        err_msg = "URL missing scheme://";
        return std::nullopt;
    }
    ParsedUrl out;
    out.scheme.assign(raw.substr(0, sep));
    for (char& ch : out.scheme) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (out.scheme != "http" && out.scheme != "https") {
        err_msg = "unsupported URL scheme (only http and https are supported)";
        return std::nullopt;
    }
    std::string_view rest = raw.substr(sep + 3);
    auto frag_pos = rest.find('#');
    if (frag_pos != std::string_view::npos) {
        out.fragment.assign(rest.substr(frag_pos + 1));
        rest = rest.substr(0, frag_pos);
    }
    auto query_pos = rest.find('?');
    if (query_pos != std::string_view::npos) {
        out.query.assign(rest.substr(query_pos + 1));
        rest = rest.substr(0, query_pos);
    }
    auto path_pos = rest.find('/');
    std::string_view authority = (path_pos == std::string_view::npos) ? rest : rest.substr(0, path_pos);
    out.path = (path_pos == std::string_view::npos) ? "/" : std::string(rest.substr(path_pos));
    if (authority.empty()) {
        err_msg = "URL missing host";
        return std::nullopt;
    }
    if (authority.find('@') != std::string_view::npos) {
        err_msg = "URL userinfo (user@host) is not allowed";
        return std::nullopt;
    }
    std::string_view host_part = authority;
    std::string_view port_part;
    if (authority.front() == '[') {
        auto close_br = authority.find(']');
        if (close_br == std::string_view::npos) {
            err_msg = "unclosed IPv6 literal in URL host";
            return std::nullopt;
        }
        host_part = authority.substr(1, close_br - 1);
        if (close_br + 1 < authority.size()) {
            if (authority[close_br + 1] != ':') {
                err_msg = "unexpected characters after IPv6 host";
                return std::nullopt;
            }
            port_part = authority.substr(close_br + 2);
        }
    } else {
        auto colon = authority.rfind(':');
        if (colon != std::string_view::npos) {
            host_part = authority.substr(0, colon);
            port_part = authority.substr(colon + 1);
        }
    }
    if (host_part.empty()) {
        err_msg = "URL host is empty";
        return std::nullopt;
    }
    for (unsigned char c : host_part) {
        if (c == ' ' || c == '\t' || c == '\\') {
            err_msg = "URL host contains invalid characters";
            return std::nullopt;
        }
    }
    out.host.assign(host_part);
    for (char& ch : out.host) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (port_part.empty()) {
        out.port = (out.scheme == "https") ? 443 : 80;
    } else {
        std::uint64_t val = 0;
        for (char c : port_part) {
            if (c < '0' || c > '9') {
                err_msg = "invalid URL port";
                return std::nullopt;
            }
            val = val * 10 + static_cast<std::uint64_t>(c - '0');
            if (val > 65535) {
                err_msg = "URL port out of range";
                return std::nullopt;
            }
        }
        if (val == 0) {
            err_msg = "URL port must be 1..65535";
            return std::nullopt;
        }
        out.port = static_cast<std::uint16_t>(val);
    }
    return out;
}

std::string url_origin(const ParsedUrl& u) {
    return u.scheme + "://" + u.host + ":" + std::to_string(u.port);
}

Value invoke_url_standard(const StandardFunction& function, const std::vector<Value>& args,
                          const SourceSpan& span) {
    const auto& op = function.name;
    std::string origin = "std.url." + op;
    if (op == "parse") {
        auto* raw = string_data(&args.at(0).data);
        if (!raw) runtime_error(span, "std.url.parse expects string", "E4003");
        std::string err;
        auto parsed = parse_url_strict(*raw, err);
        if (!parsed) return err_url("invalid_url", err, "map[string]string", origin);
        auto data = managed<MapData>();
        data->key_type = "string";
        data->item_type = "string";
        data->entries.emplace(MapKey(std::string("scheme")), Value(parsed->scheme));
        data->entries.emplace(MapKey(std::string("host")), Value(parsed->host));
        data->entries.emplace(MapKey(std::string("port")), Value(std::to_string(parsed->port)));
        data->entries.emplace(MapKey(std::string("path")), Value(parsed->path));
        data->entries.emplace(MapKey(std::string("query")), Value(parsed->query));
        data->entries.emplace(MapKey(std::string("fragment")), Value(parsed->fragment));
        return ok_url(Value(MapValue{std::move(data), true}), "map[string]string");
    }
    if (op == "encode") {
        auto* raw = string_data(&args.at(0).data);
        if (!raw) runtime_error(span, "std.url.encode expects string", "E4003");
        return Value(pct_encode(*raw));
    }
    if (op == "decode") {
        auto* raw = string_data(&args.at(0).data);
        if (!raw) runtime_error(span, "std.url.decode expects string", "E4003");
        std::string err;
        auto decoded = pct_decode(*raw, err);
        if (!decoded) return err_url("invalid_encoding", err, "string", origin);
        return ok_url(Value(std::move(*decoded)), "string");
    }
    if (op == "build_query") {
        auto* m = get_if<MapValue>(&args.at(0).data);
        if (!m || !m->data) runtime_error(span, "std.url.build_query expects map[string]string", "E4003");
        std::vector<std::pair<std::string, std::string>> pairs;
        for (const auto& [k, v] : m->data->entries) {
            auto* ks = std::get_if<std::string>(&k);
            auto* vs = string_data(&v.data);
            if (!ks || !vs) runtime_error(span, "std.url.build_query expects map[string]string", "E4003");
            pairs.emplace_back(*ks, *vs);
        }
        std::sort(pairs.begin(), pairs.end());
        std::string out;
        for (std::size_t i = 0; i < pairs.size(); ++i) {
            if (i > 0) out.push_back('&');
            out += pct_encode(pairs[i].first);
            out.push_back('=');
            out += pct_encode(pairs[i].second);
        }
        return Value(std::move(out));
    }
    runtime_error(span, "unknown std.url function `" + op + "`", "E4001");
}
} // namespace hua
