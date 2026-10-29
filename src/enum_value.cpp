#include "hua/enum_value.hpp"
#include <charconv>
namespace hua {
namespace {
[[noreturn]] void invalid_layout(const Node& n) {
    runtime_error(n.span, "invalid enum layout", "E4003");
}
void payload_type(const Value& value, const EnumField& field, const SourceSpan& span) {
    // Constructor parameters have already performed contextual conversion. Checking
    // here must not copy/re-enforce a Result or fixed array a second time.
    if (field.type == "void") {
        if (value.data.index() != 0)
            runtime_error(span, "empty enum variant cannot carry a payload", "E4003");
    } else if (!compatible(field.type, value_type(value))) {
        runtime_error(span, "enum payload type mismatch", "E4003");
    }
}
}
std::shared_ptr<const EnumLayout> enum_layout(const Node& n) {
    if (n.kind != NodeKind::Struct || n.text.find(" enum") == std::string::npos)
        return {};
    auto layout = std::make_shared<EnumLayout>();
    layout->name = declared_name(n);
    bool tag = false;
    std::map<std::string, std::map<std::size_t, std::string>> positions;
    for (const auto& f : n.children) {
        if (f->kind != NodeKind::Field || f->children.empty())
            invalid_layout(n);
        if (f->text == "$case") {
            if (tag || type_name(*f->children[0]) != "string") invalid_layout(n);
            tag = true;
            continue;
        }
        auto first = f->text.find('#');
        auto second = first == std::string::npos ? first : f->text.find('#', first + 1);
        if (!f->text.starts_with('$') || first <= 1 || second == std::string::npos ||
            second == first + 1 || second + 1 == f->text.size())
            invalid_layout(n);
        auto variant = f->text.substr(1, first - 1);
        auto number = std::string_view(f->text).substr(first + 1, second - first - 1);
        std::size_t index{};
        auto [end, error] = std::from_chars(number.data(), number.data() + number.size(), index);
        if (error != std::errc{} || end != number.data() + number.size() ||
            index >= 4096 || std::to_string(index) != number)
            invalid_layout(n);
        auto type = canonical_type(f->text.substr(second + 1));
        auto annotation = canonical_type(type_name(*f->children[0]));
        if (!annotation.empty() && annotation != type) invalid_layout(n);
        if (!positions[variant].emplace(index, type).second ||
            !layout->fields.emplace(f->text, EnumField{variant, type, index}).second)
            invalid_layout(n);
        auto& item = layout->variants[variant];
        item.name = variant;
        item.fields.emplace(f->text, type);
    }
    if (!tag || positions.empty()) invalid_layout(n);
    for (const auto& [_, fields] : positions) {
        std::size_t expected = 0;
        for (const auto& [index, type] : fields) {
            if (index != expected++ || (type == "void" && (index != 0 || fields.size() != 1)))
                invalid_layout(n);
        }
    }
    return layout;
}
void initialize_field(StructData& object, const std::string& name, Value value,
                      const SourceSpan& span) {
    if (!object.enumeration) {
        object.fields.emplace(name, copy_value(std::move(value)));
        return;
    }
    const auto& layout = *object.enumeration;
    if (object.fields.contains(name))
        runtime_error(span, "duplicate enum field initializer", "E4003");
    if (name == "$case") {
        auto tag = hua::string_data(&value.data);
        if (!tag) runtime_error(span, "enum tag must be a string", "E4003");
        auto found = layout.variants.find(*tag);
        if (found == layout.variants.end())
            runtime_error(span, "unknown enum variant", "E4003");
        // Old HUAB can initialize the tag after payload fields. Preserve their
        // evaluation order, validate them, then discard only inactive nils.
        for (auto item = object.fields.begin(); item != object.fields.end();) {
            const auto& field = layout.fields.at(item->first);
            if (field.variant == *tag) {
                payload_type(item->second, field, span);
                ++item;
            } else {
                if (item->second.data.index() != 0)
                    runtime_error(span, "inactive enum payload must be nil", "E4003");
                item = object.fields.erase(item);
            }
        }
        object.variant = &found->second;
    } else {
        auto found = layout.fields.find(name);
        if (found == layout.fields.end())
            runtime_error(span, "unknown enum payload field", "E4003");
        if (object.variant) {
            if (found->second.variant != object.variant->name) {
                if (value.data.index() != 0)
                    runtime_error(span, "inactive enum payload must be nil", "E4003");
                return;
            }
            payload_type(value, found->second, span);
        }
    }
    object.fields.emplace(name, copy_value(std::move(value)));
}
void require_complete_enum(const StructData& object, const SourceSpan& span) {
    if (!object.enumeration) return;
    auto tag = object.fields.find("$case");
    auto text = tag == object.fields.end() ? nullptr : hua::string_data(&tag->second.data);
    if (!object.variant || !text || *text != object.variant->name ||
        object.fields.size() != object.variant->fields.size() + 1)
        runtime_error(span, "enum payload is incomplete", "E4003");
}
}
