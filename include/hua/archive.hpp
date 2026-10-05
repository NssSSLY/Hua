#pragma once
#include "hua/bytecode.hpp"
#include <filesystem>
namespace hua {
struct BytecodeImage {
    std::vector<std::unique_ptr<Source>> sources;
    std::vector<NodePtr> declarations;
    SemanticModel model;
    Bytecode bytecode;
    const Source* source(const std::string& id) const;
};
void verify_bytecode(const Bytecode& code, const SemanticModel& model);
void write_huab(const std::filesystem::path& path, const Bytecode& code,
                const SemanticModel& model, const std::vector<const Source*>& sources);
BytecodeImage read_huab(const std::filesystem::path& path);
}
