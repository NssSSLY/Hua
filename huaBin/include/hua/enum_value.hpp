#pragma once
#include "hua/value.hpp"
namespace hua {
// Reconstructed from a declaration marked enum, never inferred from a value's fields.
struct EnumField { std::string variant, type; std::size_t index{}; };
struct EnumVariant { std::string name; std::map<std::string, std::string> fields; };
struct EnumLayout {
    std::string name;
    std::map<std::string, EnumVariant> variants;
    std::map<std::string, EnumField> fields;
};
std::shared_ptr<const EnumLayout> enum_layout(const Node& declaration);
void initialize_field(StructData&, const std::string&, Value, const SourceSpan&);
void require_complete_enum(const StructData&, const SourceSpan& = {});
}
