#pragma once

#ifdef _WIN32
#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

namespace hua::release_inputs {
namespace fs = std::filesystem;

inline std::string utf8(const fs::path& path) {
    const auto text = path.generic_u8string();
    return {text.begin(), text.end()};
}

struct Directory {
    HANDLE handle;
    ~Directory() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
};

inline void snapshot(const fs::path& root, const fs::path& exclude, std::vector<std::string>& rows) {
    Directory dir{CreateFileW(root.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    if (dir.handle == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            rows.push_back(utf8(root) + "|missing");
            return;
        }
        throw std::runtime_error("cannot enumerate SDK directory");
    }
    wchar_t kind[16]{};
    DWORD serial{};
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetVolumeInformationByHandleW(dir.handle, nullptr, 0, &serial, nullptr, nullptr, kind, 16)
            || std::wstring_view(kind) != L"NTFS"
            || !GetFileInformationByHandleEx(dir.handle, FileAttributeTagInfo, &attributes, sizeof(attributes))
            || !(attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            || (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        throw std::runtime_error("SDK fingerprint requires regular NTFS directories");
    }

    alignas(FILE_ID_BOTH_DIR_INFO) std::array<std::byte, 65536> buffer{};
    for (;;) {
        if (!GetFileInformationByHandleEx(dir.handle, FileIdBothDirectoryInfo, buffer.data(), buffer.size())) {
            if (GetLastError() == ERROR_NO_MORE_FILES) break;
            throw std::runtime_error("cannot read SDK directory entries");
        }
        const std::byte* at = buffer.data();
        for (;;) {
            const auto& entry = *reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(at);
            const std::wstring_view name(entry.FileName, entry.FileNameLength / sizeof(wchar_t));
            if (name != L"." && name != L"..") {
                const auto file = root / name;
                if (file != exclude) {
                    if (entry.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                        throw std::runtime_error("SDK fingerprint encountered a reparse point");
                    if (rows.size() >= 50000) throw std::runtime_error("SDK fingerprint entry limit");
                    if (entry.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                        rows.push_back(utf8(file) + "|directory");
                        snapshot(file, exclude, rows);
                    } else {
                        rows.push_back(utf8(file) + "|" + std::to_string(serial) + "|" +
                            std::to_string(entry.FileId.QuadPart) + "|" + std::to_string(entry.ChangeTime.QuadPart) +
                            "|" + std::to_string(entry.LastWriteTime.QuadPart) + "|" +
                            std::to_string(entry.EndOfFile.QuadPart));
                    }
                }
            }
            if (!entry.NextEntryOffset) break;
            at += entry.NextEntryOffset;
        }
    }
}

// Explicit inputs outside the header search directories use the same NTFS
// identity and change time as the entries encountered during enumeration.
inline void snapshot_file(const fs::path& file, std::vector<std::string>& rows) {
    Directory handle{CreateFileW(file.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
    FILE_BASIC_INFO change{};
    BY_HANDLE_FILE_INFORMATION identity{};
    FILE_STANDARD_INFO size{};
    wchar_t kind[16]{};
    if (handle.handle == INVALID_HANDLE_VALUE
            || !GetVolumeInformationByHandleW(handle.handle, nullptr, 0, nullptr, nullptr, nullptr, kind, 16)
            || std::wstring_view(kind) != L"NTFS"
            || !GetFileInformationByHandle(handle.handle, &identity)
            || !GetFileInformationByHandleEx(handle.handle, FileBasicInfo, &change, sizeof(change))
            || !GetFileInformationByHandleEx(handle.handle, FileStandardInfo, &size, sizeof(size))
            || (change.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY))) {
        throw std::runtime_error("SDK fingerprint requires regular NTFS files");
    }
    rows.push_back(utf8(file) + "|" + std::to_string(identity.dwVolumeSerialNumber) + "|"
        + std::to_string(identity.nFileIndexHigh) + "|" + std::to_string(identity.nFileIndexLow) + "|"
        + std::to_string(change.ChangeTime.QuadPart) + "|" + std::to_string(change.LastWriteTime.QuadPart)
        + "|" + std::to_string(size.EndOfFile.QuadPart));
}

inline fs::path checked_path(const std::string& value) {
    const auto path = fs::path(std::u8string(value.begin(), value.end())).lexically_normal();
    if (!path.is_absolute() || GetDriveTypeW(path.root_path().c_str()) != DRIVE_FIXED)
        throw std::runtime_error("SDK fingerprint requires local fixed-drive paths");
    return path;
}

inline void write_rows(std::vector<std::string>& rows) {
    std::sort(rows.begin(), rows.end());
    for (const auto& row : rows) std::cout << row << '\n';
}

inline int grouped_command(const std::vector<std::string>& arguments) {
    try {
        std::vector<std::string> rows;
        const auto linker = checked_path(arguments[2]);
        bool compiler = true;
        for (std::size_t i = 3; i < arguments.size(); ++i) {
            const auto& item = arguments[i];
            if (item == "|") {
                write_rows(rows);
                std::cout << "|\n";
                rows.clear();
                compiler = false;
            } else if (item.starts_with("f:")) {
                snapshot_file(checked_path(item.substr(2)), rows);
            } else {
                snapshot(checked_path(item), compiler ? linker : fs::path{}, rows);
            }
        }
        write_rows(rows);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

inline int command(const std::vector<std::string>& arguments) {
    try {
        const auto path = [](const std::string& value) {
            return fs::path(std::u8string(value.begin(), value.end())).lexically_normal();
        };
        const auto exclude = path(arguments[2]);
        std::vector<std::string> rows;
        for (std::size_t i = 3; i < arguments.size(); ++i) {
            const auto dir = path(arguments[i]);
            if (!dir.is_absolute() || GetDriveTypeW(dir.root_path().c_str()) != DRIVE_FIXED)
                throw std::runtime_error("SDK fingerprint requires local fixed-drive paths");
            snapshot(dir, exclude, rows);
        }
        std::sort(rows.begin(), rows.end());
        for (const auto& row : rows) std::cout << row << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
}
#endif
