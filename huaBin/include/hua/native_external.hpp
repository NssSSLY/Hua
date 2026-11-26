#pragma once
#include "hua/external.hpp"
namespace hua {
class NativeExternalHost {
public:
    explicit NativeExternalHost(std::filesystem::path root);
    ~NativeExternalHost();
    NativeExternalHost(const NativeExternalHost&) = delete;
    NativeExternalHost& operator=(const NativeExternalHost&) = delete;
    void set_root(std::filesystem::path root) { root_ = std::move(root); }
    void add(ExternalModule module);
    void reset();
    void initialize(const std::string& id, const SourceSpan& site);
    Value call(const std::string& linked_name, const std::vector<Value>& args,
               const std::vector<bool>& contextual, const SourceSpan& site);
    bool contains(const std::string& linked_name) const;
    bool module_exists(const std::string& id) const;
    struct HandleTable;
private:
    struct Instance;
    std::filesystem::path root_;
    std::vector<ExternalModule> modules_;
    std::unordered_map<std::string, std::unique_ptr<Instance>> instances_;
    std::unique_ptr<HandleTable> handles_;
    std::uint32_t next_module_token_{1};
};
}
