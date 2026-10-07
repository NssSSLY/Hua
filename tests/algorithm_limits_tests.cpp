#include "hua/runtime.hpp"
#include "hua/tasks.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace hua;
namespace {
void check(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
template<class F> void diagnostic(const char* code, F operation) {
    try { operation(); } catch (const Diagnostic& error) { check(error.code() == code, "wrong diagnostic"); return; }
    throw std::runtime_error("missing diagnostic");
}
Value call(const char* name, std::vector<Value> args, RuntimeContext* context = nullptr) {
    return invoke_builtin(name, args, {}, std::cout, {}, {}, context);
}
}
int main() {
    SliceValue too_many; too_many.length = 1000001; too_many.element_type = "int";
    diagnostic("E4099", [&] { call("$std$alg$sorted", {Value(too_many)}); });
    auto storage = managed<std::vector<Value>>();
    storage->emplace_back(std::string(16 * 1024 * 1024, 'x'));
    storage->emplace_back(std::string("x"));
    SliceValue text{storage, 0, 2, true, "string"};
    diagnostic("E4099", [&] { call("$std$alg$reverse", {Value(text)}); });
    storage->clear();
    for (int i = 0; i < 16; ++i) storage->emplace_back(std::string(1024 * 1024, 'x'));
    text.length = 16;
    diagnostic("E4099", [&] { call("$std$alg$sorted", {Value(text)}); });
    RuntimeContext context; context.cancellation = std::make_shared<Cancellation>();
    context.cancellation->requested = true;
    diagnostic("E4101", [&] { call("$std$time$monotonic_ns", {}, &context); });
    diagnostic("E4101", [&] { call("$std$alg$sorted", {Value(text)}, &context); });
    diagnostic("E4101", [&] { call("$std$math$floor", {Value(1.0)}, &context); });
    diagnostic("E4101", [&] { call("$std$os$getenv", {Value(std::string("HUA_TEST"))}, &context); });
    // Independently assert signed-zero stability and rounding, not just printed equality.
    auto input = managed<std::vector<Value>>(); input->emplace_back(0.0); input->emplace_back(-0.0);
    auto result = result_payload(call("$std$alg$sorted", {Value(SliceValue{input,0,2,false,"float"})}), true, {});
    auto values = std::get<SliceValue>(result.data);
    check(!std::signbit(std::get<double>(sequence_read(values,0).data)), "unstable positive zero");
    check(std::signbit(std::get<double>(sequence_read(values,1).data)), "unstable negative zero");
    auto rounded = result_payload(call("$std$math$round", {Value(-0.5)}),true,{});
    check(std::signbit(std::get<double>(rounded.data)), "negative zero round");
    auto bad = call("$std$os$getenv", {Value(std::string(32768,'x'))});
    check(!std::get<ResultValue>(bad.data).ok, "environment name bound");
    diagnostic("E4003", [&] { call("$std$math$floor", {convert_numeric(Value(1.0),"f32",{},true)}); });
    diagnostic("E4003", [&] { call("$std$alg$contains", {Value(SliceValue{input,0,2,false,"float"}),Value(std::int64_t{0})}); });
    auto invalid = call("$std$math$floor", {Value(std::numeric_limits<double>::quiet_NaN())});
    check(!std::get<ResultValue>(invalid.data).ok, "non-finite math input");
    std::cout << "14 resource/cancellation/rounding/stability/type checks passed\n";

}
