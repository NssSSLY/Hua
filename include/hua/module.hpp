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
        std::filesystem::path path;
        std::string prefix;
        std::unique_ptr<Source> source;
        NodePtr tree;
        std::unordered_map<std::string, std::string> globals;
        std::unordered_map<std::string, const Node*> exports;
        std::unordered_map<std::string, Module*> imports;
    };
    std::filesystem::path root_;
    std::vector<std::unique_ptr<Module>> modules_;
    std::unordered_map<std::string, Module*> cache_;
    std::vector<Module*> ordered_, loading_;
    NodePtr linked_;
    std::size_t bytes_{};
    std::shared_ptr<ExternalRegistry> external_;
    Module& visit(const std::filesystem::path& path, const SourceSpan& site, bool entry = false);
    std::filesystem::path resolve(const std::string& name, const SourceSpan& site) const;
    NodePtr link(Module& module);
};
}
