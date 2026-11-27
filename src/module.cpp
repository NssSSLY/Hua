#include "hua/module.hpp"
#include "hua/lowering.hpp"
#include "hua/lexer.hpp"
#include "hua/parser.hpp"
#include "hua/value.hpp"
#include "hua/runtime.hpp"
#include "hua/json.hpp"
#include <cstdlib>
#include <algorithm>
#include <cwctype>
#include <fstream>
#include <functional>
#include <optional>
#include <unordered_set>
namespace hua {
namespace {
using N=NodeKind;
std::string utf8(const std::filesystem::path& p){auto s=p.u8string();return {s.begin(),s.end()};}
std::string key(const std::filesystem::path& p) {
#ifdef _WIN32
    auto s=p.wstring();std::transform(s.begin(),s.end(),s.begin(),[](wchar_t c){return std::towlower(c);});
    return utf8(std::filesystem::path(s));
#else
    return utf8(p);
#endif
}
[[noreturn]] void error(const SourceSpan& s,const std::string& reason,std::string code="E5001") {throw Diagnostic(std::move(code),s,reason);}
JsonData::Pointer json_member(const JsonData::Pointer& value, const std::string& name) {
    if (!value) return {};
    const auto* object = std::get_if<JsonData::Object>(&value->data);
    if (!object) return {};
    const auto found = object->find(name);
    return found == object->end() ? nullptr : found->second;
}
bool json_text_equals(const JsonData::Pointer& value, std::string_view text) {
    if (!value) return false;
    const auto* string = std::get_if<std::string>(&value->data);
    return string && *string == text;
}
std::string dotted(const Node& n) {
    if(n.kind==N::Name)return n.text;
    if(n.kind==N::Member){auto path=dotted(*n.children[0]);if(!path.empty())return path+"."+n.text;}
    return "";
}
bool valid_package_name(std::string_view name) {
    if (name.empty() || name.size() > 128 || name.starts_with("__")) return false;
    auto alpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
    auto alnum = [&](char c) { return alpha(c) || (c >= '0' && c <= '9'); };
    if (!alpha(name[0])) return false;
    for (char c : name) if (!alnum(c)) return false;
    static const std::unordered_set<std::string_view> reserved = {
        "std", "int", "float", "bool", "string", "byte", "i8", "i16", "i32", "i64",
        "u8", "u16", "u32", "u64", "f32", "f64", "usize", "isize", "void",
        "Json", "Bytes", "Buffer", "List", "Deque", "Result", "Task", "NativeHandle", "map",
        "fn", "struct", "enum", "interface", "import", "as", "let", "var", "const",
        "pub", "mut", "async", "await", "spawn", "taskgroup", "parallel", "simd",
        "match", "if", "else", "while", "for", "in", "by", "break", "continue",
        "return", "defer", "true", "false", "nil", "unsafe"
    };
    return !reserved.contains(name);
}
// Reject duplicate JSON object keys before json_decode's map overwrites them.
bool unique_json_keys(std::string_view text) {
    std::size_t at = 0;
    auto space = [&]() { while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\r' || text[at] == '\n')) ++at; };
    auto read_str = [&]() -> std::optional<std::string> {
        if (at >= text.size() || text[at] != '"') return std::nullopt;
        const auto start = at++;
        while (at < text.size()) {
            char c = text[at++];
            if (c == '"') {
                const auto decoded = json_decode(std::string(text.substr(start, at - start)));
                const auto& r = hua::cget<ResultValue>(decoded.data);
                if (!r.ok) return std::nullopt;
                const auto& j = hua::get<JsonValue>(r.payload->data).data;
                const auto* s = j ? std::get_if<std::string>(&j->data) : nullptr;
                return s ? std::optional<std::string>(*s) : std::nullopt;
            }
            if (c == '\\') {
                if (at >= text.size()) return std::nullopt;
                ++at;
            }
        }
        return std::nullopt;
    };
    std::function<bool()> scan = [&]() -> bool {
        space();
        if (at >= text.size()) return true;
        char c = text[at];
        if (c == '"') return static_cast<bool>(read_str());
        if (c == '[') {
            ++at; space();
            if (at < text.size() && text[at] == ']') { ++at; return true; }
            while (at < text.size()) {
                if (!scan()) return false;
                space();
                if (at < text.size() && text[at] == ']') { ++at; return true; }
                if (at >= text.size() || text[at] != ',') return false;
                ++at;
            }
            return false;
        }
        if (c == '{') {
            ++at; space();
            std::unordered_set<std::string> seen;
            if (at < text.size() && text[at] == '}') { ++at; return true; }
            while (at < text.size()) {
                space();
                auto k = read_str();
                if (!k || !seen.insert(*k).second) return false;
                space();
                if (at >= text.size() || text[at] != ':') return false;
                ++at;
                if (!scan()) return false;
                space();
                if (at < text.size() && text[at] == '}') { ++at; return true; }
                if (at >= text.size() || text[at] != ',') return false;
                ++at;
            }
            return false;
        }
        while (at < text.size() && text[at] != ',' && text[at] != ']' && text[at] != '}' &&
               text[at] != ' ' && text[at] != '\t' && text[at] != '\r' && text[at] != '\n') ++at;
        return true;
    };
    return scan();
}
std::filesystem::path resolve_dep_dir(const std::filesystem::path& owner_root, const std::string& dep_name,
                                      const std::string& rel_path, const SourceSpan& site) {
    namespace fs = std::filesystem;
    if (rel_path.empty() || rel_path.size() > 1024 || rel_path.front() == '/' || rel_path.back() == '/' ||
        rel_path.find('\\') != std::string::npos || rel_path.find(':') != std::string::npos ||
        rel_path.find('\0') != std::string::npos) {
        error(site, "package dependency `" + dep_name + "` requires a normalized relative slash path", "E5004");
    }
    fs::path current = owner_root;
    std::error_code ec;
    std::size_t start = 0;
    while (start <= rel_path.size()) {
        auto slash = rel_path.find('/', start);
        auto seg = rel_path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (seg.empty() || seg == ".")
            error(site, "invalid path segment in package dependency `" + dep_name + "`", "E5004");
        if (seg == "..") {
            auto parent = current.parent_path();
            if (key(parent) == key(current))
                error(site, "package dependency `" + dep_name + "` escapes filesystem root", "E5001");
            current = parent;
        } else {
            auto next = current / fs::path(std::u8string(seg.begin(), seg.end()));
            auto st = fs::symlink_status(next, ec);
            if (ec) error(site, "cannot resolve package dependency `" + dep_name + "` at " + rel_path, "E5001");
            if (fs::is_symlink(st))
                error(site, "symlink dependency is outside this resolver contract: `" + dep_name + "`", "E5001");
            if (!fs::is_directory(st))
                error(site, "package dependency `" + dep_name + "` is not a directory", "E5001");
            current = next;
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    auto resolved = fs::canonical(current, ec);
    if (ec || key(resolved) != key(current.lexically_normal()))
        error(site, "symlink or junction dependency is outside this resolver contract: `" + dep_name + "`", "E5001");
    return resolved;
}
void check_local_dep_conflict(const std::filesystem::path& dir, const std::string& dep_name, const SourceSpan& site) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto base = dir / fs::path(std::u8string(dep_name.begin(), dep_name.end()));
    auto file = base; file += ".hua";
    auto native_f = base; native_f += ".huam";
    auto wasm_f = base; wasm_f += ".wasm";
    for (const auto& candidate : {file, base / "package.hua", native_f, base / "package.huam", wasm_f, base / "package.wasm"}) {
        if (fs::exists(candidate, ec) && !ec)
            error(site, "local module conflicts with package dependency `" + dep_name + "`", "E5004");
        ec.clear();
    }
    if (fs::is_directory(base, ec) && !ec)
        error(site, "local directory `" + dep_name + "` conflicts with package dependency `" + dep_name + "`", "E5004");
}
}
std::vector<const Source*> ModuleLoader::sources() const {std::vector<const Source*> out;for(const auto& m:modules_)if(m->source)out.push_back(m->source.get());return out;}
const Source* ModuleLoader::source(const std::string& id) const {
    for(const auto& module:modules_)if(module->source && module->source->filename()==id)return module->source.get();return nullptr;
}
void ModuleLoader::load_package_manifest(const std::string& expected_name, const std::filesystem::path& dir,
                                         const SourceSpan& site, std::vector<std::string>& active) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto manifest_path = dir / "hua.package.json";
    const auto st = fs::symlink_status(manifest_path, ec);
    if (ec || fs::is_symlink(st) || !fs::is_regular_file(st))
        error(site, "missing hua.package.json in package directory: " + utf8(dir), "E5001");
    const auto fsize = fs::file_size(manifest_path, ec);
    if (ec || fsize > 1024 * 1024)
        error(site, "hua.package.json exceeds 1 MiB limit", "E5005");
    std::ifstream in(manifest_path, std::ios::binary);
    std::string raw(static_cast<std::size_t>(fsize), '\0');
    if (!raw.empty() && !in.read(raw.data(), static_cast<std::streamsize>(fsize)))
        error(site, "cannot read hua.package.json: " + utf8(manifest_path), "E5001");

    const auto decoded = json_decode(raw);
    const auto& res = hua::cget<ResultValue>(decoded.data);
    if (!res.ok) error(site, "invalid JSON in hua.package.json: " + utf8(manifest_path), "E5004");
    if (!unique_json_keys(raw))
        error(site, "duplicate key in hua.package.json: " + utf8(manifest_path), "E5004");
    const auto& root_json = hua::get<JsonValue>(res.payload->data).data;
    const auto* obj = root_json ? std::get_if<JsonData::Object>(&root_json->data) : nullptr;
    if (!obj) error(site, "hua.package.json must be a JSON object", "E5004");
    for (const auto& [k, _] : *obj) {
        if (k != "schema" && k != "name" && k != "version" && k != "deps" && k != "native_libs")
            error(site, "unknown field `" + k + "` in hua.package.json", "E5004");
    }
    if (!json_text_equals(json_member(root_json, "schema"), "hua.package.v1"))
        error(site, "unsupported hua.package.json schema (expected hua.package.v1)", "E5004");
    const auto name_node = json_member(root_json, "name");
    const auto* name_ptr = name_node ? std::get_if<std::string>(&name_node->data) : nullptr;
    if (!name_ptr || !valid_package_name(*name_ptr))
        error(site, "invalid or reserved package name in hua.package.json", "E5004");
    const std::string pkg_name = *name_ptr;
    const auto ver_node = json_member(root_json, "version");
    const auto* ver_ptr = ver_node ? std::get_if<std::string>(&ver_node->data) : nullptr;
    if (!ver_ptr || ver_ptr->empty() || ver_ptr->size() > 128)
        error(site, "missing or invalid package version in hua.package.json", "E5004");

    const bool is_root = expected_name.empty();
    if (!is_root) {
        if (pkg_name != expected_name)
            error(site, "package manifest name `" + pkg_name + "` does not match dependency `" + expected_name + "`", "E5004");
        if (pkg_name == root_package_.name || key(dir) == key(root_))
            error(site, "package dependency `" + pkg_name + "` conflicts with root package", "E5004");
        const auto pkg_entry = dir / "package.hua";
        const auto entry_st = fs::symlink_status(pkg_entry, ec);
        if (ec || fs::is_symlink(entry_st) || !fs::is_regular_file(entry_st))
            error(site, "package `" + pkg_name + "` is missing package.hua", "E5001");
        if (std::find(active.begin(), active.end(), pkg_name) != active.end()) {
            std::string chain;
            for (const auto& item : active) { if (!chain.empty()) chain += " -> "; chain += item; }
            error(site, "cyclic package dependency: " + chain + " -> " + pkg_name, "E5002");
        }
        if (auto existing = packages_.find(pkg_name); existing != packages_.end()) {
            if (key(existing->second.root) != key(dir))
                error(site, "conflicting package dependency `" + pkg_name + "` points to different directories", "E5004");
            return;
        }
        for (const auto& [other_name, other_info] : packages_) {
            if (other_name != pkg_name && key(other_info.root) == key(dir))
                error(site, "package directory already registered under another name", "E5004");
        }
        if (packages_.size() >= 128)
            error(site, "package dependency graph exceeds 128 packages", "E5005");
    }

    PackageInfo info{pkg_name, *ver_ptr, dir, {}, {}};
    if (const auto libs_node = json_member(root_json, "native_libs")) {
        const auto* libs_obj = std::get_if<JsonData::Object>(&libs_node->data);
        if (!libs_obj) error(site, "`native_libs` in hua.package.json must be a JSON object", "E5004");
#ifdef _WIN32
        const std::string host_target = "windows-x86_64";
#elif __APPLE__
        const std::string host_target = "macos-arm64";
#else
        const std::string host_target = "linux-x86_64";
#endif
        for (const auto& [plat, plat_val] : *libs_obj) {
            if (plat != "windows-x86_64" && plat != "linux-x86_64" && plat != "macos-arm64" && plat != "macos-x86_64")
                error(site, "unsupported platform key `" + plat + "` in `native_libs`", "E5004");
            const auto* arr = plat_val ? std::get_if<JsonData::Array>(&plat_val->data) : nullptr;
            if (!arr) error(site, "`native_libs." + plat + "` must be a JSON array of filenames", "E5004");
            std::unordered_set<std::string> seen_libs;
            for (const auto& elem : *arr) {
                const auto* fname = elem ? std::get_if<std::string>(&elem->data) : nullptr;
                if (!fname || fname->empty() || fname->size() > 256 ||
                    fname->find('/') != std::string::npos || fname->find('\\') != std::string::npos ||
                    fname->find(':') != std::string::npos || *fname == "." || *fname == ".." ||
                    !seen_libs.insert(*fname).second)
                    error(site, "invalid or duplicate filename in `native_libs." + plat + "`", "E5004");
                if (plat == host_target) {
                    const auto lib_path = dir / fs::path(std::u8string(fname->begin(), fname->end()));
                    const auto lib_st = fs::symlink_status(lib_path, ec);
                    if (ec || lib_st.type() == fs::file_type::not_found)
                        error(site, "missing native library `" + *fname + "` declared in hua.package.json", "E7001");
                    if (fs::is_symlink(lib_st) || !fs::is_regular_file(lib_st))
                        error(site, "native library `" + *fname + "` must be a regular non-symlink file", "E5001");
                    info.native_libs.push_back(*fname);
                }
            }
        }
    }
    if (const auto deps_node = json_member(root_json, "deps")) {
        const auto* deps_obj = std::get_if<JsonData::Object>(&deps_node->data);
        if (!deps_obj) error(site, "`deps` in hua.package.json must be a JSON object", "E5004");
        for (const auto& [dep_key, dep_val] : *deps_obj) {
            if (!valid_package_name(dep_key))
                error(site, "invalid or reserved dependency name `" + dep_key + "`", "E5004");
            if (dep_key == pkg_name)
                error(site, "package `" + pkg_name + "` cannot depend on itself", "E5004");
            const auto* rel_ptr = dep_val ? std::get_if<std::string>(&dep_val->data) : nullptr;
            if (!rel_ptr) error(site, "dependency `" + dep_key + "` path must be a string", "E5004");
            check_local_dep_conflict(dir, dep_key, site);
            info.deps.emplace(dep_key, *rel_ptr);
        }
    }

    if (is_root) root_package_ = info;
    else packages_.emplace(pkg_name, info);

    active.push_back(pkg_name);
    const auto deps_copy = info.deps;
    for (const auto& [dep_key, rel_path] : deps_copy) {
        const auto dep_dir = resolve_dep_dir(dir, dep_key, rel_path, site);
        load_package_manifest(dep_key, dep_dir, site, active);
    }
    active.pop_back();
}
void ModuleLoader::load_manifest_graph(const SourceSpan& site) {
    namespace fs = std::filesystem;
    has_manifest_ = false;
    root_package_ = {};
    packages_.clear();
    std::error_code ec;
    const auto manifest_path = root_ / "hua.package.json";
    const auto st = fs::symlink_status(manifest_path, ec);
    if (ec || st.type() == fs::file_type::not_found) return;
    if (fs::is_symlink(st))
        error(site, "symlink hua.package.json is outside this resolver contract", "E5001");
    if (!fs::is_regular_file(st))
        error(site, "hua.package.json must be a regular file", "E5004");
    has_manifest_ = true;
    std::vector<std::string> active;
    load_package_manifest("", root_, site, active);
}
void ModuleLoader::set_overlay(const std::filesystem::path& path, std::string text) {
    std::error_code ec;
    auto normalized = std::filesystem::weakly_canonical(path, ec);
    overlays_[key(ec ? path : normalized)] = std::move(text);
}
const std::filesystem::path* ModuleLoader::file_path(const std::string& id) const {
    for (const auto& m : modules_)
        if (m->source && m->source->filename() == id) return &m->path;
    return nullptr;
}
const Node& ModuleLoader::load(const std::filesystem::path& entry) {
    linked_.reset();modules_.clear();cache_.clear();ordered_.clear();loading_.clear();bytes_=0;
    has_manifest_=false;root_package_={};packages_.clear();
    std::error_code ec;auto path=std::filesystem::canonical(entry,ec);
    if(ec){
        std::error_code wec;
        auto weak=std::filesystem::weakly_canonical(entry,wec);
        if(!wec && overlays_.contains(key(weak))) path=weak;
        else error({utf8(entry),0,0,1,1},"cannot read source file: "+utf8(entry),"E0001");
    }
    root_=path.parent_path();external_=std::make_shared<ExternalRegistry>(root_);
    const std::string entry_rel = utf8(path.filename());
    load_manifest_graph({entry_rel,0,0,1,1});
    const std::string entry_logical = has_manifest_ ? entry_rel : std::string{};
    auto& main=visit(path,{has_manifest_ ? entry_rel : utf8(path),0,0,1,1},true,{},entry_logical,{});
    linked_=std::make_unique<Node>(N::Program,main.tree->span);
    for(auto module:ordered_) {auto tree=link(*module);for(auto& n:tree->children)linked_->add(std::move(n));}
    lower_language(*linked_);
    return *linked_;
}
std::pair<std::filesystem::path, std::pair<std::string, std::string>>
ModuleLoader::resolve_in_context(const Module& caller, const std::string& name, const SourceSpan& site) const {
    if (!has_manifest_) {
        return {resolve(name, site), {{}, {}}};
    }
    const PackageInfo& ctx = caller.package.empty() ? root_package_ : packages_.at(caller.package);
    const auto dot = name.find('.');
    const std::string first = dot == std::string::npos ? name : name.substr(0, dot);
    const std::string rest = dot == std::string::npos ? std::string{} : name.substr(dot + 1);

    auto resolve_under = [&](const std::filesystem::path& base_root, const std::string& rel_mod,
                             const std::string& pkg_owner, bool package_entry) -> std::pair<std::filesystem::path, std::pair<std::string, std::string>> {
        namespace fs = std::filesystem;
        std::error_code ec;
        std::vector<std::pair<fs::path, std::string>> candidates;
        if (package_entry) {
            candidates.emplace_back(base_root / "package.hua", "package.hua");
        } else {
            std::string rel = rel_mod;
            std::replace(rel.begin(), rel.end(), '.', '/');
            auto base = base_root / fs::path(std::u8string(rel.begin(), rel.end()));
            auto f = base; f += ".hua";
            candidates.emplace_back(f, rel + ".hua");
            candidates.emplace_back(base / "package.hua", rel + "/package.hua");
            auto nf = base; nf += ".huam";
            candidates.emplace_back(nf, rel + ".huam");
            candidates.emplace_back(base / "package.huam", rel + "/package.huam");
            if (pkg_owner.empty()) {
                auto wf = base; wf += ".wasm";
                candidates.emplace_back(wf, rel + ".wasm");
                candidates.emplace_back(base / "package.wasm", rel + "/package.wasm");
            }
        }
        for (const auto& [cand, rel_id] : candidates) {
            // Walk segments from base_root to reject symlinks consistently with selfhost.
            fs::path cur = base_root;
            bool ok_walk = true;
            std::size_t s = 0;
            while (s < rel_id.size()) {
                auto sl = rel_id.find('/', s);
                auto part = rel_id.substr(s, sl == std::string::npos ? std::string::npos : sl - s);
                cur /= fs::path(std::u8string(part.begin(), part.end()));
                auto st = fs::symlink_status(cur, ec);
                if (ec) { ok_walk = false; break; }
                if (fs::is_symlink(st))
                    error(site, "symlink dependency is outside this resolver contract", "E5001");
                if (sl != std::string::npos && !fs::is_directory(st)) { ok_walk = false; break; }
                if (sl == std::string::npos && !fs::is_regular_file(st)) { ok_walk = false; break; }
                if (sl == std::string::npos) break;
                s = sl + 1;
            }
            ec.clear();
            if (!ok_walk) continue;
            auto canonical_cand = fs::canonical(cand, ec);
            if (ec) error(site, "cannot resolve module `" + name + "`", "E5001");
            auto rel_check = fs::relative(canonical_cand, base_root, ec);
            if (ec || rel_check.empty() || *rel_check.begin() == ".." ||
                key(canonical_cand) != key(cand.lexically_normal()))
                error(site, "module `" + name + "` resolves outside the package directory", "E5001");
            std::string logical = pkg_owner.empty() ? rel_id : ("@pkg/" + pkg_owner + "/" + rel_id);
            return {canonical_cand, {logical, pkg_owner}};
        }
        error(site, "cannot resolve module `" + name + "`", "E5001");
    };

    if (ctx.deps.contains(first)) {
        check_local_dep_conflict(ctx.root, first, site);
        const auto& target_pkg = packages_.at(first);
        return resolve_under(target_pkg.root, rest, target_pkg.name, rest.empty());
    }
    return resolve_under(ctx.root, name, caller.package, false);
}
std::filesystem::path ModuleLoader::resolve(const std::string& name,const SourceSpan& site) const {
    // Import grammar accepts ASCII identifier components only; it cannot express absolute paths or '..'.
    std::string relative=name;std::replace(relative.begin(),relative.end(),'.','/');
    auto base=root_/relative;auto file=base;file+=".hua";
    std::error_code ec;auto native_file=base;native_file+=".huam";auto wasm_file=base;wasm_file+=".wasm";
    for(const auto& candidate:{file,base/"package.hua",native_file,base/"package.huam",wasm_file,base/"package.wasm"}) {
        if(std::filesystem::is_regular_file(candidate,ec) && !ec) {
            auto path=std::filesystem::canonical(candidate,ec);if(ec)error(site,"cannot resolve module `"+name+"`");
            auto rel=std::filesystem::relative(path,root_,ec);
            if(ec || rel.empty() || *rel.begin()=="..")error(site,"module `"+name+"` resolves outside the entry source directory");
            return path;
        }
        ec.clear();
    }
    error(site,"cannot resolve module `"+name+"`; searched "+utf8(file)+" and "+utf8(base/"package.hua"));
}
ModuleLoader::Module& ModuleLoader::visit(const std::filesystem::path& path,const SourceSpan& site,bool entry,const std::string& sdk_id,const std::string& logical_id,const std::string& owner_package) {
    auto cache_key=sdk_id.empty() ? key(path) : sdk_id;
    if(auto found=cache_.find(cache_key);found!=cache_.end()) {
        if(found->second->state==Module::State::Loading) {
            std::string chain;
            for(auto module:loading_){
                if(!chain.empty())chain+=" -> ";
                chain += (has_manifest_ && module->source) ? module->source->filename() : utf8(module->path.filename());
            }
            const std::string tail = (!logical_id.empty() ? logical_id : utf8(path.filename()));
            error(site,"cyclic module import: "+chain+" -> "+tail,"E5002");
        }
        return *found->second;
    }
    if(modules_.size()>=128 || loading_.size()>=64)error(site,"module graph limit exceeded (128 files / 64 dependency levels)","E5005");
    std::string text;
    if(auto ov = overlays_.find(key(path)); ov != overlays_.end()) {
        if(ov->second.size() > 16*1024*1024)error(site,"source file is unreadable or exceeds 16 MiB",entry?"E0001":"E5005");
        if(bytes_+ov->second.size()>64*1024*1024)error(site,"module source graph exceeds 64 MiB","E5005");
        bytes_+=ov->second.size();
        text=ov->second;
    } else {
        std::ifstream input(path,std::ios::binary|std::ios::ate);
        if(!input)error(site,"cannot read source file: "+utf8(path),entry?"E0001":"E5001");
        auto size=input.tellg();
        if(size<0 || size>16*1024*1024)error(site,"source file is unreadable or exceeds 16 MiB",entry?"E0001":"E5005");
        if(bytes_+static_cast<std::size_t>(size)>64*1024*1024)error(site,"module source graph exceeds 64 MiB","E5005");
        bytes_+=static_cast<std::size_t>(size);text.assign(static_cast<std::size_t>(size),'\0');input.seekg(0);
        if(!text.empty() && !input.read(text.data(),size))error(site,"failed to read complete source file",entry?"E0001":"E5001");
    }
    auto owned=std::make_unique<Module>();owned->path=path;owned->package=owner_package;owned->prefix=entry?"":"$"+std::to_string(modules_.size())+"$";
    if (!sdk_id.empty()) owned->prefix="$sdk$std$alg_integer$";
    bool native=path.extension()==".huam",wasm=path.extension()==".wasm";
    std::string binary;std::vector<ExternalExport> wasm_exports;
    if(wasm) {
        binary=std::move(text);wasm_exports=ExternalRegistry::inspect_wasm(binary,site);
        text="# generated WASM interface\n";
        for(const auto& e:wasm_exports){text+="pub fn "+e.name+"(";for(std::size_t i=0;i<e.parameters.size();++i){if(i)text+=",";text+="a"+std::to_string(i)+" "+e.parameters[i];}text+=") "+e.result+" {}\n";}
    }
    const std::string source_name = !sdk_id.empty() ? sdk_id : (!logical_id.empty() ? logical_id : utf8(path));
    owned->source=std::make_unique<Source>(source_name,std::move(text));auto& module=*owned;
    modules_.push_back(std::move(owned));cache_.emplace(cache_key,&module);loading_.push_back(&module);
    std::uint32_t native_abi_ver = 1;
    if(native) {
        auto header_line = module.source->line_text(1);
        if(header_line=="HUA_NATIVE 1") native_abi_ver = 1;
        else if(header_line=="HUA_NATIVE 2") native_abi_ver = 2;
        else error(module.source->span(0,1),"native manifest requires HUA_NATIVE 1 or HUA_NATIVE 2 header","E7002");
        auto parse_text=module.source->text();parse_text[0]='#';Source interface(module.source->filename(),std::move(parse_text));
        module.tree=Parser(Lexer(interface).scan()).parse();
    }else module.tree=Parser(Lexer(*module.source).scan()).parse();
    if(native||wasm) {
        ExternalModule external{};external.id=module.prefix+"module";external.span=module.tree->span;
        external.kind=native?ExternalKind::Native:ExternalKind::Wasm;external.abi_version=native_abi_ver;external.wasm=std::move(binary);
        auto artifact=path;
        if(native) {
#ifdef _WIN32
            artifact.replace_extension(".dll");
#elif __APPLE__
            artifact.replace_extension(".dylib");
#else
            artifact.replace_extension(".so");
#endif
            std::error_code aec;
            auto ast = std::filesystem::symlink_status(artifact, aec);
            if(aec || std::filesystem::is_symlink(ast) || !std::filesystem::is_regular_file(ast))
                error(site,"native manifest binary is missing","E7001");
            external.source_artifact = artifact;
        }
        if (has_manifest_) {
            if (!owner_package.empty()) {
                external.artifact = "native/" + owner_package + "/" + utf8(artifact.filename());
            } else {
                auto rel_art = utf8(std::filesystem::relative(artifact, root_));
                std::replace(rel_art.begin(), rel_art.end(), '\\', '/');
                external.artifact = "native/_root/" + rel_art;
            }
        } else {
            external.artifact=utf8(std::filesystem::relative(artifact,root_));
            std::replace(external.artifact.begin(), external.artifact.end(), '\\', '/');
        }
        for(auto& n:module.tree->children) {
            if(n->kind!=N::Function || n->text.find(" pub")==std::string::npos || n->text.find(" unsafe")!=std::string::npos || declared_name(*n).find('.')!=std::string::npos || !n->children.back()->children.empty())error(n->span,"native/WASM interfaces require public bodyless scalar functions","E7003");
            ExternalExport e;e.name=declared_name(*n);e.linked_name=module.prefix+e.name;e.result="void";
            for(const auto& c:n->children) {if(c->kind==N::Parameter){if(c->children.size()!=1 || c->text.find(" mut")!=std::string::npos)error(c->span,"external parameters require scalar type annotations","E7003");e.parameters.push_back(type_name(*c->children[0]));}
                if(c->kind==N::ReturnTypes){if(c->children.size()!=1)error(c->span,"external functions require one return type","E7003");e.result=type_name(*c->children[0]);}}
            external.exports.push_back(std::move(e));n->text+=" external";
        }
        external_->add(std::move(external));
        module.tree->children.insert(module.tree->children.begin(),std::make_unique<Node>(N::ExternalInit,module.tree->span,module.prefix+"module"));
    }
    if(!native&&!wasm)lower_declarations(*module.tree);
    bool declarations=false;
    for(const auto& n:module.tree->children) {
        if(n->kind==N::Import) {
            if(declarations)error(n->span,"imports must precede all declarations and statements","E5004");
            auto binding=n->children.empty()?n->text:n->children[0]->text;
            if(module.imports.contains(binding))error(n->span,"duplicate import namespace `"+binding+"`","E5004");
            if (!sdk_id.empty() && !(n->text=="std" || n->text.starts_with("std.")))
                error(n->span,"SDK source modules can only import standard modules");
            if(n->text=="std"||n->text.starts_with("std."))module.imports.emplace(binding,&standard(n->text,n->span));
            else {try{auto [dependency, meta]=resolve_in_context(module,n->text,n->span);module.imports.emplace(binding,&visit(dependency,n->span,false,{},meta.first,meta.second));}catch(const Diagnostic& error){auto reported=error;reported.add_context(n->span,"imported from");throw reported;}}
        }else {
            declarations=true;if(n->kind==N::Struct&&(declared_name(*n)=="Json"||declared_name(*n)=="Bytes"||declared_name(*n)=="Buffer"||declared_name(*n)=="List"||declared_name(*n)=="Deque"||declared_name(*n)=="Task"||declared_name(*n)=="NativeHandle"))error(n->span,"reserved standard value type","E3002");
            if(n->kind==N::MultiBinding){for(const auto& p:n->children[0]->children)if(p->text!="_")module.globals.emplace(p->text,module.prefix+p->text);}
            if(n->kind==N::Function || n->kind==N::Struct || n->kind==N::Let || n->kind==N::Var || n->kind==N::Const) {
                auto name=declared_name(*n);module.globals.emplace(name,module.prefix+name);
                if(n->text.find(" pub")!=std::string::npos)module.exports.emplace(name,n.get());
            }
        }
    }
    for(const auto& [name,_]:module.imports)if(module.globals.contains(name.substr(0,name.find('.'))))
        error(module.tree->span,"import namespace conflicts with declaration `"+name+"`","E5004");
    loading_.pop_back();module.state=Module::State::Loaded;ordered_.push_back(&module);return module;
}
// Registry types are canonical (map<K,V>); the generated declaration text needs source syntax.
static std::string source_type(const std::string& type){
    if(type.size()>1 && type.ends_with('?'))return source_type(type.substr(0,type.size()-1))+"?";
    auto kv=type_arguments(type,"map");
    if(kv.size()==2)return "map["+source_type(kv[0])+"]"+source_type(kv[1]);
    auto res=type_arguments(type,"Result");
    if(res.size()==2)return "Result<"+source_type(res[0])+","+source_type(res[1])+">";
    return type;
}
ModuleLoader::Module& ModuleLoader::sdk_standard(const std::string& module, const std::string& relative, const SourceSpan& site) {
    namespace fs = std::filesystem;
    const std::string id = "@sdk/" + relative;
    if (auto found = cache_.find(id); found != cache_.end()) {
        if (found->second->state == Module::State::Loading)
            error(site, "cyclic module import: " + module, "E5002");
        return *found->second;
    }
#ifdef _WIN32
    const auto* configured = _wgetenv(L"HUA_HOME");
#else
    const auto* configured = std::getenv("HUA_HOME");
#endif
    if (!configured || !*configured)
        error(site, module + " requires HUA_HOME set to a Hua SDK root");

    const fs::path root(configured);
    if (!root.is_absolute()) error(site, "HUA_HOME must be an absolute SDK root");
    std::error_code ec;
    const auto canonical_root = fs::canonical(root, ec);
    if (ec) error(site, "invalid Hua SDK root");

    const auto manifest_path = canonical_root / "manifest.json";
    auto manifest_status = fs::symlink_status(manifest_path, ec);
    if (ec || !fs::is_regular_file(manifest_status))
        error(site, "cannot read Hua SDK manifest.json");
    const auto bytes = fs::file_size(manifest_path, ec);
    if (ec || bytes > 1024 * 1024)
        error(site, "invalid size for Hua SDK manifest.json");

    std::ifstream manifest_file(manifest_path, std::ios::binary);
    std::string text(static_cast<std::size_t>(bytes), '\0');
    if (!manifest_file.read(text.data(), static_cast<std::streamsize>(bytes)))
        error(site, "cannot read Hua SDK manifest.json");
    const auto decoded = json_decode(text);
    const auto& result = hua::cget<ResultValue>(decoded.data);
    if (!result.ok) error(site, "invalid JSON in Hua SDK manifest.json");
    const auto& manifest = hua::get<JsonValue>(result.payload->data).data;
    if (!json_text_equals(json_member(manifest, "schema"), "hua.sdk.v1") ||
        !json_text_equals(json_member(json_member(manifest, "source_modules"), module), relative) ||
        !json_text_equals(json_member(json_member(json_member(manifest, "files"), relative), "role"),
                          "stdlib_source"))
        error(site, "Hua SDK manifest does not register " + module);

    const auto path = (canonical_root / relative).make_preferred();
    const auto resolved = fs::canonical(path, ec);
    if (ec || key(path) != key(resolved) || !fs::is_regular_file(resolved, ec) || ec)
        error(site, "cannot read registered SDK source: " + relative);
    return visit(resolved, site, false, id);
}
ModuleLoader::Module& ModuleLoader::standard(const std::string& name,const SourceSpan& site) {
    if (name == "std.alg_integer") return sdk_standard("std.alg_integer", "src/std/alg_integer.hua", site);
    if (name == "std.sse") return sdk_standard("std.sse", "src/std/sse.hua", site);
    if (name == "std.csv") return sdk_standard("std.csv", "src/std/csv.hua", site);
    auto id="builtin:"+name;if(auto found=cache_.find(id);found!=cache_.end())return *found->second;
    if(!standard_module(name))error(site,"unknown standard module `"+name+"`");
    if(modules_.size()>=128)error(site,"module graph limit exceeded","E5005");
    auto owned=std::make_unique<Module>();owned->standard=true;owned->state=Module::State::Loaded;
    std::string text="# built-in standard interface; implemented by the shared runtime\n";
    for(const auto& f:standard_functions())if(name=="std."+f.module){text+="pub fn "+f.name+"(";for(std::size_t i=0;i<f.parameters.size();++i){if(i)text+=",";text+="a"+std::to_string(i);if(!f.parameters[i].empty())text+=" "+source_type(f.parameters[i]);}text+=") "+source_type(f.result)+" {}\n";owned->globals.emplace(f.name,"$core$"+standard_name(f));}
    if(name=="std.file"){text+="pub struct File {}\npub struct IOFailure {}\n";owned->globals.emplace("File","std.file.File");owned->globals.emplace("IOFailure","std.file.IOFailure");}
    if(name=="std.stream"){text+="pub struct Reader {}\npub struct Writer {}\n";owned->globals.emplace("Reader","std.stream.Reader");owned->globals.emplace("Writer","std.stream.Writer");}
    if(name=="std.http"){text+="pub struct Response {}\npub struct StreamResponse {}\n";owned->globals.emplace("Response","std.http.Response");owned->globals.emplace("StreamResponse","std.http.StreamResponse");}
    if(name=="std.regex"){text+="pub struct Pattern {}\n";owned->globals.emplace("Pattern","std.regex.Pattern");}
    if(name=="std.process"){text+="pub struct Child {}\n";owned->globals.emplace("Child","std.process.Child");}
    if(name=="std.random"){text+="pub struct Rng {}\n";owned->globals.emplace("Rng","std.random.Rng");}
    if(name=="std.error"){text+="pub struct Value {}\n";owned->globals.emplace("Value","std.error.Value");}
    if(name=="std.json"){text+="pub struct Value {}\n";owned->globals.emplace("Value","Json");}
    owned->source=std::make_unique<Source>("<"+name+">",std::move(text));owned->tree=Parser(Lexer(*owned->source).scan()).parse();
    for(const auto& n:owned->tree->children)owned->exports.emplace(declared_name(*n),n.get());
    auto& module=*owned;modules_.push_back(std::move(owned));cache_.emplace(id,&module);ordered_.push_back(&module);return module;
}
NodePtr ModuleLoader::link(Module& module) {
    if(module.standard)return std::make_unique<Node>(N::Program,module.tree->span);
    std::vector<std::unordered_set<std::string>> locals;
    std::vector<std::unordered_set<std::string>> local_types;
    auto local=[&](const std::string& name){for(auto i=locals.rbegin();i!=locals.rend();++i)if(i->contains(name))return true;return false;};
    auto imported=[&](const std::string& name,const SourceSpan& span,bool require_type)->std::string {
        auto root=name.substr(0,name.find('.'));if(!require_type && local(root))return "";
        // Resolve the most specific imported namespace, independent of hash/insertion order.
        const std::string* selected=nullptr;Module* target=nullptr;
        for(const auto& [prefix,candidate]:module.imports)
            if(name.starts_with(prefix+".") && (!selected || prefix.size()>selected->size())){selected=&prefix;target=candidate;}
        if(!target)return "";
        auto member=name.substr(selected->size()+1);
        auto found=target->exports.find(member);
        if(found==target->exports.end())error(span,"module `"+*selected+"` has no public export `"+member+"`","E5003");
        if(require_type && found->second->kind!=N::Struct)error(span,"module export `"+name+"` is not a struct type","E5003");
        if(!require_type && found->second->kind==N::Struct)error(span,"struct type `"+name+"` requires a named-field initializer","E5003");
        return target->globals.at(member);
    };
    auto type=[&](const std::string& name,const SourceSpan& span) {
        if(name.empty())return std::string{};
        for(auto i=local_types.rbegin();i!=local_types.rend();++i)if(i->contains(name))return name;
        if(auto linked=imported(name,span,true);!linked.empty())return linked;
        static const std::unordered_set<std::string> primitives={"int","float","bool","string","byte","i8","i16","i32","i64","u8","u16","u32","u64","f32","f64","usize","isize","void","Result","Json","Bytes","Buffer","Task","NativeHandle","std.error.Value","std.stream.Reader","std.stream.Writer","std.http.Response","std.http.StreamResponse","std.regex.Pattern","std.process.Child"};
        if(primitives.contains(name))return name;
        if(auto found=module.globals.find(name);found!=module.globals.end())return found->second;
        return module.prefix+name;
    };
    std::function<NodePtr(const Node&,bool)> rewrite;
    rewrite=[&](const Node& n,bool top)->NodePtr {
        if(n.kind==N::Import)error(n.span,"imports are allowed only at the start of a module","E5004");
        if(n.kind==N::Member) {
            auto path=dotted(n);auto linked=imported(path,n.span,false);
            if(!linked.empty())return std::make_unique<Node>(N::Name,n.span,linked);
            if(auto found=module.globals.find(path);found!=module.globals.end())return std::make_unique<Node>(N::Name,n.span,found->second);
        }
        auto out=std::make_unique<Node>(n.kind,n.span,n.text);
        if(n.kind==N::MatchArm){out->add(rewrite(*n.children[0],false));out->add(rewrite(*n.children[1],false));locals.emplace_back();for(const auto& p:n.children[1]->children)locals.back().insert(p->text);out->add(rewrite(*n.children[2],false));locals.pop_back();return out;}
        if(n.kind==N::Name) {
            if(n.text.find('.')!=std::string::npos){auto linked=imported(n.text,n.span,false);if(!linked.empty()){out->text=linked;return out;}}
            if(!local(n.text)) {
                for(const auto& [prefix,_]:module.imports)if(prefix.substr(0,prefix.find('.'))==n.text)
                    error(n.span,"module namespace `"+n.text+"` must be used as a qualified export","E5003");
                if(auto found=module.globals.find(n.text);found!=module.globals.end())out->text=found->second;
                else {
                    out->text=builtin_names().contains(n.text)?"$core$"+n.text:module.prefix+n.text;
                }
            }
            return out;
        }
        if(n.kind==N::MultiAssignment) {
            auto list=std::make_unique<Node>(N::BindingList,n.children[0]->span);for(const auto& t:n.children[0]->children)list->add(t->kind==N::Name&&t->text=="_"?std::make_unique<Node>(N::Name,t->span,"_"):rewrite(*t,false));out->add(std::move(list));out->add(rewrite(*n.children[1],false));return out;
        }
        if(n.kind==N::TypeName || n.kind==N::StructLiteral || (n.kind==N::GenericType&&n.text!="map"&&n.text!="Result"&&n.text!="List"&&n.text!="Deque"&&n.text!="Task"&&n.text!="ptr"&&n.text!="ref"))out->text=type(n.text,n.span);
        if(n.kind==N::Function) {
            if(!top && !locals.empty())locals.back().insert(declared_name(n));
            if(top)out->text=module.prefix+n.text;
            local_types.emplace_back();for(const auto& c:n.children)if(c->kind==N::GenericParameters)for(const auto& p:c->children)local_types.back().insert(p->text);
            locals.emplace_back();for(const auto& c:n.children)if(c->kind==N::Parameter)locals.back().insert(declared_name(*c));
            if(declared_name(n).find('.')!=std::string::npos)locals.back().insert("self");
            for(const auto& c:n.children)out->add(rewrite(*c,false));locals.pop_back();local_types.pop_back();return out;
        }
        if(n.kind==N::Struct){if(top)out->text=module.prefix+n.text;else if(!local_types.empty())local_types.back().insert(declared_name(n));local_types.emplace_back();for(const auto& c:n.children)if(c->kind==N::GenericParameters)for(const auto& p:c->children)local_types.back().insert(p->text);for(const auto& c:n.children)out->add(rewrite(*c,false));local_types.pop_back();return out;}
        if(n.kind==N::MultiBinding) {
            auto list=std::make_unique<Node>(N::BindingList,n.children[0]->span);
            for(const auto& p:n.children[0]->children){auto item=rewrite(*p,false);if(top&&p->text!="_")item->text=module.prefix+p->text;list->add(std::move(item));}
            out->add(std::move(list));out->add(rewrite(*n.children[1],false));
            if(!top&&!locals.empty())for(const auto& p:n.children[0]->children)if(p->text!="_")locals.back().insert(p->text);return out;
        }
        if(n.kind==N::Let || n.kind==N::Var || n.kind==N::Const) {
            if(top)out->text=module.prefix+n.text;
            for(const auto& c:n.children)out->add(rewrite(*c,false));
            if(!top && !locals.empty())locals.back().insert(n.text);return out;
        }
        if(n.kind==N::Block) {
            locals.emplace_back();local_types.emplace_back();for(const auto& c:n.children)out->add(rewrite(*c,false));locals.pop_back();local_types.pop_back();return out;
        }
        if(n.kind==N::For) {
            auto count=n.children.size()-2;
            for(std::size_t i=0;i<count;++i)out->add(std::make_unique<Node>(N::Name,n.children[i]->span,n.children[i]->text));
            out->add(rewrite(*n.children[count],false));locals.emplace_back();
            for(std::size_t i=0;i<count;++i)locals.back().insert(n.children[i]->text);
            out->add(rewrite(*n.children.back(),false));locals.pop_back();return out;
        }
        for(const auto& c:n.children)out->add(rewrite(*c,false));if(n.kind==N::Field&&n.text.starts_with('$')&&n.text.find('#')!=std::string::npos&&out->children.size()>1)out->text=n.text.substr(0,n.text.rfind('#')+1)+type_name(*out->children[1]);return out;
    };
    auto program=std::make_unique<Node>(N::Program,module.tree->span);
    for(const auto& n:module.tree->children)if(n->kind!=N::Import)program->add(rewrite(*n,true));return program;
}
}
