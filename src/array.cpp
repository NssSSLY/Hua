#include "hua/value.hpp"
#include <bit>
#include <charconv>
#include <limits>
namespace hua {
namespace {
std::pair<std::size_t, std::string> array_type(std::string_view t) {
  auto close = t.find(']');
  std::size_t count{};
  if (close == std::string_view::npos || close < 2)
    return {};
  auto [end, err] = std::from_chars(t.data() + 1, t.data() + close, count);
  if (err != std::errc{} || end != t.data() + close || count > 1000000)
    return {};
  return {count, std::string(t.substr(close + 1))};
}
std::uint64_t read_bits(const std::string &bytes, std::size_t offset,
                        std::size_t width) {
  std::uint64_t result = 0;
  for (std::size_t i = 0; i < width; ++i)
    result |= static_cast<std::uint64_t>(
                  static_cast<unsigned char>(bytes.at(offset + i)))
              << (8 * i);
  return result;
}
void write_bits(std::string &bytes, std::size_t offset, std::size_t width,
                std::uint64_t value) {
  for (std::size_t i = 0; i < width; ++i)
    bytes.at(offset + i) = static_cast<char>((value >> (8 * i)) & 255);
}
Value load(const std::shared_ptr<std::string> &bytes, std::size_t offset,
           const std::string &type) {
  if (type.starts_with('[')) {
    auto [count, element] = array_type(type);
    SliceValue child{};
    child.length = count;
    child.element_type = element;
    child.packed = bytes;
    child.byte_offset = offset;
    child.fixed = true;
    return Value(child);
  }
  auto width = *packed_width(type), bits = read_bits(*bytes, offset, width);
  if (type == "bool")
    return Value(bits != 0);
  auto spec = *numeric_spec(type);
  Value value;
  if (spec.category == 'u')
    value = Value(NumericValue{"u64", bits});
  else if (spec.category == 'i') {
    if (width < 8 && (bits & (std::uint64_t{1} << (width * 8 - 1))))
      bits |= (~std::uint64_t{}) << (width * 8);
    value = Value(std::bit_cast<std::int64_t>(bits));
  } else
    value = Value(width == 4 ? static_cast<double>(std::bit_cast<float>(
                                   static_cast<std::uint32_t>(bits)))
                             : std::bit_cast<double>(bits));
  return convert_numeric(value, type, {}, true);
}
void store(std::string &bytes, std::size_t offset, const std::string &type,
           const Value &value) {
  if (type.starts_with('[')) {
    const auto &sequence = std::get<SliceValue>(value.data);
    auto [count, element] = array_type(type);
    auto stride = *packed_width(element);
    for (std::size_t i = 0; i < count; ++i)
      store(bytes, offset + i * stride, element, sequence_read(sequence, i));
    return;
  }
  auto width = *packed_width(type);
  std::uint64_t bits{};
  if (type == "bool")
    bits = std::get<bool>(value.data);
  else {
    auto spec = *numeric_spec(type);
    auto numeric = convert_numeric(value,
                                   spec.category == 'u'   ? "u64"
                                   : spec.category == 'i' ? "int"
                                                          : "float",
                                   {}, true);
    if (spec.category == 'u')
      bits =
          std::get<std::uint64_t>(std::get<NumericValue>(numeric.data).number);
    else if (spec.category == 'i')
      bits = std::bit_cast<std::uint64_t>(std::get<std::int64_t>(numeric.data));
    else {
      auto real = std::get<double>(numeric.data);
      bits = width == 4 ? std::bit_cast<std::uint32_t>(static_cast<float>(real))
                        : std::bit_cast<std::uint64_t>(real);
    }
  }
  write_bits(bytes, offset, width, bits);
}
} // namespace
std::optional<std::size_t> packed_width(std::string_view type) {
  if (type == "bool")
    return 1;
  if (auto spec = numeric_spec(type))
    return spec->bits / 8;
  if (type.starts_with('[') && !type.starts_with("[]")) {
    auto [count, element] = array_type(type);
    auto stride = packed_width(element);
    if (stride && count <= 134217728 / (*stride ? *stride : 1))
      return count * (*stride);
  }
  return {};
}
Value sequence_read(const SliceValue &s, std::size_t index) {
  if (index >= s.length)
    runtime_error({}, "array index out of bounds", "E4006");
  auto value = s.packed
                   ? load(s.packed,
                          s.byte_offset + (s.start + index) *
                                              (*packed_width(s.element_type)),
                          s.element_type)
                   : s.storage->at(s.start + index);
  return s.writable ? value : read_only(std::move(value));
}
void sequence_write(const SliceValue &s, std::size_t index,
                    const Value &value) {
  if (!s.writable)
    runtime_error({}, "cannot mutate a read-only array/view", "E4007");
  if (s.packed)
    store(*s.packed,
          s.byte_offset + (s.start + index) * (*packed_width(s.element_type)),
          s.element_type, value);
  else
    s.storage->at(s.start + index) = copy_value(value);
}
SliceValue fixed_array(SliceValue s, const std::string &element,
                       const SourceSpan &span, bool contextual) {
  if (s.fixed && s.element_type == element)
    return s;
  SliceValue result{};
  result.length = s.length;
  result.writable = s.writable;
  result.element_type = element;
  result.fixed = true;
  auto stride = packed_width(element);
  if (stride) {
    if (s.length > 134217728 / (*stride ? *stride : 1))
      runtime_error(span, "fixed array storage exceeds 128 MiB", "E4099");
    result.packed = managed<std::string>(s.length * (*stride), '\0');
  } else
    result.storage = managed<std::vector<Value>>(s.length);
  // Construction validates before publishing the new backing storage.
  result.writable = true;
  for (std::size_t i = 0; i < s.length; ++i)
    sequence_write(
        result, i,
        enforce_type(sequence_read(s, i), element, span, contextual && i < s.contextual.size() && s.contextual[i]));
  result.writable = s.writable;
  return result;
}
std::size_t value_size(const Value &v, const SourceSpan &span) {
  if (auto sequence = std::get_if<SliceValue>(&v.data)) {
    if (!sequence->fixed)
      runtime_error(span, "sizeof requires a numeric/bool value or fixed array",
                    "E4003");
    if (auto stride = packed_width(sequence->element_type))
      return sequence->length * (*stride);
    return sequence->length * sizeof(Value);
  }
  if (auto width = packed_width(value_type(v)))
    return *width;
  runtime_error(span, "sizeof requires a numeric/bool value or fixed array",
                "E4003");
}
std::size_t value_alignment(const Value &v, const SourceSpan &span) {
  if (auto sequence = std::get_if<SliceValue>(&v.data)) {
    if (!sequence->fixed)
      runtime_error(span, "alignof requires a fixed array or scalar", "E4003");
    auto type = sequence->element_type;
    while (type.starts_with('['))
      type = type.substr(type.find(']') + 1);
    if (auto width = packed_width(type))
      return *width;
    return alignof(Value);
  }
  return value_size(v, span);
}
} // namespace hua
