// std.process (since 28): run a child to completion and collect its output. No shell;
// argv is passed as is. Pipes are drained concurrently, so a child that fills one cannot
// deadlock. The whole process tree is killed on timeout, output limit and cancellation.
// Helper threads only touch plain host strings; Values are built after they have joined.
#include "hua/stdlib.hpp"
#include "hua/tasks.hpp"
#include <atomic>
#include <chrono>
#include <map>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif
namespace hua {
namespace {
using I = std::int64_t;
using Clock = std::chrono::steady_clock;
constexpr std::size_t max_output = 128 * 1024 * 1024;

struct Request {
    std::vector<std::string> argv;
    const std::string* input{};
    I timeout_ms{};
    std::string cwd;
    std::map<std::string, std::string> env;
    const Cancellation* cancel{};
};
struct Outcome {
    I code{-1};
    std::string out, err, problem;
    bool canceled{};
};
bool has_nul(const std::string& text) { return text.find('\0') != std::string::npos; }

#ifdef _WIN32
std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}
// Quoting that CommandLineToArgvW and the C runtime undo exactly.
void append_argument(std::wstring& line, const std::wstring& arg) {
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
struct Handle {
    HANDLE value{};
    ~Handle() { reset(); }
    void reset() { if (value) { CloseHandle(value); value = nullptr; } }
};
std::wstring upper_key(std::wstring key) {
    if (!key.empty()) CharUpperBuffW(key.data(), static_cast<DWORD>(key.size()));
    return key;
}
// The current environment with overrides applied; names compare case-insensitively on Windows.
std::wstring environment_block(const std::map<std::string, std::string>& extra) {
    std::map<std::wstring, std::wstring> merged;
    auto* block = GetEnvironmentStringsW();
    for (auto* p = block; p && *p; p += wcslen(p) + 1) {
        const std::wstring entry(p);
        const auto at = entry.find(L'=', 1);
        if (at != std::wstring::npos) merged[upper_key(entry.substr(0, at))] = entry;
    }
    if (block) FreeEnvironmentStringsW(block);
    for (const auto& [k, v] : extra) merged[upper_key(wide(k))] = wide(k) + L'=' + wide(v);
    std::wstring out;
    for (const auto& [_, entry] : merged) { out += entry; out += L'\0'; }
    out += L'\0';
    return out;
}
void reader(HANDLE pipe, std::string& out, std::atomic<bool>& overflow) {
    char buffer[65536];
    DWORD got = 0;
    while (ReadFile(pipe, buffer, sizeof buffer, &got, nullptr) && got) {
        if (out.size() + got > max_output) { overflow = true; return; }
        out.append(buffer, got);
    }
}
void writer(HANDLE pipe, const std::string& data) {
    std::size_t at = 0;
    while (at < data.size()) {
        DWORD wrote = 0;
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(data.size() - at, 1 << 20));
        if (!WriteFile(pipe, data.data() + at, chunk, &wrote, nullptr)) return;
        at += wrote;
    }
}
Outcome run_child(const Request& request) {
    Outcome outcome;
    auto start_failed = [&](const char* step) { outcome.problem = std::string("PROCESS_START: ") + step + " failed (" + std::to_string(GetLastError()) + ")"; return outcome; };
    SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
    Handle in_read, in_write, out_read, out_write, err_read, err_write;
    if (!CreatePipe(&in_read.value, &in_write.value, &inherit, 0) || !CreatePipe(&out_read.value, &out_write.value, &inherit, 0) || !CreatePipe(&err_read.value, &err_write.value, &inherit, 0)) return start_failed("CreatePipe");
    SetHandleInformation(in_write.value, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(out_read.value, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(err_read.value, HANDLE_FLAG_INHERIT, 0);
    Handle job;
    job.value = CreateJobObjectW(nullptr, nullptr);
    if (!job.value) return start_failed("CreateJobObject");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof limits);
    // Inherit only this child's pipes, even when other Tasks spawn at the same time.
    HANDLE inherited[] = {in_read.value, out_write.value, err_write.value};
    SIZE_T attribute_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
    std::string attribute_storage(attribute_size, '\0');
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size)) return start_failed("InitializeProcThreadAttributeList");
    UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof inherited, nullptr, nullptr);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = in_read.value;
    startup.StartupInfo.hStdOutput = out_write.value;
    startup.StartupInfo.hStdError = err_write.value;
    startup.lpAttributeList = attributes;
    std::wstring line;
    for (const auto& arg : request.argv) append_argument(line, wide(arg));
    const auto block = environment_block(request.env);
    const auto directory = wide(request.cwd);
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
        const_cast<wchar_t*>(block.c_str()), directory.empty() ? nullptr : directory.c_str(), &startup.StartupInfo, &process);
    const auto start_error = GetLastError();
    DeleteProcThreadAttributeList(attributes);
    if (!created) { SetLastError(start_error); return start_failed("CreateProcess"); }
    Handle process_handle, thread_handle;
    process_handle.value = process.hProcess;
    thread_handle.value = process.hThread;
    AssignProcessToJobObject(job.value, process.hProcess);
    ResumeThread(process.hThread);
    in_read.reset();
    out_write.reset();
    err_write.reset();
    std::atomic<bool> overflow{false};
    HANDLE stdin_pipe = in_write.value;
    in_write.value = nullptr;
    std::thread feed([&] { writer(stdin_pipe, *request.input); CloseHandle(stdin_pipe); });
    std::thread drain_out([&] { reader(out_read.value, outcome.out, overflow); });
    std::thread drain_err([&] { reader(err_read.value, outcome.err, overflow); });
    const auto deadline = Clock::now() + std::chrono::milliseconds(request.timeout_ms);
    bool killed = false;
    auto kill_tree = [&](const char* why) { if (why) outcome.problem = why; TerminateJobObject(job.value, 1); killed = true; };
    while (WaitForSingleObject(process.hProcess, 10) == WAIT_TIMEOUT) {
        if (killed) continue;
        if (request.cancel && request.cancel->stopped()) { outcome.canceled = true; kill_tree(nullptr); }
        else if (overflow) kill_tree("PROCESS_OUTPUT_LIMIT: output exceeds 128 MiB");
        else if (request.timeout_ms > 0 && Clock::now() >= deadline) kill_tree("PROCESS_TIMEOUT: time limit exceeded");
    }
    if (!killed && overflow) kill_tree("PROCESS_OUTPUT_LIMIT: output exceeds 128 MiB");
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    // Descendants still hold the pipes open; ending the job lets the readers finish.
    TerminateJobObject(job.value, 1);
    feed.join();
    drain_out.join();
    drain_err.join();
    if (overflow && outcome.problem.empty()) outcome.problem = "PROCESS_OUTPUT_LIMIT: output exceeds 128 MiB";
    outcome.code = outcome.problem.empty() && !outcome.canceled ? static_cast<I>(code) : -1;
    return outcome;
}
#else
// POSIX path: not exercised on the development machine.
void close_fd(int& fd) { if (fd >= 0) { close(fd); fd = -1; } }
Outcome run_child(const Request& request) {
    Outcome outcome;
    int in[2]{-1, -1}, out[2]{-1, -1}, err[2]{-1, -1}, report[2]{-1, -1};
    auto start_failed = [&](const char* step, int code) { outcome.problem = std::string("PROCESS_START: ") + step + " failed (" + std::strerror(code) + ")"; return outcome; };
    if (pipe2(in, O_CLOEXEC) || pipe2(out, O_CLOEXEC) || pipe2(err, O_CLOEXEC) || pipe2(report, O_CLOEXEC)) return start_failed("pipe", errno);
    std::vector<char*> args;
    for (const auto& a : request.argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    std::map<std::string, std::string> merged;
    for (auto** p = environ; p && *p; ++p) { std::string entry(*p); const auto at = entry.find('='); if (at != std::string::npos) merged[entry.substr(0, at)] = entry.substr(at + 1); }
    for (const auto& [k, v] : request.env) merged[k] = v;
    std::vector<std::string> entries;
    for (const auto& [k, v] : merged) entries.push_back(k + "=" + v);
    std::vector<char*> envp;
    for (auto& e : entries) envp.push_back(e.data());
    envp.push_back(nullptr);
    const pid_t child = fork();
    if (child < 0) return start_failed("fork", errno);
    if (child == 0) {
        setpgid(0, 0);
        dup2(in[0], 0);
        dup2(out[1], 1);
        dup2(err[1], 2);
        if (request.cwd.empty() || chdir(request.cwd.c_str()) == 0) {
            environ = envp.data();
            execvp(args[0], args.data());
        }
        const int code = errno;
        (void)!write(report[1], &code, sizeof code);
        _exit(127);
    }
    setpgid(child, child);
    close_fd(in[0]);
    close_fd(out[1]);
    close_fd(err[1]);
    close_fd(report[1]);
    int exec_error = 0;
    const bool exec_failed = read(report[0], &exec_error, sizeof exec_error) == static_cast<ssize_t>(sizeof exec_error);
    close_fd(report[0]);
    if (exec_failed) {
        int status;
        waitpid(child, &status, 0);
        close_fd(in[1]);
        close_fd(out[0]);
        close_fd(err[0]);
        return start_failed("exec", exec_error);
    }
    signal(SIGPIPE, SIG_IGN);
    fcntl(in[1], F_SETFL, O_NONBLOCK);
    std::size_t sent = 0;
    if (request.input->empty()) close_fd(in[1]);
    const auto deadline = Clock::now() + std::chrono::milliseconds(request.timeout_ms);
    bool killed = false, overflow = false;
    auto kill_tree = [&](const char* why) { if (why) outcome.problem = why; kill(-child, SIGKILL); killed = true; };
    while (out[0] >= 0 || err[0] >= 0) {
        if (!killed) {
            if (request.cancel && request.cancel->stopped()) { outcome.canceled = true; kill_tree(nullptr); }
            else if (overflow) kill_tree("PROCESS_OUTPUT_LIMIT: output exceeds 128 MiB");
            else if (request.timeout_ms > 0 && Clock::now() >= deadline) kill_tree("PROCESS_TIMEOUT: time limit exceeded");
        }
        pollfd fds[3]{{out[0], POLLIN, 0}, {err[0], POLLIN, 0}, {in[1], POLLOUT, 0}};
        if (poll(fds, 3, 10) < 0 && errno != EINTR) break;
        char buffer[65536];
        auto drain = [&](pollfd& p, int& fd, std::string& into) {
            if (fd < 0 || !(p.revents & (POLLIN | POLLHUP | POLLERR))) return;
            const auto got = read(fd, buffer, sizeof buffer);
            if (got > 0) { if (into.size() + static_cast<std::size_t>(got) > max_output) overflow = true; else into.append(buffer, static_cast<std::size_t>(got)); }
            else if (got == 0 || errno != EINTR) close_fd(fd);
        };
        drain(fds[0], out[0], outcome.out);
        drain(fds[1], err[0], outcome.err);
        if (in[1] >= 0 && (fds[2].revents & (POLLOUT | POLLERR | POLLHUP))) {
            const auto wrote = write(in[1], request.input->data() + sent, request.input->size() - sent);
            if (wrote > 0) sent += static_cast<std::size_t>(wrote);
            if (sent == request.input->size() || (wrote < 0 && errno != EAGAIN && errno != EINTR)) close_fd(in[1]);
        }
        // The child is gone: end any descendant still holding the pipes open.
        int status;
        if (!killed && waitpid(child, &status, WNOHANG) == child) {
            outcome.code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
            kill(-child, SIGKILL);
            killed = true;
            close_fd(in[1]);
            while (out[0] >= 0 || err[0] >= 0) {
                pollfd rest[2]{{out[0], POLLIN, 0}, {err[0], POLLIN, 0}};
                if (poll(rest, 2, 100) <= 0) break;
                drain(rest[0], out[0], outcome.out);
                drain(rest[1], err[0], outcome.err);
            }
            close_fd(out[0]);
            close_fd(err[0]);
            return outcome;
        }
    }
    close_fd(in[1]);
    close_fd(out[0]);
    close_fd(err[0]);
    int status = 0;
    if (!killed) waitpid(child, &status, 0);
    else { kill(-child, SIGKILL); waitpid(child, &status, 0); }
    if (outcome.problem.empty() && !outcome.canceled) outcome.code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    return outcome;
}
#endif
Value bytes_value(std::string text) { return Value(BytesValue{managed<const std::string>(std::move(text))}); }
}
Value invoke_process_standard(const StandardFunction& f, const std::vector<Value>& args, const SourceSpan& s, RuntimeContext* context) {
    Request request;
    const auto& argv = std::get<SliceValue>(args[0].data);
    if (argv.length == 0) runtime_error(s, "process argv must not be empty", "E4003");
    auto started = [](std::string problem) { return make_multi({Value(I{-1}), bytes_value({}), bytes_value({}), Value(std::move(problem))}); };
    for (std::size_t i = 0; i < argv.length; ++i) {
        request.argv.push_back(string_value(sequence_read(argv, i).data));
        if (has_nul(request.argv.back())) return started("PROCESS_START: argument contains NUL");
    }
    request.input = std::get<BytesValue>(args[1].data).data.get();
    request.timeout_ms = as_int(args[2], s);
    if (request.timeout_ms < 0) runtime_error(s, "process timeout must not be negative", "E4003");
    if (f.name == "run_with") {
        request.cwd = string_value(args[3].data);
        if (has_nul(request.cwd)) return started("PROCESS_START: cwd contains NUL");
        for (const auto& [k, v] : std::get<MapValue>(args[4].data).data->entries) {
            const auto& name = std::get<std::string>(k);
            const auto& value = string_value(v.data);
            if (name.empty() || name.find('=') != std::string::npos || has_nul(name) || has_nul(value)) return started("PROCESS_START: invalid environment entry");
            request.env.emplace(name, value);
        }
    }
    if (context) request.cancel = context->cancellation.get();
    auto outcome = run_child(request);
    if (outcome.canceled) runtime_error(s, "task canceled", "E4101");
    return make_multi({Value(outcome.code), bytes_value(std::move(outcome.out)), bytes_value(std::move(outcome.err)), Value(std::move(outcome.problem))});
}
}
