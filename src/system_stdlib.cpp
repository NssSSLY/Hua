#include "hua/stdlib.hpp"
#include "hua/tasks.hpp"
#include <chrono>
#include <cstdlib>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif
#include <thread>

namespace hua {
namespace {
constexpr std::size_t max_text = 16 * 1024 * 1024, max_name = 32767;
Value environment_error(std::string message) {
    return standard_result(Value("ENV_ERROR: " + message), false, "string?");
}
Value environment(const std::string& name) {
    if (name.empty() || name.size() > max_name || name.find('\0') != std::string::npos ||
        name.find('=') != std::string::npos || !text_utf8_error(name).empty())
        return environment_error("invalid variable name");
#ifdef _WIN32
    auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()), nullptr, 0);
    if (!size) return environment_error("variable name conversion failed");
    std::wstring wide(size, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name.data(), static_cast<int>(name.size()), wide.data(), size))
        return environment_error("variable name conversion failed");
    // Foreign hosts may change their environment between the size and copy calls.
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        SetLastError(ERROR_SUCCESS);
        auto count = GetEnvironmentVariableW(wide.c_str(), nullptr, 0);
        if (!count) {
            auto error = GetLastError();
            if (error == ERROR_ENVVAR_NOT_FOUND) return standard_result(Value{}, true, "string?");
            if (error == ERROR_SUCCESS) return standard_result(Value(std::string{}), true, "string?");
            return environment_error("variable read failed");
        }
        if (count > max_text) return environment_error("value exceeds 16 MiB");
        std::wstring value(count, L'\0');
        SetLastError(ERROR_SUCCESS);
        auto actual = GetEnvironmentVariableW(wide.c_str(), value.data(), count);
        if (!actual) {
            auto error = GetLastError();
            if (error == ERROR_ENVVAR_NOT_FOUND) return standard_result(Value{}, true, "string?");
            if (error == ERROR_SUCCESS) return standard_result(Value(std::string{}), true, "string?");
            return environment_error("variable read failed");
        }
        if (actual >= count) continue;
        auto bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                        static_cast<int>(actual), nullptr, 0, nullptr, nullptr);
        if (!bytes) return environment_error("value is not valid Unicode");
        if (static_cast<std::size_t>(bytes) > max_text) return environment_error("value exceeds 16 MiB");
        std::string text(bytes, '\0');
        if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(actual),
                                text.data(), bytes, nullptr, nullptr))
            return environment_error("value conversion failed");
        return standard_result(Value(std::move(text)), true, "string?");
    }
    return environment_error("variable changed during read");
#else
    auto value = std::getenv(name.c_str());
    if (!value) return standard_result(Value{}, true, "string?");
    std::size_t length = 0;
    while (length <= max_text && value[length]) ++length;
    if (length > max_text) return environment_error("value exceeds 16 MiB");
    std::string text(value, length);
    if (!text_utf8_error(text).empty()) return environment_error("value is not valid UTF-8");
    return standard_result(Value(std::move(text)), true, "string?");
#endif
}
}
// std.os exit and platform information (since 16). Values describe the host that
// runs the program, not the one that built its HUAB.
Value invoke_os_standard(const StandardFunction& f, const std::vector<Value>& args, const SourceSpan& span, RuntimeContext* context) {
    if (args.size() != f.parameters.size()) runtime_error(span, "standard function argument count mismatch", "E4003");
    const auto& name = f.name;
    if (name == "exit") {
        auto code = std::get_if<std::int64_t>(&args[0].data);
        if (!code) runtime_error(span, "expected int", "E4003");
        if (*code < 0 || *code > 125) runtime_error(span, "os.exit code must be 0..125", "E4003");
        if (context && context->in_task) runtime_error(span, "os.exit is only allowed in the main execution", "E4104");
        if (context && context->exiting) runtime_error(span, "os.exit during exit cleanup", "E4104");
        if (context) context->exiting = true;
        throw ProgramExit{static_cast<int>(*code)};
    }
    if (name == "platform") {
#if defined(_WIN32)
        return Value(std::string("windows"));
#elif defined(__APPLE__)
        return Value(std::string("macos"));
#elif defined(__linux__)
        return Value(std::string("linux"));
#else
        return Value(std::string("other"));
#endif
    }
    if (name == "arch") {
#if defined(__x86_64__) || defined(_M_X64)
        return Value(std::string("x86_64"));
#elif defined(__aarch64__) || defined(_M_ARM64)
        return Value(std::string("aarch64"));
#elif defined(__i386__) || defined(_M_IX86)
        return Value(std::string("x86"));
#elif defined(__arm__) || defined(_M_ARM)
        return Value(std::string("arm"));
#else
        return Value(std::string("other"));
#endif
    }
    if (name == "pid") {
#ifdef _WIN32
        return Value(static_cast<std::int64_t>(GetCurrentProcessId()));
#else
        return Value(static_cast<std::int64_t>(getpid()));
#endif
    }
    if (name == "cpu_count") {
        auto n = std::thread::hardware_concurrency();
        return Value(static_cast<std::int64_t>(n ? n : 1));
    }
    runtime_error(span, "unknown os operation", "E4003");
}
// time.monotonic_ns and time.since_ns share this origin (first use in the process).
std::int64_t monotonic_now_ns(const SourceSpan& span) {
    static const auto origin = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::steady_clock::now() - origin;
    auto count = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    if (count < 0) runtime_error(span, "monotonic clock range exceeded", "E4002");
    return static_cast<std::int64_t>(count);
}
Value invoke_system_standard(const StandardFunction& f, const std::vector<Value>& args,
                             const SourceSpan& span, RuntimeContext* context) {
    task_checkpoint(context, span);
    if (f.module == "os") return environment(hua::string_value(args[0].data));
    if (f.name == "monotonic_ns") return Value(monotonic_now_ns(span));
    if (f.name == "unix_ms")
        return Value(static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()));
    runtime_error(span, "unknown clock operation", "E4003");
}
}
