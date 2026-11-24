#pragma once
#include "hua/value.hpp"
#include <filesystem>
#include <unordered_map>
namespace hua {
enum class ExternalKind : std::uint8_t { Native, Wasm };
struct ExternalExport {
    std::string name, linked_name, result;
    std::vector<std::string> parameters;
};
struct ExternalModule {
    std::string id, artifact;
    ExternalKind kind;
    SourceSpan span;
    std::vector<ExternalExport> exports;
    std::string wasm; // Embedded validated WASM bytes; native binaries are external.
};
class ExternalRegistry {
public:
    explicit ExternalRegistry(std::filesystem::path root);
    ~ExternalRegistry();
    ExternalRegistry(const ExternalRegistry&) = delete;
    ExternalRegistry& operator=(const ExternalRegistry&) = delete;
    void add(ExternalModule module);
    void reset();
    void initialize(const std::string& id, const SourceSpan& site);
    Value call(const std::string& name, const std::vector<Value>& args,
               const std::vector<bool>& contextual, const SourceSpan& site);
    bool contains(const std::string& name) const;
    bool module_exists(const std::string& id) const;
    const std::vector<ExternalModule>& modules() const { return modules_; }
    const std::filesystem::path& root() const { return root_; }
    static std::vector<ExternalExport> inspect_wasm(const std::string& bytes, const SourceSpan& site);
private:
    struct Instance;
    std::filesystem::path root_;
    std::vector<ExternalModule> modules_;
    std::unordered_map<std::string, std::unique_ptr<Instance>> instances_;
};
}
