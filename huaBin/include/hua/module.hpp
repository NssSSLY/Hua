#pragma once
#include "hua/ast.hpp"
#include "hua/external.hpp"
#include <filesystem>
#include <unordered_map>
namespace hua {
// Owns both source texts and linked nodes for the lifetime of semantic/VM metadata.
class ModuleLoader {
public:
    const Node& load(const std::filesystem::path& entry);
    const Source* source(const std::string& id) const;
    std::shared_ptr<ExternalRegistry> externals() const { return external_; }
    std::vector<const Source*> sources() const;
    std::size_t module_count() const { return modules_.size(); }
private:
    struct Module {
        enum class State { Loading, Loaded } state{State::Loading};
        bool standard{};
        std::string package;
        std::filesystem::path path;
        std::string prefix;
        std::unique_ptr<Source> source;
        NodePtr tree;
        std::unordered_map<std::string, std::string> globals;
        std::unordered_map<std::string, const Node*> exports;
        std::unordered_map<std::string, Module*> imports;
    };
    struct PackageInfo {
        std::string name;
        std::string version;
        std::filesystem::path root;
        std::unordered_map<std::string, std::string> deps;
        std::vector<std::string> native_libs;
    };
    std::filesystem::path root_;
    bool has_manifest_{};
    PackageInfo root_package_;
    std::unordered_map<std::string, PackageInfo> packages_;
    std::vector<std::unique_ptr<Module>> modules_;
    std::unordered_map<std::string, Module*> cache_;
    std::vector<Module*> ordered_, loading_;
    NodePtr linked_;
    std::size_t bytes_{};
    std::shared_ptr<ExternalRegistry> external_;
    void load_manifest_graph(const SourceSpan& site);
    void load_package_manifest(const std::string& expected_name, const std::filesystem::path& dir,
                               const SourceSpan& site, std::vector<std::string>& active);
    Module& visit(const std::filesystem::path& path, const SourceSpan& site, bool entry = false,
                  const std::string& sdk_id = {}, const std::string& logical_id = {},
                  const std::string& owner_package = {});
    std::pair<std::filesystem::path, std::pair<std::string, std::string>>
    resolve_in_context(const Module& caller, const std::string& name, const SourceSpan& site) const;
    std::filesystem::path resolve(const std::string& name, const SourceSpan& site) const;
    NodePtr link(Module& module);
    Module& sdk_standard(const SourceSpan& site);
    Module& standard(const std::string& name,const SourceSpan& site);
};
}
