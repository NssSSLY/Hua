#pragma once

#include "windows_command_line.hpp"
#include <filesystem>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace hua::native_run {
namespace fs = std::filesystem;

inline std::string utf8(const fs::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

class TemporaryProgram {
public:
    TemporaryProgram() {
        const auto temp = fs::temp_directory_path();
        std::random_device random;
        for (unsigned attempts = 0; attempts < 32; ++attempts) {
            const auto name = "hua-run-" + std::to_string(GetCurrentProcessId()) + "-"
                + std::to_string(random()) + "-" + std::to_string(random());
            auto directory = temp / name;
            if (fs::create_directory(directory)) {
                directory_ = std::move(directory);
                return;
            }
        }
        throw std::runtime_error("cannot reserve a temporary native program directory");
    }

    ~TemporaryProgram() {
        std::error_code ignored;
        fs::remove(image(), ignored);
        fs::remove(directory_, ignored);
    }

    TemporaryProgram(const TemporaryProgram&) = delete;
    TemporaryProgram& operator=(const TemporaryProgram&) = delete;
    fs::path image() const { return directory_ / "program.exe"; }

private:
    fs::path directory_;
};

inline BOOL WINAPI wait_for_child_on_interrupt(DWORD signal) {
    return signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT;
}

inline int execute(const fs::path& image, const std::vector<std::string>& args) {
    std::wstring command_line;
    windows_command_line::append_argument(command_line, image.native());
    // Native entry points consume this separator before exposing argv to Hua.
    windows_command_line::append_argument(command_line, L"--");
    for (std::size_t i = 5; i < args.size(); ++i) {
        windows_command_line::append_argument(command_line, windows_command_line::wide(args[i]));
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    const auto handler = SetConsoleCtrlHandler(wait_for_child_on_interrupt, TRUE);
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(image.c_str(), command_line.data(), nullptr, nullptr, TRUE, 0,
                        nullptr, nullptr, &startup, &child)) {
        if (handler) SetConsoleCtrlHandler(wait_for_child_on_interrupt, FALSE);
        throw std::runtime_error("cannot start the native Hua program");
    }
    CloseHandle(child.hThread);
    DWORD code = 0;
    const auto waited = WaitForSingleObject(child.hProcess, INFINITE);
    const auto have_code = waited == WAIT_OBJECT_0 && GetExitCodeProcess(child.hProcess, &code);
    CloseHandle(child.hProcess);
    if (handler) SetConsoleCtrlHandler(wait_for_child_on_interrupt, FALSE);
    if (!have_code) throw std::runtime_error("cannot obtain the native Hua program exit status");
    return static_cast<int>(code);
}
}
