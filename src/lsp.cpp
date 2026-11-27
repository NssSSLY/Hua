#include "hua/lsp.hpp"
#include "hua/ast.hpp"
#include "hua/json.hpp"
#include "hua/lexer.hpp"
#include "hua/module.hpp"
#include "hua/parser.hpp"
#include "hua/sema.hpp"
#include "hua/source.hpp"
#include "hua/stdlib.hpp"
#include "hua/token.hpp"
#include "hua/value.hpp"
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace hua {
namespace {

using K = TokenKind;
using N = NodeKind;

// --- JSON builder (zero external dependencies) ---

std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                static constexpr char hex[] = "0123456789abcdef";
                out += "\\u00";
                out.push_back(hex[c >> 4]);
                out.push_back(hex[c & 0xF]);
            } else {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    out.push_back('"');
    return out;
}

struct JsonOut {
    std::string raw;
    static JsonOut null_val() { return {"null"}; }
    static JsonOut boolean(bool v) { return {v ? "true" : "false"}; }
    static JsonOut integer(std::int64_t v) { return {std::to_string(v)}; }
    static JsonOut u_int(std::size_t v) { return {std::to_string(v)}; }
    static JsonOut str(std::string_view s) { return {json_escape(s)}; }
    static JsonOut raw_json(std::string s) { return {std::move(s)}; }
    static JsonOut array(const std::vector<JsonOut>& items) {
        std::string out = "[";
        for (std::size_t i = 0; i < items.size(); ++i) {
            if (i) out.push_back(',');
            out += items[i].raw;
        }
        out.push_back(']');
        return {std::move(out)};
    }
    static JsonOut object(const std::vector<std::pair<std::string, JsonOut>>& fields) {
        std::string out = "{";
        for (std::size_t i = 0; i < fields.size(); ++i) {
            if (i) out.push_back(',');
            out += json_escape(fields[i].first);
            out.push_back(':');
            out += fields[i].second.raw;
        }
        out.push_back('}');
        return {std::move(out)};
    }
};

// --- JsonData helpers ---

JsonData::Pointer j_get(const JsonData::Pointer& obj, const std::string& key) {
    if (!obj) return {};
    const auto* map = std::get_if<JsonData::Object>(&obj->data);
    if (!map) return {};
    auto it = map->find(key);
    return it == map->end() ? nullptr : it->second;
}

std::optional<std::string> j_str(const JsonData::Pointer& val) {
    if (!val) return std::nullopt;
    if (const auto* s = std::get_if<std::string>(&val->data)) return *s;
    return std::nullopt;
}

std::optional<std::int64_t> j_int(const JsonData::Pointer& val) {
    if (!val) return std::nullopt;
    if (const auto* i = std::get_if<std::int64_t>(&val->data)) return *i;
    if (const auto* d = std::get_if<double>(&val->data)) return static_cast<std::int64_t>(*d);
    return std::nullopt;
}

std::string j_id_raw(const JsonData::Pointer& val) {
    if (!val) return "null";
    if (const auto* i = std::get_if<std::int64_t>(&val->data)) return std::to_string(*i);
    if (const auto* d = std::get_if<double>(&val->data)) return std::to_string(static_cast<std::int64_t>(*d));
    if (const auto* s = std::get_if<std::string>(&val->data)) return json_escape(*s);
    return "null";
}

// --- URI <-> filesystem::path conversion ---

int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string url_decode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = hex_val(s[i + 1]);
            int lo = hex_val(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }
    return out;
}

std::string url_encode_path(std::string_view p) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(p.size() + 16);
    for (std::size_t i = 0; i < p.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(p[i]);
        if (c == '\\') {
            out.push_back('/');
        } else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                   c == '/' || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else if (c == ':' && i == 2 && p[0] == '/') {
            out += "%3A";
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

std::optional<std::filesystem::path> uri_to_path(std::string_view uri) {
    constexpr std::string_view prefix = "file://";
    if (!uri.starts_with(prefix)) return std::nullopt;
    std::string rest = url_decode(uri.substr(prefix.size()));
    if (rest.starts_with("localhost/")) rest = rest.substr(9);
#ifdef _WIN32
    if (rest.size() >= 3 && rest[0] == '/' && std::isalpha(static_cast<unsigned char>(rest[1])) && rest[2] == ':') {
        rest.erase(rest.begin());
    }
#endif
    return std::filesystem::path(std::u8string(rest.begin(), rest.end()));
}

std::string path_to_uri(const std::filesystem::path& path) {
    std::error_code ec;
    auto norm = std::filesystem::weakly_canonical(path, ec);
    const auto& use = ec ? path : norm;
    auto u8 = use.u8string();
    std::string s(u8.begin(), u8.end());
    std::replace(s.begin(), s.end(), '\\', '/');
    if (!s.empty() && s[0] != '/') s.insert(s.begin(), '/');
    return "file://" + url_encode_path(s);
}

// --- UTF-8 <-> UTF-16 position helpers ---

std::size_t utf8_prefix_to_utf16(std::string_view line, std::size_t byte_col) {
    byte_col = std::min(byte_col, line.size());
    std::size_t units = 0;
    std::size_t i = 0;
    while (i < byte_col) {
        unsigned char b = static_cast<unsigned char>(line[i]);
        if (b < 0x80) {
            i += 1;
            units += 1;
        } else if ((b & 0xE0) == 0xC0) {
            i += 2;
            units += 1;
        } else if ((b & 0xF0) == 0xE0) {
            i += 3;
            units += 1;
        } else if ((b & 0xF8) == 0xF0) {
            i += 4;
            units += 2;
        } else {
            i += 1;
            units += 1;
        }
    }
    return units;
}

std::size_t utf16_col_to_utf8_byte(std::string_view line, std::size_t utf16_col) {
    std::size_t units = 0;
    std::size_t i = 0;
    while (i < line.size() && units < utf16_col) {
        unsigned char b = static_cast<unsigned char>(line[i]);
        if (b < 0x80) {
            i += 1;
            units += 1;
        } else if ((b & 0xE0) == 0xC0) {
            i += 2;
            units += 1;
        } else if ((b & 0xF0) == 0xE0) {
            i += 3;
            units += 1;
        } else if ((b & 0xF8) == 0xF0) {
            i += 4;
            units += 2;
        } else {
            i += 1;
            units += 1;
        }
    }
    return std::min(i, line.size());
}

struct LspPos {
    std::size_t line{};
    std::size_t character{};
};

struct LspRange {
    LspPos start;
    LspPos end;
};

LspPos offset_to_lsp_pos(const Source& source, std::size_t offset) {
    auto sp = source.span(offset, offset);
    std::size_t line0 = sp.line > 0 ? sp.line - 1 : 0;
    std::size_t byte_col = sp.column > 0 ? sp.column - 1 : 0;
    auto line_str = source.line_text(sp.line);
    return {line0, utf8_prefix_to_utf16(line_str, byte_col)};
}

LspRange span_to_lsp_range(const Source& source, const SourceSpan& span) {
    auto start = offset_to_lsp_pos(source, span.start_offset);
    std::size_t end_off = span.end_offset > span.start_offset ? span.end_offset : span.start_offset + 1;
    end_off = std::min(end_off, source.text().size());
    auto end = offset_to_lsp_pos(source, end_off);
    if (end.line == start.line && end.character <= start.character) {
        end.character = start.character + 1;
    }
    return {start, end};
}

JsonOut lsp_range_json(const LspRange& r) {
    return JsonOut::object({
        {"start", JsonOut::object({{"line", JsonOut::u_int(r.start.line)}, {"character", JsonOut::u_int(r.start.character)}})},
        {"end", JsonOut::object({{"line", JsonOut::u_int(r.end.line)}, {"character", JsonOut::u_int(r.end.character)}})},
    });
}

// --- Semantic token legend ---

enum SemType : std::uint32_t {
    ST_Namespace = 0,
    ST_Type = 1,
    ST_Class = 2,
    ST_Enum = 3,
    ST_Interface = 4,
    ST_Struct = 5,
    ST_TypeParameter = 6,
    ST_Parameter = 7,
    ST_Variable = 8,
    ST_Property = 9,
    ST_EnumMember = 10,
    ST_Function = 11,
    ST_Method = 12,
    ST_Keyword = 13,
    ST_Comment = 14,
    ST_String = 15,
    ST_Number = 16,
    ST_Operator = 17,
};

enum SemMod : std::uint32_t {
    SM_None = 0,
    SM_Declaration = 1u << 0,
    SM_Definition = 1u << 1,
    SM_Readonly = 1u << 2,
    SM_Static = 1u << 3,
    SM_Deprecated = 1u << 4,
    SM_DefaultLibrary = 1u << 5,
    SM_Modification = 1u << 6,
};

const std::vector<std::string>& semantic_token_types() {
    static const std::vector<std::string> types = {
        "namespace", "type", "class", "enum", "interface", "struct",
        "typeParameter", "parameter", "variable", "property", "enumMember",
        "function", "method", "keyword", "comment", "string", "number", "operator"
    };
    return types;
}

const std::vector<std::string>& semantic_token_modifiers() {
    static const std::vector<std::string> mods = {
        "declaration", "definition", "readonly", "static", "deprecated", "defaultLibrary", "modification"
    };
    return mods;
}

const std::vector<std::string_view>& builtin_type_list() {
    static const std::vector<std::string_view> list = {
        "int", "float", "bool", "string", "byte", "void",
        "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64",
        "f32", "f64", "usize", "isize",
        "List", "Map", "map", "Result", "Task", "Bytes", "Buffer", "Json", "Deque", "NativeHandle",
        "ptr", "ref"
    };
    return list;
}

const std::vector<std::string_view>& builtin_fn_list() {
    static const std::vector<std::string_view> list = {
        "print", "println", "len", "str", "int", "float", "bool", "byte",
        "Ok", "Err", "Some", "None", "append", "push", "pop", "clone", "panic", "assert"
    };
    return list;
}

const std::vector<std::string_view>& keyword_list() {
    static const std::vector<std::string_view> list = {
        "let", "var", "const", "fn", "return", "struct", "interface", "enum", "pub", "mut",
        "if", "else", "match", "for", "while", "in", "by", "break", "continue",
        "import", "as", "async", "await", "spawn", "taskgroup", "parallel", "simd",
        "defer", "unsafe", "true", "false", "nil"
    };
    return list;
}

bool is_builtin_type(std::string_view name) {
    static const std::unordered_set<std::string_view> builtins(builtin_type_list().begin(), builtin_type_list().end());
    return builtins.contains(name);
}

bool is_builtin_fn(std::string_view name) {
    static const std::unordered_set<std::string_view> builtins(builtin_fn_list().begin(), builtin_fn_list().end());
    return builtins.contains(name);
}

std::string decl_base_name(const Node& node) {
    auto sp = node.text.find(' ');
    return sp == std::string::npos ? node.text : node.text.substr(0, sp);
}

std::string strip_type_wrappers(std::string t) {
    if (t.starts_with("mut ")) t = t.substr(4);
    while (!t.empty() && t.back() == '?') t.pop_back();
    auto lt = t.find('<');
    if (lt != std::string::npos) t = t.substr(0, lt);
    // Strip ModuleLoader internal link prefix `$<id>$` (e.g. "$1$Item" -> "Item")
    if (t.size() > 2 && t[0] == '$') {
        auto second = t.find('$', 1);
        if (second != std::string::npos && second + 1 < t.size()) {
            t = t.substr(second + 1);
        }
    }
    return t;
}

std::string render_type_node(const Node& node) {
    switch (node.kind) {
    case N::TypeName:
        return node.text;
    case N::SliceType:
        return node.children.empty() ? "[]" : "[]" + render_type_node(*node.children[0]);
    case N::ArrayType:
        if (node.children.size() >= 2)
            return "[" + node.children[1]->text + "]" + render_type_node(*node.children[0]);
        return "[]";
    case N::OptionalType:
        return node.children.empty() ? "?" : render_type_node(*node.children[0]) + "?";
    case N::MutableType:
        return node.children.empty() ? "mut" : "mut " + render_type_node(*node.children[0]);
    case N::GenericType: {
        if (node.text == "map" && node.children.size() == 2) {
            return "map[" + render_type_node(*node.children[0]) + "]" + render_type_node(*node.children[1]);
        }
        std::string out = node.text + "<";
        for (std::size_t i = 0; i < node.children.size(); ++i) {
            if (i) out += ", ";
            out += render_type_node(*node.children[i]);
        }
        out += ">";
        return out;
    }
    default:
        return node.text;
    }
}

std::string format_stdlib_sig(const StandardFunction& fn) {
    std::string out = "fn std." + fn.module + "." + fn.name + "(";
    for (std::size_t i = 0; i < fn.parameters.size(); ++i) {
        if (i) out += ", ";
        if (i == 0 && fn.mutates_first) out += "mut ";
        out += fn.parameters[i];
    }
    out += ")";
    if (!fn.result.empty() && fn.result != "void") out += " " + fn.result;
    return out;
}

bool starts_with_icase(std::string_view text, std::string_view prefix) {
    if (prefix.size() > text.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(text[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i]))) return false;
    }
    return true;
}

struct ExportedSymbol {
    std::string name;
    std::string detail;
    int completion_kind{3}; // LSP CompletionItemKind
    std::string uri;
    LspRange range;
};

struct ImportedModuleInfo {
    std::string module_path;
    std::string uri;
    std::unordered_map<std::string, ExportedSymbol> exports;
    // Cross-file struct fields, inherent methods, and enum variants: "Type.member" -> ExportedSymbol
    std::unordered_map<std::string, ExportedSymbol> member_exports;
};

struct ScopedDef {
    std::string name;
    LspRange def_range;
    LspRange scope_range;
    std::string type_hint;
};

struct SymbolSummary {
    std::unordered_set<std::string> structs;
    std::unordered_set<std::string> enums;
    std::unordered_set<std::string> interfaces;
    std::unordered_set<std::string> enum_members;
    std::unordered_set<std::string> functions;
    std::unordered_set<std::string> methods;
    std::unordered_set<std::string> namespaces;
    std::unordered_set<std::string> consts;
    std::unordered_set<std::string> lets;
    std::unordered_set<std::string> vars;
    std::unordered_set<std::string> params;
    std::unordered_set<std::string> mut_params;
    std::unordered_set<std::string> fields;
    std::unordered_set<std::string> type_params;
    // alias -> full module path (e.g., "s" -> "std.strings", "helper" -> "helper")
    std::unordered_map<std::string, std::string> import_targets;
    // Struct fields & inherent methods for member completion
    std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> struct_fields;
    std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> struct_methods;
    std::unordered_map<std::string, std::vector<std::string>> enum_variants;
    // Variable / parameter -> type name
    std::unordered_map<std::string, std::string> var_types;
    // Scoped definitions for accurate local Go to Definition & type resolution
    std::vector<ScopedDef> scoped_defs;
    // Top-level / member symbol definitions for Go to Definition
    std::unordered_map<std::string, LspRange> local_defs;
    // Hover descriptions keyed by symbol name
    std::unordered_map<std::string, std::string> hover_docs;
};

struct DocSymbolInfo {
    std::string name;
    std::string detail;
    int kind{13}; // LSP SymbolKind
    LspRange range;
    LspRange selection_range;
    std::vector<DocSymbolInfo> children;
};

std::string infer_expr_type_hint(const Node& expr) {
    if (expr.kind == N::StructLiteral) return expr.text;
    if (expr.kind == N::String) return "string";
    if (expr.kind == N::Integer) return "int";
    if (expr.kind == N::Float) return "float";
    if (expr.kind == N::Boolean) return "bool";
    return {};
}

bool pos_in_range(std::size_t line, std::size_t ch, const LspRange& r) {
    if (line < r.start.line || line > r.end.line) return false;
    if (line == r.start.line && ch < r.start.character) return false;
    if (line == r.end.line && ch > r.end.character) return false;
    return true;
}

std::size_t range_span_score(const LspRange& r) {
    return (r.end.line - r.start.line) * 100000 + (r.end.line == r.start.line ? (r.end.character - r.start.character) : 0);
}

const ScopedDef* find_scoped_def(const SymbolSummary& sym, std::string_view name, std::size_t line, std::size_t ch) {
    const ScopedDef* best = nullptr;
    std::size_t best_score = static_cast<std::size_t>(-1);
    for (const auto& sd : sym.scoped_defs) {
        if (sd.name != name) continue;
        if (!pos_in_range(line, ch, sd.scope_range)) continue;
        // Prefer declarations that appear at or before the cursor within the same scope
        if (sd.def_range.start.line > line) continue;
        std::size_t score = range_span_score(sd.scope_range);
        if (!best || score < best_score || (score == best_score && sd.def_range.start.line >= best->def_range.start.line)) {
            best = &sd;
            best_score = score;
        }
    }
    return best;
}

void collect_ast_symbols(const Node& node, const Source& source, SymbolSummary& sym,
                         std::vector<DocSymbolInfo>* out_symbols, const LspRange& current_scope) {
    switch (node.kind) {
    case N::Import: {
        std::string binding = node.children.empty() ? node.text : node.children[0]->text;
        auto dot = binding.find('.');
        std::string head = dot == std::string::npos ? binding : binding.substr(0, dot);
        sym.namespaces.insert(head);
        sym.import_targets[binding] = node.text;
        if (!node.text.empty()) {
            std::size_t start = 0;
            while (start < node.text.size()) {
                auto next = node.text.find('.', start);
                auto seg = node.text.substr(start, next == std::string::npos ? std::string::npos : next - start);
                if (!seg.empty()) sym.namespaces.insert(seg);
                if (next == std::string::npos) break;
                start = next + 1;
            }
        }
        sym.local_defs[binding] = span_to_lsp_range(source, node.span);
        sym.hover_docs[binding] = "import " + node.text + (binding != node.text ? " as " + binding : "");
        break;
    }
    case N::Struct: {
        std::string name = decl_base_name(node);
        sym.structs.insert(name);
        auto srange = span_to_lsp_range(source, node.span);
        sym.local_defs[name] = srange;
        std::string doc = (node.text.find(" pub") != std::string::npos ? "pub struct " : "struct ") + name + " {\n";
        DocSymbolInfo ds;
        ds.name = name;
        ds.kind = 23; // Struct
        ds.range = srange;
        ds.selection_range = srange;
        auto& field_vec = sym.struct_fields[name];
        for (const auto& c : node.children) {
            if (c->kind == N::GenericParameters) {
                for (const auto& gp : c->children) sym.type_params.insert(gp->text);
            } else if (c->kind == N::Field) {
                std::string fname = decl_base_name(*c);
                std::string ftype = c->children.empty() ? "" : render_type_node(*c->children[0]);
                auto frange = span_to_lsp_range(source, c->span);
                sym.fields.insert(fname);
                field_vec.emplace_back(fname, ftype);
                sym.local_defs[name + "." + fname] = frange;
                doc += "    " + fname + (ftype.empty() ? "" : " " + ftype) + "\n";
                DocSymbolInfo fs;
                fs.name = fname;
                fs.detail = ftype;
                fs.kind = 8; // Field
                fs.range = frange;
                fs.selection_range = frange;
                ds.children.push_back(std::move(fs));
            }
        }
        doc += "}";
        sym.hover_docs[name] = std::move(doc);
        if (out_symbols) out_symbols->push_back(std::move(ds));
        return;
    }
    case N::Enum: {
        std::string name = decl_base_name(node);
        sym.enums.insert(name);
        auto erange = span_to_lsp_range(source, node.span);
        sym.local_defs[name] = erange;
        std::string doc = (node.text.find(" pub") != std::string::npos ? "pub enum " : "enum ") + name + " {\n";
        DocSymbolInfo ds;
        ds.name = name;
        ds.kind = 10; // Enum
        ds.range = erange;
        ds.selection_range = erange;
        auto& var_vec = sym.enum_variants[name];
        for (const auto& c : node.children) {
            if (c->kind == N::GenericParameters) {
                for (const auto& gp : c->children) sym.type_params.insert(gp->text);
            } else if (c->kind == N::Variant) {
                std::string vname = decl_base_name(*c);
                auto vrange = span_to_lsp_range(source, c->span);
                sym.enum_members.insert(vname);
                var_vec.push_back(vname);
                sym.local_defs[name + "." + vname] = vrange;
                doc += "    " + vname + "\n";
                DocSymbolInfo vs;
                vs.name = vname;
                vs.kind = 22; // EnumMember
                vs.range = vrange;
                vs.selection_range = vrange;
                ds.children.push_back(std::move(vs));
            }
        }
        doc += "}";
        sym.hover_docs[name] = std::move(doc);
        if (out_symbols) out_symbols->push_back(std::move(ds));
        return;
    }
    case N::Interface: {
        std::string name = decl_base_name(node);
        sym.interfaces.insert(name);
        auto irange = span_to_lsp_range(source, node.span);
        sym.local_defs[name] = irange;
        sym.hover_docs[name] = (node.text.find(" pub") != std::string::npos ? "pub interface " : "interface ") + name;
        DocSymbolInfo ds;
        ds.name = name;
        ds.kind = 11; // Interface
        ds.range = irange;
        ds.selection_range = irange;
        for (const auto& c : node.children) {
            if (c->kind == N::Function) {
                std::string mname = decl_base_name(*c);
                auto mrange = span_to_lsp_range(source, c->span);
                sym.methods.insert(mname);
                sym.local_defs[name + "." + mname] = mrange;
                DocSymbolInfo ms;
                ms.name = mname;
                ms.kind = 6; // Method
                ms.range = mrange;
                ms.selection_range = mrange;
                ds.children.push_back(std::move(ms));
            }
        }
        if (out_symbols) out_symbols->push_back(std::move(ds));
        return;
    }
    case N::Function: {
        std::string name = decl_base_name(node);
        auto fn_range = span_to_lsp_range(source, node.span);
        bool is_method = name.find('.') != std::string::npos;
        std::string sig;
        if (node.text.find(" pub") != std::string::npos) sig += "pub ";
        if (node.text.find(" async") != std::string::npos) sig += "async ";
        sig += "fn " + name + "(";
        bool first_param = true;
        std::string ret_str;
        for (const auto& c : node.children) {
            if (c->kind == N::GenericParameters) {
                for (const auto& gp : c->children) sym.type_params.insert(gp->text);
            } else if (c->kind == N::Parameter) {
                std::string pname = decl_base_name(*c);
                bool is_mut = c->text.find(" mut") != std::string::npos;
                if (is_mut) sym.mut_params.insert(pname);
                else sym.params.insert(pname);
                std::string ptype = c->children.empty() ? "" : render_type_node(*c->children[0]);
                std::string clean_type = strip_type_wrappers(ptype);
                auto prange = span_to_lsp_range(source, c->span);
                if (!clean_type.empty()) sym.var_types[pname] = clean_type;
                sym.scoped_defs.push_back({pname, prange, fn_range, clean_type});
                if (!first_param) sig += ", ";
                first_param = false;
                if (is_mut) sig += "mut ";
                sig += pname;
                if (!ptype.empty()) sig += " " + ptype;
                sym.hover_docs[pname] = std::string(is_mut ? "(mut parameter) " : "(parameter) ") + pname + (ptype.empty() ? "" : ": " + ptype);
            } else if (c->kind == N::ReturnTypes) {
                for (std::size_t i = 0; i < c->children.size(); ++i) {
                    if (i) ret_str += ", ";
                    ret_str += render_type_node(*c->children[i]);
                }
            }
        }
        sig += ")";
        if (!ret_str.empty()) sig += " " + ret_str;
        if (is_method) {
            auto dot = name.rfind('.');
            std::string recv = name.substr(0, dot);
            std::string mname = name.substr(dot + 1);
            sym.methods.insert(mname);
            sym.struct_methods[recv].emplace_back(mname, sig);
            sym.local_defs[name] = fn_range;
            sym.hover_docs[mname] = sig;
            // Register `self` inside method scope pointing to receiver struct
            LspRange self_def = sym.local_defs.contains(recv) ? sym.local_defs[recv] : fn_range;
            sym.scoped_defs.push_back({"self", self_def, fn_range, recv});
        } else {
            sym.functions.insert(name);
            sym.local_defs[name] = fn_range;
        }
        sym.hover_docs[name] = sig;

        if (out_symbols) {
            DocSymbolInfo ds;
            ds.name = name;
            ds.detail = ret_str;
            ds.kind = is_method ? 6 : 12; // Method : Function
            ds.range = fn_range;
            ds.selection_range = fn_range;
            out_symbols->push_back(std::move(ds));
        }
        for (const auto& c : node.children) {
            if (c->kind == N::Block) collect_ast_symbols(*c, source, sym, nullptr, fn_range);
        }
        return;
    }
    case N::Block: {
        auto brange = span_to_lsp_range(source, node.span);
        for (const auto& c : node.children) collect_ast_symbols(*c, source, sym, nullptr, brange);
        return;
    }
    case N::Const: {
        std::string name = decl_base_name(node);
        auto crange = span_to_lsp_range(source, node.span);
        sym.consts.insert(name);
        std::string t = (node.children.size() >= 2 && node.children[0]->kind != N::Omitted)
                            ? render_type_node(*node.children[0])
                            : (node.children.empty() ? "" : infer_expr_type_hint(*node.children.back()));
        std::string clean_t = strip_type_wrappers(t);
        if (!clean_t.empty()) sym.var_types[name] = clean_t;
        if (out_symbols) sym.local_defs[name] = crange;
        else sym.scoped_defs.push_back({name, crange, current_scope, clean_t});
        sym.hover_docs[name] = "const " + name + (t.empty() ? "" : ": " + t);
        if (out_symbols) {
            DocSymbolInfo ds;
            ds.name = name;
            ds.detail = t;
            ds.kind = 14; // Constant
            ds.range = crange;
            ds.selection_range = crange;
            out_symbols->push_back(std::move(ds));
        }
        break;
    }
    case N::Let: {
        std::string name = decl_base_name(node);
        auto lrange = span_to_lsp_range(source, node.span);
        sym.lets.insert(name);
        std::string t = (node.children.size() >= 2 && node.children[0]->kind != N::Omitted)
                            ? render_type_node(*node.children[0])
                            : (node.children.empty() ? "" : infer_expr_type_hint(*node.children.back()));
        std::string clean_t = strip_type_wrappers(t);
        if (!clean_t.empty()) sym.var_types[name] = clean_t;
        if (out_symbols) sym.local_defs[name] = lrange;
        else sym.scoped_defs.push_back({name, lrange, current_scope, clean_t});
        if (!sym.hover_docs.contains(name))
            sym.hover_docs[name] = "let " + name + (t.empty() ? "" : ": " + t);
        if (out_symbols) {
            DocSymbolInfo ds;
            ds.name = name;
            ds.detail = t;
            ds.kind = 13; // Variable
            ds.range = lrange;
            ds.selection_range = lrange;
            out_symbols->push_back(std::move(ds));
        }
        break;
    }
    case N::Var: {
        std::string name = decl_base_name(node);
        auto vrange = span_to_lsp_range(source, node.span);
        sym.vars.insert(name);
        std::string t = (node.children.size() >= 2 && node.children[0]->kind != N::Omitted)
                            ? render_type_node(*node.children[0])
                            : (node.children.empty() ? "" : infer_expr_type_hint(*node.children.back()));
        std::string clean_t = strip_type_wrappers(t);
        if (!clean_t.empty()) sym.var_types[name] = clean_t;
        if (out_symbols) sym.local_defs[name] = vrange;
        else sym.scoped_defs.push_back({name, vrange, current_scope, clean_t});
        if (!sym.hover_docs.contains(name))
            sym.hover_docs[name] = "var " + name + (t.empty() ? "" : ": " + t);
        if (out_symbols) {
            DocSymbolInfo ds;
            ds.name = name;
            ds.detail = t;
            ds.kind = 13; // Variable
            ds.range = vrange;
            ds.selection_range = vrange;
            out_symbols->push_back(std::move(ds));
        }
        break;
    }
    case N::MultiBinding: {
        bool is_var = node.text == "var";
        if (!node.children.empty() && node.children[0]->kind == N::BindingList) {
            for (const auto& p : node.children[0]->children) {
                std::string bname = decl_base_name(*p);
                if (bname == "_" || bname.empty()) continue;
                auto brange = span_to_lsp_range(source, p->span);
                if (is_var) sym.vars.insert(bname);
                else sym.lets.insert(bname);
                std::string btype = p->children.empty() ? "" : render_type_node(*p->children[0]);
                std::string clean_t = strip_type_wrappers(btype);
                if (!clean_t.empty()) sym.var_types[bname] = clean_t;
                if (out_symbols) sym.local_defs[bname] = brange;
                else sym.scoped_defs.push_back({bname, brange, current_scope, clean_t});
                if (!sym.hover_docs.contains(bname))
                    sym.hover_docs[bname] = std::string(is_var ? "var " : "let ") + bname + (btype.empty() ? "" : ": " + btype);
            }
        }
        break;
    }
    case N::For: {
        auto frange = span_to_lsp_range(source, node.span);
        if (node.children.size() >= 2) {
            for (std::size_t i = 0; i + 2 < node.children.size(); ++i) {
                const auto& b = *node.children[i];
                sym.lets.insert(b.text);
                sym.scoped_defs.push_back({b.text, span_to_lsp_range(source, b.span), frange, ""});
            }
        }
        for (const auto& c : node.children) collect_ast_symbols(*c, source, sym, nullptr, frange);
        return;
    }
    case N::MatchArm: {
        auto arange = span_to_lsp_range(source, node.span);
        if (node.children.size() >= 3) {
            for (const auto& p : node.children[1]->children) {
                if (p->text != "_" && !p->text.empty()) {
                    sym.lets.insert(p->text);
                    sym.scoped_defs.push_back({p->text, span_to_lsp_range(source, p->span), arange, ""});
                }
            }
            collect_ast_symbols(*node.children[2], source, sym, nullptr, arange);
        }
        return;
    }
    default:
        break;
    }
    for (const auto& c : node.children) {
        collect_ast_symbols(*c, source, sym, node.kind == N::Program ? out_symbols : nullptr, current_scope);
    }
}

void collect_ast_symbols(const Node& node, const Source& source, SymbolSummary& sym, std::vector<DocSymbolInfo>* out_symbols) {
    collect_ast_symbols(node, source, sym, out_symbols, span_to_lsp_range(source, node.span));
}

void enrich_from_sema(const Node& node, const SemanticModel& model, const std::string& file_id,
                      SymbolSummary& sym) {
    if (node.span.file_id == file_id) {
        if (auto it = model.types.find(&node); it != model.types.end() && !it->second.empty()) {
            if (node.kind == N::Let || node.kind == N::Var || node.kind == N::Const) {
                std::string name = decl_base_name(node);
                const char* kw = node.kind == N::Const ? "const " : (node.kind == N::Var ? "var " : "let ");
                sym.hover_docs[name] = std::string(kw) + name + ": " + it->second;
                sym.var_types[name] = strip_type_wrappers(it->second);
            }
        }
    }
    for (const auto& c : node.children) enrich_from_sema(*c, model, file_id, sym);
}

JsonOut doc_symbol_json(const DocSymbolInfo& s) {
    std::vector<JsonOut> kids;
    kids.reserve(s.children.size());
    for (const auto& c : s.children) kids.push_back(doc_symbol_json(c));
    return JsonOut::object({
        {"name", JsonOut::str(s.name)},
        {"detail", JsonOut::str(s.detail)},
        {"kind", JsonOut::integer(s.kind)},
        {"range", lsp_range_json(s.range)},
        {"selectionRange", lsp_range_json(s.selection_range)},
        {"children", JsonOut::array(kids)},
    });
}

// --- Semantic tokens computation ---

struct EncodedToken {
    std::size_t line;
    std::size_t start_char;
    std::size_t length;
    std::uint32_t type;
    std::uint32_t modifiers;
};

bool is_keyword_token(K kind) {
    return kind >= K::Let && kind <= K::Nil;
}

bool is_operator_token(K kind) {
    switch (kind) {
    case K::Range: case K::RangeInclusive: case K::Question:
    case K::Plus: case K::Minus: case K::Star: case K::Slash:
    case K::FloorDivide: case K::Percent: case K::Power:
    case K::Assign: case K::PlusAssign: case K::MinusAssign: case K::StarAssign:
    case K::SlashAssign: case K::PercentAssign: case K::PowerAssign: case K::FloorAssign:
    case K::Increment: case K::Decrement:
    case K::Equal: case K::NotEqual: case K::Less: case K::LessEqual: case K::Greater: case K::GreaterEqual:
    case K::And: case K::Or: case K::Not:
    case K::BitAnd: case K::BitOr: case K::BitXor: case K::BitNot:
    case K::ShiftLeft: case K::ShiftRight: case K::Arrow:
        return true;
    default:
        return false;
    }
}

void push_span_tokens(const Source& source, std::size_t start_off, std::size_t end_off,
                      std::uint32_t type, std::uint32_t mods, std::vector<EncodedToken>& out) {
    if (end_off <= start_off || start_off >= source.text().size()) return;
    end_off = std::min(end_off, source.text().size());
    auto sp_start = source.span(start_off, start_off);
    auto sp_end = source.span(end_off, end_off);
    if (sp_start.line == sp_end.line) {
        auto line_str = source.line_text(sp_start.line);
        std::size_t s_utf16 = utf8_prefix_to_utf16(line_str, sp_start.column - 1);
        std::size_t e_utf16 = utf8_prefix_to_utf16(line_str, sp_end.column - 1);
        if (e_utf16 > s_utf16) {
            out.push_back({sp_start.line - 1, s_utf16, e_utf16 - s_utf16, type, mods});
        }
        return;
    }
    for (std::size_t ln = sp_start.line; ln <= sp_end.line; ++ln) {
        auto line_str = source.line_text(ln);
        std::size_t byte_s = (ln == sp_start.line) ? (sp_start.column - 1) : 0;
        std::size_t byte_e = (ln == sp_end.line) ? (sp_end.column - 1) : line_str.size();
        std::size_t s_utf16 = utf8_prefix_to_utf16(line_str, byte_s);
        std::size_t e_utf16 = utf8_prefix_to_utf16(line_str, byte_e);
        if (e_utf16 > s_utf16) {
            out.push_back({ln - 1, s_utf16, e_utf16 - s_utf16, type, mods});
        }
    }
}

std::vector<std::uint32_t> build_semantic_tokens(const Source& source, const std::vector<Token>& tokens,
                                                 const SymbolSummary& sym) {
    std::vector<EncodedToken> raw;
    raw.reserve(tokens.size());

    auto non_nl_prev = [&](std::size_t idx) -> const Token* {
        while (idx > 0) {
            --idx;
            if (tokens[idx].kind != K::Newline) return &tokens[idx];
        }
        return nullptr;
    };
    auto non_nl_next = [&](std::size_t idx) -> const Token* {
        while (idx + 1 < tokens.size()) {
            ++idx;
            if (tokens[idx].kind != K::Newline) return &tokens[idx];
        }
        return nullptr;
    };

    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const auto& tok = tokens[i];
        if (tok.kind == K::End || tok.kind == K::Newline) continue;

        std::optional<std::pair<std::uint32_t, std::uint32_t>> classified;

        if (is_keyword_token(tok.kind)) {
            classified = {ST_Keyword, SM_None};
        } else if (tok.kind == K::Integer || tok.kind == K::Float) {
            classified = {ST_Number, SM_None};
        } else if (tok.kind == K::String) {
            classified = {ST_String, SM_None};
        } else if (is_operator_token(tok.kind)) {
            classified = {ST_Operator, SM_None};
        } else if (tok.kind == K::Identifier) {
            const auto* prev = non_nl_prev(i);
            const auto* next = non_nl_next(i);
            const std::string& name = tok.text;

            if (name == "mut" || name == "by") {
                classified = {ST_Keyword, SM_None};
            } else if (prev && prev->kind == K::At) {
                classified = {ST_Keyword, SM_None};
            } else if (prev && prev->kind == K::Struct) {
                classified = {ST_Struct, SM_Declaration};
            } else if (prev && prev->kind == K::Enum) {
                classified = {ST_Enum, SM_Declaration};
            } else if (prev && prev->kind == K::Interface) {
                classified = {ST_Interface, SM_Declaration};
            } else if (prev && prev->kind == K::Fn) {
                if (next && next->kind == K::Dot) {
                    classified = {ST_Struct, SM_None};
                } else {
                    classified = {ST_Function, SM_Declaration};
                }
            } else if (prev && (prev->kind == K::Import || prev->kind == K::As)) {
                classified = {ST_Namespace, SM_None};
            } else if (prev && prev->kind == K::Const) {
                classified = {ST_Variable, SM_Declaration | SM_Readonly | SM_Static};
            } else if (prev && prev->kind == K::Let) {
                classified = {ST_Variable, SM_Declaration | SM_Readonly};
            } else if (prev && prev->kind == K::Var) {
                classified = {ST_Variable, SM_Declaration | SM_Modification};
            } else if (prev && prev->kind == K::Dot) {
                const auto* prev2 = (i >= 2 && tokens[i - 1].kind == K::Dot) ? non_nl_prev(i - 1) : nullptr;
                if (prev2 && sym.enums.contains(prev2->text)) {
                    classified = {ST_EnumMember, SM_Readonly};
                } else if (prev2 && sym.namespaces.contains(prev2->text)) {
                    if (next && next->kind == K::LParen) classified = {ST_Function, SM_None};
                    else if (next && next->kind == K::Dot) classified = {ST_Namespace, SM_None};
                    else if (!name.empty() && std::isupper(static_cast<unsigned char>(name[0]))) classified = {ST_Type, SM_None};
                    else classified = {ST_Variable, SM_Readonly};
                } else if (next && next->kind == K::LParen) {
                    classified = {ST_Method, SM_None};
                } else {
                    classified = {ST_Property, SM_None};
                }
            } else if (is_builtin_type(name)) {
                classified = {ST_Type, SM_DefaultLibrary};
            } else if (sym.structs.contains(name)) {
                classified = {ST_Struct, SM_None};
            } else if (sym.enums.contains(name)) {
                classified = {ST_Enum, SM_None};
            } else if (sym.interfaces.contains(name)) {
                classified = {ST_Interface, SM_None};
            } else if (sym.type_params.contains(name)) {
                classified = {ST_TypeParameter, SM_None};
            } else if (sym.namespaces.contains(name) && next && next->kind == K::Dot) {
                classified = {ST_Namespace, SM_None};
            } else if (next && next->kind == K::LParen) {
                if (is_builtin_fn(name)) classified = {ST_Function, SM_DefaultLibrary};
                else classified = {ST_Function, SM_None};
            } else if (sym.consts.contains(name)) {
                classified = {ST_Variable, SM_Readonly | SM_Static};
            } else if (sym.mut_params.contains(name)) {
                classified = {ST_Parameter, SM_Modification};
            } else if (sym.params.contains(name)) {
                classified = {ST_Parameter, SM_Readonly};
            } else if (sym.vars.contains(name)) {
                classified = {ST_Variable, SM_Modification};
            } else if (sym.lets.contains(name)) {
                classified = {ST_Variable, SM_Readonly};
            } else if (sym.functions.contains(name)) {
                classified = {ST_Function, SM_None};
            } else if (sym.enum_members.contains(name)) {
                classified = {ST_EnumMember, SM_Readonly};
            } else if (sym.fields.contains(name) && next && next->kind == K::Colon) {
                classified = {ST_Property, SM_None};
            } else if (name == "self") {
                classified = {ST_Parameter, SM_Readonly};
            } else if (!name.empty() && std::isupper(static_cast<unsigned char>(name[0]))) {
                classified = {ST_Type, SM_None};
            } else {
                classified = {ST_Variable, SM_None};
            }
        }

        if (classified) {
            push_span_tokens(source, tok.span.start_offset, tok.span.end_offset,
                             classified->first, classified->second, raw);
        }
    }

    std::vector<std::uint32_t> data;
    data.reserve(raw.size() * 5);
    std::size_t prev_line = 0;
    std::size_t prev_char = 0;
    for (const auto& t : raw) {
        std::size_t delta_line = t.line - prev_line;
        std::size_t delta_char = (delta_line == 0) ? (t.start_char - prev_char) : t.start_char;
        data.push_back(static_cast<std::uint32_t>(delta_line));
        data.push_back(static_cast<std::uint32_t>(delta_char));
        data.push_back(static_cast<std::uint32_t>(t.length));
        data.push_back(t.type);
        data.push_back(t.modifiers);
        prev_line = t.line;
        prev_char = t.start_char;
    }
    return data;
}

// --- LSP Server state ---

struct DocumentState {
    std::string uri;
    std::optional<std::filesystem::path> path;
    std::string text;
    std::int64_t version{0};
    std::unique_ptr<Source> source;
    std::vector<Token> tokens;
    SymbolSummary symbols;
    std::vector<DocSymbolInfo> outline;
    std::unordered_map<std::string, ImportedModuleInfo> imported_modules;
    std::unordered_set<std::string> published_uris;
};

class LspServer {
public:
    LspServer(std::istream& in, std::ostream& out) : in_(in), out_(out) {}

    int run() {
        while (true) {
            auto payload = read_message();
            if (!payload) break;
            if (!handle_message(*payload)) break;
        }
        return shutdown_requested_ ? 0 : 0;
    }

private:
    std::istream& in_;
    std::ostream& out_;
    bool shutdown_requested_{false};
    std::unordered_map<std::string, DocumentState> docs_;

    std::optional<std::string> read_message() {
        std::size_t content_length = 0;
        bool has_length = false;
        std::string line;
        while (std::getline(in_, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) break;
            constexpr std::string_view cl = "Content-Length:";
            if (line.size() >= cl.size()) {
                bool match = true;
                for (std::size_t i = 0; i < cl.size(); ++i) {
                    if (std::tolower(static_cast<unsigned char>(line[i])) !=
                        std::tolower(static_cast<unsigned char>(cl[i]))) {
                        match = false;
                        break;
                    }
                }
                if (match) {
                    std::string num = line.substr(cl.size());
                    std::size_t p = 0;
                    while (p < num.size() && (num[p] == ' ' || num[p] == '\t')) ++p;
                    try {
                        content_length = static_cast<std::size_t>(std::stoull(num.substr(p)));
                        has_length = true;
                    } catch (...) {
                        return std::nullopt;
                    }
                }
            }
        }
        if (!has_length || in_.eof()) return std::nullopt;
        if (content_length > 64 * 1024 * 1024) return std::nullopt;
        std::string body(content_length, '\0');
        in_.read(body.data(), static_cast<std::streamsize>(content_length));
        if (static_cast<std::size_t>(in_.gcount()) != content_length) return std::nullopt;
        return body;
    }

    void write_message(const std::string& json_body) {
        out_ << "Content-Length: " << json_body.size() << "\r\n\r\n" << json_body;
        out_.flush();
    }

    void send_response(const std::string& id_raw, const JsonOut& result) {
        write_message(JsonOut::object({
            {"jsonrpc", JsonOut::str("2.0")},
            {"id", JsonOut::raw_json(id_raw)},
            {"result", result},
        }).raw);
    }

    void send_error(const std::string& id_raw, int code, std::string_view message) {
        write_message(JsonOut::object({
            {"jsonrpc", JsonOut::str("2.0")},
            {"id", JsonOut::raw_json(id_raw)},
            {"error", JsonOut::object({
                {"code", JsonOut::integer(code)},
                {"message", JsonOut::str(message)},
            })},
        }).raw);
    }

    void send_notification(std::string_view method, const JsonOut& params) {
        write_message(JsonOut::object({
            {"jsonrpc", JsonOut::str("2.0")},
            {"method", JsonOut::str(method)},
            {"params", params},
        }).raw);
    }

    void publish_diagnostics(const std::string& uri, const std::vector<JsonOut>& diags) {
        send_notification("textDocument/publishDiagnostics", JsonOut::object({
            {"uri", JsonOut::str(uri)},
            {"diagnostics", JsonOut::array(diags)},
        }));
    }

    std::string uri_for_path(const std::filesystem::path& fpath) const {
        std::error_code ec1;
        auto norm = std::filesystem::weakly_canonical(fpath, ec1);
        const auto& target = ec1 ? fpath : norm;
        for (const auto& [open_uri, open_doc] : docs_) {
            if (!open_doc.path) continue;
            std::error_code ec2;
            auto open_norm = std::filesystem::weakly_canonical(*open_doc.path, ec2);
            std::error_code eq_ec;
            if (std::filesystem::equivalent(target, ec2 ? *open_doc.path : open_norm, eq_ec) && !eq_ec) {
                return open_uri;
            }
            if (target == (ec2 ? *open_doc.path : open_norm)) {
                return open_uri;
            }
        }
        return path_to_uri(fpath);
    }

    std::optional<std::string> read_text_with_overlay(const std::filesystem::path& path) const {
        std::string target_uri = uri_for_path(path);
        if (auto it = docs_.find(target_uri); it != docs_.end()) {
            return it->second.text;
        }
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) return std::nullopt;
        auto sz = input.tellg();
        if (sz < 0 || sz > 16 * 1024 * 1024) return std::nullopt;
        std::string text(static_cast<std::size_t>(sz), '\0');
        input.seekg(0);
        if (!text.empty() && !input.read(text.data(), sz)) return std::nullopt;
        return text;
    }

    std::optional<std::filesystem::path> resolve_import_file(const std::filesystem::path& base_dir, const std::string& target) const {
        namespace fs = std::filesystem;
        std::error_code ec;
        auto try_under = [&](const fs::path& root, const std::string& mod) -> std::optional<fs::path> {
            if (mod.empty()) {
                auto pkg = root / "package.hua";
                if (fs::is_regular_file(pkg, ec) && !ec) return pkg;
                return std::nullopt;
            }
            std::string rel = mod;
            std::replace(rel.begin(), rel.end(), '.', '/');
            fs::path c1 = root / fs::path(std::u8string(rel.begin(), rel.end()));
            c1 += ".hua";
            if (fs::is_regular_file(c1, ec) && !ec) return c1;
            ec.clear();
            fs::path c2 = root / fs::path(std::u8string(rel.begin(), rel.end())) / "package.hua";
            if (fs::is_regular_file(c2, ec) && !ec) return c2;
            ec.clear();
            return std::nullopt;
        };

        // 1. Direct relative resolution under current directory (and walk up for package root)
        if (auto direct = try_under(base_dir, target)) return direct;

        // 2. Check hua.package.json deps in current or ancestor directories
        fs::path dir = base_dir;
        for (int depth = 0; depth < 6; ++depth) {
            auto manifest = dir / "hua.package.json";
            if (fs::is_regular_file(manifest, ec) && !ec) {
                if (auto text = read_text_with_overlay(manifest)) {
                    const auto decoded = json_decode(*text);
                    if (hua::holds_alternative<Cow<ResultValue>>(decoded.data)) {
                        const auto& res = hua::cget<ResultValue>(decoded.data);
                        if (res.ok && res.payload) {
                            if (const auto* jv = hua::get_if<JsonValue>(&res.payload->data); jv && jv->data) {
                                if (auto deps = j_get(jv->data, "deps")) {
                                    if (const auto* dep_map = std::get_if<JsonData::Object>(&deps->data)) {
                                        auto dot = target.find('.');
                                        std::string first = dot == std::string::npos ? target : target.substr(0, dot);
                                        std::string rest = dot == std::string::npos ? std::string{} : target.substr(dot + 1);
                                        if (auto it = dep_map->find(first); it != dep_map->end()) {
                                            if (auto rel_path = j_str(it->second)) {
                                                fs::path dep_root = dir / fs::path(std::u8string(rel_path->begin(), rel_path->end()));
                                                if (auto found = try_under(dep_root, rest)) return found;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                if (auto in_root = try_under(dir, target)) return in_root;
                break;
            }
            ec.clear();
            auto parent = dir.parent_path();
            if (parent == dir) break;
            dir = parent;
        }
        return std::nullopt;
    }

    void index_imported_modules(DocumentState& doc) {
        doc.imported_modules.clear();
        if (!doc.path) return;
        const auto base_dir = doc.path->parent_path();
        for (const auto& [alias, target] : doc.symbols.import_targets) {
            if (target == "std" || target.starts_with("std.")) continue;
            auto chosen_opt = resolve_import_file(base_dir, target);
            if (!chosen_opt) continue;
            const auto& chosen = *chosen_opt;

            auto text = read_text_with_overlay(chosen);
            if (!text) continue;
            auto u8 = chosen.u8string();
            Source mod_src(std::string(u8.begin(), u8.end()), std::move(*text));
            try {
                auto toks = Lexer(mod_src).scan();
                std::vector<Diagnostic> ignored_errs;
                auto tree = Parser(std::move(toks)).parse_tolerant(ignored_errs);
                SymbolSummary mod_sym;
                collect_ast_symbols(*tree, mod_src, mod_sym, nullptr);
                ImportedModuleInfo info;
                info.module_path = target;
                info.uri = uri_for_path(chosen);
                for (const auto& c : tree->children) {
                    if (c->text.find(" pub") == std::string::npos) continue;
                    std::string ename = decl_base_name(*c);
                    if (ename.find('.') != std::string::npos) {
                        // Exported inherent method: `pub fn User.title()`
                        std::string detail = mod_sym.hover_docs.contains(ename) ? mod_sym.hover_docs[ename] : ename;
                        info.member_exports[ename] = {ename, detail, 2, info.uri, span_to_lsp_range(mod_src, c->span)};
                        continue;
                    }
                    int ckind = 3; // Function
                    if (c->kind == N::Struct) {
                        ckind = 22; // Struct
                        for (const auto& fc : c->children) {
                            if (fc->kind == N::Field) {
                                std::string fname = decl_base_name(*fc);
                                std::string ftype = fc->children.empty() ? "" : render_type_node(*fc->children[0]);
                                info.member_exports[ename + "." + fname] = {fname, ftype, 5, info.uri, span_to_lsp_range(mod_src, fc->span)};
                            }
                        }
                    } else if (c->kind == N::Enum) {
                        ckind = 13; // Enum
                        for (const auto& vc : c->children) {
                            if (vc->kind == N::Variant) {
                                std::string vname = decl_base_name(*vc);
                                info.member_exports[ename + "." + vname] = {vname, ename + "." + vname, 20, info.uri, span_to_lsp_range(mod_src, vc->span)};
                            }
                        }
                    } else if (c->kind == N::Interface) {
                        ckind = 8; // Interface
                    } else if (c->kind == N::Const) {
                        ckind = 21; // Constant
                    } else if (c->kind == N::Let || c->kind == N::Var) {
                        ckind = 6; // Variable
                    }
                    std::string detail = mod_sym.hover_docs.contains(ename) ? mod_sym.hover_docs[ename] : ename;
                    info.exports[ename] = {ename, detail, ckind, info.uri, span_to_lsp_range(mod_src, c->span)};
                }
                doc.imported_modules[alias] = std::move(info);
            } catch (...) {
            }
        }
    }

    JsonOut make_diagnostic_json(const Diagnostic& err, const DocumentState& doc, const ModuleLoader* loader, std::string& target_uri_out) {
        target_uri_out = doc.uri;
        const Source* target_source = doc.source.get();
        if (loader && !err.span().file_id.empty()) {
            if (const auto* fpath = loader->file_path(err.span().file_id)) {
                target_uri_out = uri_for_path(*fpath);
            }
            if (const auto* src = loader->source(err.span().file_id)) {
                target_source = src;
            }
        }
        LspRange range{{0, 0}, {0, 1}};
        if (target_source) {
            range = span_to_lsp_range(*target_source, err.span());
        } else if (err.span().line > 0) {
            std::size_t ln = err.span().line - 1;
            std::size_t col = err.span().column > 0 ? err.span().column - 1 : 0;
            range = {{ln, col}, {ln, col + 1}};
        }

        std::string msg = err.what();
        if (!err.help().empty()) msg += "\nhelp: " + err.help();

        std::vector<JsonOut> related;
        for (const auto& site : err.trace().sites) {
            std::string rel_uri = doc.uri;
            const Source* rel_src = doc.source.get();
            if (loader) {
                if (const auto* rpath = loader->file_path(site.span.file_id)) rel_uri = uri_for_path(*rpath);
                if (const auto* rsrc = loader->source(site.span.file_id)) rel_src = rsrc;
            }
            LspRange rrange = rel_src ? span_to_lsp_range(*rel_src, site.span)
                                      : LspRange{{site.span.line > 0 ? site.span.line - 1 : 0, site.span.column > 0 ? site.span.column - 1 : 0},
                                                 {site.span.line > 0 ? site.span.line - 1 : 0, (site.span.column > 0 ? site.span.column - 1 : 0) + 1}};
            related.push_back(JsonOut::object({
                {"location", JsonOut::object({
                    {"uri", JsonOut::str(rel_uri)},
                    {"range", lsp_range_json(rrange)},
                })},
                {"message", JsonOut::str(site.message)},
            }));
        }

        std::vector<std::pair<std::string, JsonOut>> fields = {
            {"range", lsp_range_json(range)},
            {"severity", JsonOut::integer(1)},
            {"code", JsonOut::str(err.code())},
            {"source", JsonOut::str("hua")},
            {"message", JsonOut::str(msg)},
        };
        if (!related.empty()) fields.emplace_back("relatedInformation", JsonOut::array(related));
        return JsonOut::object(fields);
    }

    void analyze_document(DocumentState& doc) {
        const std::string filename = doc.path ? [&] {
            auto u8 = doc.path->u8string();
            return std::string(u8.begin(), u8.end());
        }() : doc.uri;

        doc.source = std::make_unique<Source>(filename, doc.text);
        doc.tokens.clear();

        std::unordered_map<std::string, std::vector<JsonOut>> diag_map;
        diag_map[doc.uri] = {};

        // Step 1: Tolerant lex + parse (collects multiple syntax errors & builds partial AST)
        std::vector<Diagnostic> syntax_errors;
        bool lex_ok = false;
        try {
            doc.tokens = Lexer(*doc.source).scan();
            lex_ok = true;
            auto tree = Parser(doc.tokens).parse_tolerant(syntax_errors);
            SymbolSummary fresh;
            std::vector<DocSymbolInfo> outline;
            collect_ast_symbols(*tree, *doc.source, fresh, &outline);
            // If there are syntax errors, merge fresh partial symbols over cached ones so nothing vanishes
            if (!syntax_errors.empty()) {
                for (const auto& [k, v] : doc.symbols.var_types) fresh.var_types.try_emplace(k, v);
                for (const auto& [k, v] : doc.symbols.struct_fields) fresh.struct_fields.try_emplace(k, v);
                for (const auto& [k, v] : doc.symbols.struct_methods) fresh.struct_methods.try_emplace(k, v);
                for (const auto& [k, v] : doc.symbols.enum_variants) fresh.enum_variants.try_emplace(k, v);
                for (const auto& [k, v] : doc.symbols.import_targets) fresh.import_targets.try_emplace(k, v);
                for (const auto& [k, v] : doc.symbols.local_defs) fresh.local_defs.try_emplace(k, v);
                for (const auto& s : doc.symbols.structs) fresh.structs.insert(s);
                for (const auto& s : doc.symbols.enums) fresh.enums.insert(s);
                for (const auto& s : doc.symbols.functions) fresh.functions.insert(s);
                for (const auto& s : doc.symbols.namespaces) fresh.namespaces.insert(s);
            }
            doc.symbols = std::move(fresh);
            if (!outline.empty() || syntax_errors.empty()) doc.outline = std::move(outline);
        } catch (const Diagnostic& lex_err) {
            std::string target_uri;
            auto j = make_diagnostic_json(lex_err, doc, nullptr, target_uri);
            diag_map[target_uri].push_back(std::move(j));
        } catch (...) {
        }

        // Index imported user modules for cross-file Go to Definition & Completion
        index_imported_modules(doc);

        // Step 2: If tolerant parse found syntax errors, report ALL of them immediately
        if (lex_ok && !syntax_errors.empty()) {
            for (const auto& err : syntax_errors) {
                std::string target_uri;
                auto j = make_diagnostic_json(err, doc, nullptr, target_uri);
                diag_map[target_uri].push_back(std::move(j));
            }
        } else if (lex_ok) {
            // Step 3: Clean syntax -> run full ModuleLoader (with VFS overlays) + SemanticAnalyzer
            if (doc.path) {
                ModuleLoader loader;
                for (const auto& [_, d] : docs_) {
                    if (d.path) loader.set_overlay(*d.path, d.text);
                }
                try {
                    const auto& linked = loader.load(*doc.path);
                    auto model = SemanticAnalyzer{}.analyze(linked, loader.externals());
                    std::string file_id = filename;
                    for (const auto* s : loader.sources()) {
                        if (const auto* fp = loader.file_path(s->filename())) {
                            if (uri_for_path(*fp) == doc.uri) {
                                file_id = s->filename();
                                break;
                            }
                        }
                    }
                    enrich_from_sema(linked, model, file_id, doc.symbols);
                } catch (const Diagnostic& err) {
                    std::string target_uri;
                    auto j = make_diagnostic_json(err, doc, &loader, target_uri);
                    diag_map[target_uri].push_back(std::move(j));
                } catch (const std::exception& ex) {
                    diag_map[doc.uri].push_back(JsonOut::object({
                        {"range", lsp_range_json({{0, 0}, {0, 1}})},
                        {"severity", JsonOut::integer(1)},
                        {"source", JsonOut::str("hua")},
                        {"message", JsonOut::str(ex.what())},
                    }));
                }
            } else {
                try {
                    auto tokens = Lexer(*doc.source).scan();
                    auto tree = Parser(std::move(tokens)).parse();
                    auto model = SemanticAnalyzer{}.analyze(*tree);
                    enrich_from_sema(*tree, model, doc.source->filename(), doc.symbols);
                } catch (const Diagnostic& err) {
                    std::string target_uri;
                    auto j = make_diagnostic_json(err, doc, nullptr, target_uri);
                    diag_map[target_uri].push_back(std::move(j));
                } catch (...) {
                }
            }
        }

        for (const auto& old_uri : doc.published_uris) {
            if (!diag_map.contains(old_uri)) publish_diagnostics(old_uri, {});
        }
        doc.published_uris.clear();
        for (const auto& [uri, items] : diag_map) {
            publish_diagnostics(uri, items);
            if (!items.empty()) doc.published_uris.insert(uri);
        }
    }

    // Find identifier token and optional preceding qualifier (for `qual.ident`) at cursor
    struct CursorToken {
        std::size_t index{};
        std::string name;
        std::string qualifier; // non-empty if preceded by `<qualifier>.`
        SourceSpan span;
    };

    std::optional<CursorToken> find_token_at(const DocumentState& doc, std::size_t line, std::size_t character) const {
        if (!doc.source || doc.tokens.empty()) return std::nullopt;
        auto line_str = doc.source->line_text(line + 1);
        std::size_t byte_col = utf16_col_to_utf8_byte(line_str, character);

        for (std::size_t i = 0; i < doc.tokens.size(); ++i) {
            const auto& tok = doc.tokens[i];
            if (tok.kind != K::Identifier || tok.span.line != line + 1) continue;
            std::size_t tok_start = tok.span.column > 0 ? tok.span.column - 1 : 0;
            std::size_t tok_len = tok.span.end_offset > tok.span.start_offset ? (tok.span.end_offset - tok.span.start_offset) : tok.text.size();
            if (byte_col >= tok_start && byte_col <= tok_start + tok_len) {
                std::string qual;
                std::size_t q = i;
                while (q >= 2 && doc.tokens[q - 1].kind == K::Dot && doc.tokens[q - 2].kind == K::Identifier) {
                    qual = qual.empty() ? doc.tokens[q - 2].text : (doc.tokens[q - 2].text + "." + qual);
                    q -= 2;
                }
                return CursorToken{i, tok.text, qual, tok.span};
            }
        }
        return std::nullopt;
    }

    std::optional<std::string> resolve_std_module(const DocumentState& doc, const std::string& qual) const {
        if (qual.starts_with("std.")) return qual.substr(4);
        if (auto it = doc.symbols.import_targets.find(qual); it != doc.symbols.import_targets.end()) {
            if (it->second.starts_with("std.")) return it->second.substr(4);
        }
        return std::nullopt;
    }

    JsonOut handle_hover(const DocumentState& doc, std::size_t line, std::size_t character) {
        auto ct = find_token_at(doc, line, character);
        if (!ct) return JsonOut::null_val();
        std::string content;

        if (!ct->qualifier.empty()) {
            if (auto std_mod = resolve_std_module(doc, ct->qualifier)) {
                for (const auto& fn : standard_functions()) {
                    if (fn.module == *std_mod && fn.name == ct->name) {
                        content = "```hua\n" + format_stdlib_sig(fn) + "\n```";
                        break;
                    }
                }
            }
            if (content.empty()) {
                if (auto mit = doc.imported_modules.find(ct->qualifier); mit != doc.imported_modules.end()) {
                    if (auto eit = mit->second.exports.find(ct->name); eit != mit->second.exports.end()) {
                        content = "```hua\n" + eit->second.detail + "\n```";
                    }
                }
            }
            if (content.empty()) {
                if (auto vit = doc.symbols.var_types.find(ct->qualifier); vit != doc.symbols.var_types.end()) {
                    const std::string& tname = vit->second;
                    if (auto fit = doc.symbols.struct_fields.find(tname); fit != doc.symbols.struct_fields.end()) {
                        for (const auto& [fn, ft] : fit->second) {
                            if (fn == ct->name) {
                                content = "```hua\n(field) " + tname + "." + fn + (ft.empty() ? "" : ": " + ft) + "\n```";
                                break;
                            }
                        }
                    }
                    if (content.empty()) {
                        if (auto mit = doc.symbols.struct_methods.find(tname); mit != doc.symbols.struct_methods.end()) {
                            for (const auto& [mn, msig] : mit->second) {
                                if (mn == ct->name) {
                                    content = "```hua\n" + msig + "\n```";
                                    break;
                                }
                            }
                        }
                    }
                }
            }
        }

        if (content.empty()) {
            if (auto it = doc.symbols.hover_docs.find(ct->name); it != doc.symbols.hover_docs.end()) {
                content = "```hua\n" + it->second + "\n```";
            } else if (is_builtin_type(ct->name)) {
                content = "```hua\n(builtin type) " + ct->name + "\n```";
            } else if (is_builtin_fn(ct->name)) {
                content = "```hua\n(builtin) " + ct->name + "\n```";
            }
        }

        if (content.empty()) return JsonOut::null_val();
        return JsonOut::object({
            {"contents", JsonOut::object({
                {"kind", JsonOut::str("markdown")},
                {"value", JsonOut::str(content)},
            })},
            {"range", lsp_range_json(span_to_lsp_range(*doc.source, ct->span))},
        });
    }

    JsonOut handle_definition(const DocumentState& doc, std::size_t line, std::size_t character) {
        auto ct = find_token_at(doc, line, character);
        if (!ct) return JsonOut::null_val();

        auto loc = [](const std::string& uri, const LspRange& range) {
            return JsonOut::object({
                {"uri", JsonOut::str(uri)},
                {"range", lsp_range_json(range)},
            });
        };

        auto resolve_var_type = [&](const std::string& vname) -> std::string {
            if (const auto* sd = find_scoped_def(doc.symbols, vname, line, character)) {
                if (!sd->type_hint.empty()) return sd->type_hint;
            }
            if (auto vit = doc.symbols.var_types.find(vname); vit != doc.symbols.var_types.end()) {
                return vit->second;
            }
            return {};
        };

        if (!ct->qualifier.empty()) {
            // 1. Cross-file imported module export: `helper.add`, `pkg.sub.fn`
            if (auto mit = doc.imported_modules.find(ct->qualifier); mit != doc.imported_modules.end()) {
                if (auto eit = mit->second.exports.find(ct->name); eit != mit->second.exports.end()) {
                    return loc(eit->second.uri, eit->second.range);
                }
            }
            // 2. Cross-file imported enum variant or type member: `helper.Role.Admin`
            if (auto last_dot = ct->qualifier.rfind('.'); last_dot != std::string::npos) {
                std::string mod_alias = ct->qualifier.substr(0, last_dot);
                std::string type_name = ct->qualifier.substr(last_dot + 1);
                if (auto mit = doc.imported_modules.find(mod_alias); mit != doc.imported_modules.end()) {
                    if (auto meit = mit->second.member_exports.find(type_name + "." + ct->name); meit != mit->second.member_exports.end()) {
                        return loc(meit->second.uri, meit->second.range);
                    }
                }
            }
            // 3. Typed variable field or method: `u.name`, `u.greet()`, `self.name` (supports cross-file types!)
            std::string vtype = resolve_var_type(ct->qualifier);
            if (!vtype.empty()) {
                if (auto dot = vtype.rfind('.'); dot != std::string::npos) {
                    std::string mod_alias = vtype.substr(0, dot);
                    std::string type_name = vtype.substr(dot + 1);
                    if (auto mit = doc.imported_modules.find(mod_alias); mit != doc.imported_modules.end()) {
                        if (auto meit = mit->second.member_exports.find(type_name + "." + ct->name); meit != mit->second.member_exports.end()) {
                            return loc(meit->second.uri, meit->second.range);
                        }
                    }
                }
                if (auto dit = doc.symbols.local_defs.find(vtype + "." + ct->name); dit != doc.symbols.local_defs.end()) {
                    return loc(doc.uri, dit->second);
                }
                for (const auto& [alias, mod] : doc.imported_modules) {
                    if (auto meit = mod.member_exports.find(vtype + "." + ct->name); meit != mod.member_exports.end()) {
                        return loc(meit->second.uri, meit->second.range);
                    }
                }
            }
            // 4. Local enum variant or direct type member: `Role.Admin`, `User.title`
            if (auto dit = doc.symbols.local_defs.find(ct->qualifier + "." + ct->name); dit != doc.symbols.local_defs.end()) {
                return loc(doc.uri, dit->second);
            }
            // 5. Dotted import path itself (e.g. `import a.b` clicking on `b`)
            std::string full_dotted = ct->qualifier + "." + ct->name;
            if (auto mit = doc.imported_modules.find(full_dotted); mit != doc.imported_modules.end()) {
                return loc(mit->second.uri, LspRange{{0, 0}, {0, 0}});
            }
            // 6. Fallback: search unique matching member across local structs and imported modules
            for (const auto& [alias, mod] : doc.imported_modules) {
                for (const auto& [mkey, exp] : mod.member_exports) {
                    if (exp.name == ct->name) return loc(exp.uri, exp.range);
                }
            }
            for (const auto& [k, r] : doc.symbols.local_defs) {
                auto dot = k.rfind('.');
                if (dot != std::string::npos && k.substr(dot + 1) == ct->name) {
                    return loc(doc.uri, r);
                }
            }
        }

        // 1. Scoped local variable / parameter / MultiBinding / For / MatchArm / self
        if (const auto* sd = find_scoped_def(doc.symbols, ct->name, line, character)) {
            return loc(doc.uri, sd->def_range);
        }

        // 2. Imported module alias jumps to that module file
        if (auto mit = doc.imported_modules.find(ct->name); mit != doc.imported_modules.end()) {
            return loc(mit->second.uri, LspRange{{0, 0}, {0, 0}});
        }

        // 3. Top-level declaration in current file
        if (auto dit = doc.symbols.local_defs.find(ct->name); dit != doc.symbols.local_defs.end()) {
            return loc(doc.uri, dit->second);
        }

        // 4. Struct literal field initializer `FieldName: expr`
        if (ct->index + 1 < doc.tokens.size() && doc.tokens[ct->index + 1].kind == K::Colon) {
            for (const auto& [k, r] : doc.symbols.local_defs) {
                auto dot = k.rfind('.');
                if (dot != std::string::npos && k.substr(dot + 1) == ct->name) return loc(doc.uri, r);
            }
            for (const auto& [alias, mod] : doc.imported_modules) {
                for (const auto& [mkey, exp] : mod.member_exports) {
                    if (exp.name == ct->name) return loc(exp.uri, exp.range);
                }
            }
        }
        return JsonOut::null_val();
    }

    JsonOut handle_completion(const DocumentState& doc, std::size_t line, std::size_t character) {
        if (!doc.source) return JsonOut::array({});
        auto line_str = doc.source->line_text(line + 1);
        std::size_t byte_col = utf16_col_to_utf8_byte(line_str, character);
        std::string_view before = line_str.substr(0, byte_col);

        // Extract trailing identifier prefix
        std::size_t prefix_start = before.size();
        while (prefix_start > 0 && (std::isalnum(static_cast<unsigned char>(before[prefix_start - 1])) || before[prefix_start - 1] == '_')) {
            --prefix_start;
        }
        std::string_view prefix = before.substr(prefix_start);
        std::string_view head = before.substr(0, prefix_start);

        std::vector<JsonOut> items;
        std::unordered_set<std::string> seen;
        auto add_item = [&](const std::string& label, int kind, const std::string& detail = {}) {
            if (!starts_with_icase(label, prefix)) return;
            if (!seen.insert(label).second) return;
            std::vector<std::pair<std::string, JsonOut>> fields = {
                {"label", JsonOut::str(label)},
                {"kind", JsonOut::integer(kind)},
            };
            if (!detail.empty()) fields.emplace_back("detail", JsonOut::str(detail));
            items.push_back(JsonOut::object(fields));
        };

        // Check for dot-triggered completion: `<expr>.<prefix>`
        if (!head.empty() && head.back() == '.') {
            std::string_view before_dot = head.substr(0, head.size() - 1);
            // Check `import std.<prefix>`
            std::size_t qual_start = before_dot.size();
            while (qual_start > 0 && (std::isalnum(static_cast<unsigned char>(before_dot[qual_start - 1])) ||
                                      before_dot[qual_start - 1] == '_' || before_dot[qual_start - 1] == '.')) {
                --qual_start;
            }
            std::string qualifier(before_dot.substr(qual_start));

            if (qualifier == "std") {
                for (std::string_view mod : standard_modules) {
                    add_item(std::string(mod), 9, "standard library module");
                }
                for (std::string_view sdk_mod : {"alg_integer", "csv", "sse"}) {
                    add_item(std::string(sdk_mod), 9, "SDK Hua standard module");
                }
                return JsonOut::array(items);
            }

            // 1. Standard library module completion (all 32 modules / 422 functions)
            if (auto std_mod = resolve_std_module(doc, qualifier)) {
                for (const auto& fn : standard_functions()) {
                    if (fn.module == *std_mod) {
                        add_item(fn.name, 3, format_stdlib_sig(fn));
                    }
                }
                return JsonOut::array(items);
            }

            // 2. Imported user module exports completion
            if (auto mit = doc.imported_modules.find(qualifier); mit != doc.imported_modules.end()) {
                for (const auto& [ename, exp] : mit->second.exports) {
                    add_item(ename, exp.completion_kind, exp.detail);
                }
                return JsonOut::array(items);
            }

            // 3. Enum variants completion
            if (auto eit = doc.symbols.enum_variants.find(qualifier); eit != doc.symbols.enum_variants.end()) {
                for (const auto& v : eit->second) {
                    add_item(v, 20, qualifier + "." + v);
                }
                return JsonOut::array(items);
            }

            // 4. Typed variable/parameter struct fields + inherent methods completion
            std::string resolved_type;
            if (auto vit = doc.symbols.var_types.find(qualifier); vit != doc.symbols.var_types.end()) {
                resolved_type = vit->second;
            } else if (doc.symbols.structs.contains(qualifier)) {
                resolved_type = qualifier;
            }
            if (!resolved_type.empty()) {
                if (auto fit = doc.symbols.struct_fields.find(resolved_type); fit != doc.symbols.struct_fields.end()) {
                    for (const auto& [fname, ftype] : fit->second) {
                        add_item(fname, 5, ftype.empty() ? "field" : ftype);
                    }
                }
                if (auto mit = doc.symbols.struct_methods.find(resolved_type); mit != doc.symbols.struct_methods.end()) {
                    for (const auto& [mname, msig] : mit->second) {
                        add_item(mname, 2, msig);
                    }
                }
                return JsonOut::array(items);
            }

            return JsonOut::array(items);
        }

        // General identifier completion
        for (const auto& s : doc.symbols.structs) add_item(s, 22, "struct");
        for (const auto& e : doc.symbols.enums) add_item(e, 13, "enum");
        for (const auto& i : doc.symbols.interfaces) add_item(i, 8, "interface");
        for (const auto& f : doc.symbols.functions) {
            auto it = doc.symbols.hover_docs.find(f);
            add_item(f, 3, it != doc.symbols.hover_docs.end() ? it->second : "fn");
        }
        for (const auto& c : doc.symbols.consts) add_item(c, 21, "const");
        for (const auto& p : doc.symbols.params) add_item(p, 6, "parameter");
        for (const auto& p : doc.symbols.mut_params) add_item(p, 6, "mut parameter");
        for (const auto& v : doc.symbols.lets) add_item(v, 6, "let");
        for (const auto& v : doc.symbols.vars) add_item(v, 6, "var");
        for (const auto& [alias, target] : doc.symbols.import_targets) add_item(alias, 9, "import " + target);
        for (std::string_view bf : builtin_fn_list()) add_item(std::string(bf), 3, "builtin");
        for (std::string_view bt : builtin_type_list()) add_item(std::string(bt), 7, "builtin type");
        for (std::string_view kw : keyword_list()) add_item(std::string(kw), 14, "keyword");

        return JsonOut::array(items);
    }

    bool handle_message(const std::string& body) {
        const auto decoded = json_decode(body);
        if (!hua::holds_alternative<Cow<ResultValue>>(decoded.data)) return true;
        const auto& res = hua::cget<ResultValue>(decoded.data);
        if (!res.ok || !res.payload) return true;
        const auto* jv = hua::get_if<JsonValue>(&res.payload->data);
        if (!jv || !jv->data) return true;
        const auto& root = jv->data;

        auto method_opt = j_str(j_get(root, "method"));
        auto id_node = j_get(root, "id");
        auto params = j_get(root, "params");
        if (!method_opt) return true;
        const std::string& method = *method_opt;

        if (method == "initialize") {
            std::vector<JsonOut> tt;
            for (const auto& s : semantic_token_types()) tt.push_back(JsonOut::str(s));
            std::vector<JsonOut> tm;
            for (const auto& s : semantic_token_modifiers()) tm.push_back(JsonOut::str(s));

            send_response(j_id_raw(id_node), JsonOut::object({
                {"capabilities", JsonOut::object({
                    {"textDocumentSync", JsonOut::object({
                        {"openClose", JsonOut::boolean(true)},
                        {"change", JsonOut::integer(1)},
                        {"save", JsonOut::boolean(true)},
                    })},
                    {"semanticTokensProvider", JsonOut::object({
                        {"legend", JsonOut::object({
                            {"tokenTypes", JsonOut::array(tt)},
                            {"tokenModifiers", JsonOut::array(tm)},
                        })},
                        {"full", JsonOut::boolean(true)},
                    })},
                    {"hoverProvider", JsonOut::boolean(true)},
                    {"definitionProvider", JsonOut::boolean(true)},
                    {"completionProvider", JsonOut::object({
                        {"triggerCharacters", JsonOut::array({JsonOut::str(".")})},
                    })},
                    {"documentSymbolProvider", JsonOut::boolean(true)},
                })},
                {"serverInfo", JsonOut::object({
                    {"name", JsonOut::str("hua-lsp")},
                    {"version", JsonOut::str("0.3.0")},
                })},
            }));
            return true;
        }
        if (method == "initialized") return true;
        if (method == "shutdown") {
            shutdown_requested_ = true;
            send_response(j_id_raw(id_node), JsonOut::null_val());
            return true;
        }
        if (method == "exit") return false;

        if (method == "textDocument/didOpen") {
            auto td = j_get(params, "textDocument");
            auto uri = j_str(j_get(td, "uri"));
            auto text = j_str(j_get(td, "text"));
            if (uri && text) {
                auto& doc = docs_[*uri];
                doc.uri = *uri;
                doc.path = uri_to_path(*uri);
                doc.text = *text;
                doc.version = j_int(j_get(td, "version")).value_or(0);
                analyze_document(doc);
            }
            return true;
        }
        if (method == "textDocument/didChange") {
            auto td = j_get(params, "textDocument");
            auto uri = j_str(j_get(td, "uri"));
            auto changes = j_get(params, "contentChanges");
            if (uri && changes) {
                if (const auto* arr = std::get_if<JsonData::Array>(&changes->data); arr && !arr->empty()) {
                    if (auto text = j_str(j_get(arr->back(), "text"))) {
                        auto& doc = docs_[*uri];
                        doc.uri = *uri;
                        if (!doc.path) doc.path = uri_to_path(*uri);
                        doc.text = *text;
                        doc.version = j_int(j_get(td, "version")).value_or(doc.version + 1);
                        analyze_document(doc);
                    }
                }
            }
            return true;
        }
        if (method == "textDocument/didSave") {
            auto td = j_get(params, "textDocument");
            auto uri = j_str(j_get(td, "uri"));
            if (uri) {
                if (auto it = docs_.find(*uri); it != docs_.end()) {
                    if (auto text = j_str(j_get(params, "text"))) it->second.text = *text;
                    analyze_document(it->second);
                }
            }
            return true;
        }
        if (method == "textDocument/didClose") {
            auto td = j_get(params, "textDocument");
            auto uri = j_str(j_get(td, "uri"));
            if (uri) {
                if (auto it = docs_.find(*uri); it != docs_.end()) {
                    for (const auto& pub : it->second.published_uris) publish_diagnostics(pub, {});
                    publish_diagnostics(*uri, {});
                    docs_.erase(it);
                }
            }
            return true;
        }
        if (method == "textDocument/semanticTokens/full") {
            auto td = j_get(params, "textDocument");
            auto uri = j_str(j_get(td, "uri"));
            std::vector<JsonOut> nums;
            if (uri) {
                if (auto it = docs_.find(*uri); it != docs_.end() && it->second.source) {
                    auto encoded = build_semantic_tokens(*it->second.source, it->second.tokens, it->second.symbols);
                    nums.reserve(encoded.size());
                    for (auto v : encoded) nums.push_back(JsonOut::u_int(v));
                }
            }
            send_response(j_id_raw(id_node), JsonOut::object({{"data", JsonOut::array(nums)}}));
            return true;
        }
        if (method == "textDocument/hover") {
            auto td = j_get(params, "textDocument");
            auto uri = j_str(j_get(td, "uri"));
            auto pos = j_get(params, "position");
            auto line = j_int(j_get(pos, "line"));
            auto ch = j_int(j_get(pos, "character"));
            if (uri && line && ch && *line >= 0 && *ch >= 0) {
                if (auto it = docs_.find(*uri); it != docs_.end()) {
                    send_response(j_id_raw(id_node), handle_hover(it->second, static_cast<std::size_t>(*line), static_cast<std::size_t>(*ch)));
                    return true;
                }
            }
            send_response(j_id_raw(id_node), JsonOut::null_val());
            return true;
        }
        if (method == "textDocument/definition") {
            auto td = j_get(params, "textDocument");
            auto uri = j_str(j_get(td, "uri"));
            auto pos = j_get(params, "position");
            auto line = j_int(j_get(pos, "line"));
            auto ch = j_int(j_get(pos, "character"));
            if (uri && line && ch && *line >= 0 && *ch >= 0) {
                if (auto it = docs_.find(*uri); it != docs_.end()) {
                    send_response(j_id_raw(id_node), handle_definition(it->second, static_cast<std::size_t>(*line), static_cast<std::size_t>(*ch)));
                    return true;
                }
            }
            send_response(j_id_raw(id_node), JsonOut::null_val());
            return true;
        }
        if (method == "textDocument/completion") {
            auto td = j_get(params, "textDocument");
            auto uri = j_str(j_get(td, "uri"));
            auto pos = j_get(params, "position");
            auto line = j_int(j_get(pos, "line"));
            auto ch = j_int(j_get(pos, "character"));
            if (uri && line && ch && *line >= 0 && *ch >= 0) {
                if (auto it = docs_.find(*uri); it != docs_.end()) {
                    send_response(j_id_raw(id_node), handle_completion(it->second, static_cast<std::size_t>(*line), static_cast<std::size_t>(*ch)));
                    return true;
                }
            }
            send_response(j_id_raw(id_node), JsonOut::array({}));
            return true;
        }
        if (method == "textDocument/documentSymbol") {
            auto td = j_get(params, "textDocument");
            auto uri = j_str(j_get(td, "uri"));
            std::vector<JsonOut> out;
            if (uri) {
                if (auto it = docs_.find(*uri); it != docs_.end()) {
                    for (const auto& s : it->second.outline) out.push_back(doc_symbol_json(s));
                }
            }
            send_response(j_id_raw(id_node), JsonOut::array(out));
            return true;
        }

        if (id_node) {
            send_error(j_id_raw(id_node), -32601, "Method not found: " + method);
        }
        return true;
    }
};

} // namespace

int run_lsp(std::istream& in, std::ostream& out) {
#ifdef _WIN32
    if (&in == &std::cin) _setmode(_fileno(stdin), _O_BINARY);
    if (&out == &std::cout) _setmode(_fileno(stdout), _O_BINARY);
#endif
    LspServer server(in, out);
    return server.run();
}

} // namespace hua
