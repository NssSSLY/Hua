#pragma once

#include <string>
#include <windows.h>

namespace hua::windows_command_line {
inline std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}
// Quoting that CommandLineToArgvW and the C runtime undo exactly.
inline void append_argument(std::wstring& line, const std::wstring& arg) {
    if (!line.empty()) line += L' ';
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) { line += arg; return; }
    line += L'"';
    for (std::size_t i = 0;; ++i) {
        std::size_t slashes = 0;
        while (i < arg.size() && arg[i] == L'\\') { ++i; ++slashes; }
        if (i == arg.size()) { line.append(slashes * 2, L'\\'); break; }
        if (arg[i] == L'"') { line.append(slashes * 2 + 1, L'\\'); line += L'"'; }
        else { line.append(slashes, L'\\'); line += arg[i]; }
    }
    line += L'"';
}
}
