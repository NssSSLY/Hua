// Full-budget VM call observer for C04 revisit and helper step attribution.
#include "bootstrap_capability_profile.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

struct Observer final : hua::VMCallObserver {
    hua::bootstrap_phase::Session& session;
    explicit Observer(hua::bootstrap_phase::Session& s) : session(s) {}
    void enter(const hua::Node* fn, const hua::Node* caller, const std::vector<hua::Value>& args) override {
        session.enter(fn, caller, args);
    }
    void leave(const hua::Node* fn, const hua::Value* result) override {
        session.leave(fn, result);
    }
};

static std::string to_utf8(const std::filesystem::path& p) {
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

int execute(const std::vector<std::filesystem::path>& paths) {
    if (paths.size() != 5) {
        std::cerr << "usage: c04_vm_probe <events.jsonl> <report.json> <compiler.huab> <input.hua> <output.huab>\n";
        return 2;
    }
    const auto& events_path = paths[0];
    const auto& report_path = paths[1];
    const auto& compiler_path = paths[2];
    const auto& input_path = paths[3];
    const auto& output_path = paths[4];
    auto image = hua::read_huab(compiler_path);
    hua::RuntimeContext context;
    context.input = &std::cin;
    context.working_directory = std::filesystem::current_path();
    context.arguments = {to_utf8(input_path), to_utf8(output_path)};
    // Compiler execution profile limits: 100B steps, 128 calls, 65536 GC interval.
    context.execution_limits = {100000000000ULL, 128, 65536};
    context.execution_metrics = std::make_shared<hua::ExecutionMetrics>();
    std::ofstream events_file(events_path);
    if (!events_file) throw std::runtime_error("cannot open events file");
    hua::bootstrap_phase::Session session(image.model, *context.execution_metrics, events_file);
    Observer observer(session);
    auto rc = hua::report_entry(
        hua::VirtualMachine(image.model, std::cout, &context, &observer).run(image.bytecode),
        std::cerr);
    if (rc != 0) throw std::runtime_error("compiler failed with code " + std::to_string(rc));
    session.finish();
    std::ofstream report_file(report_path);
    if (!report_file) throw std::runtime_error("cannot open report file");
    session.report(report_file);
    return 0;
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    try {
        std::vector<std::filesystem::path> paths;
        for (int i = 1; i < argc; ++i) paths.emplace_back(argv[i]);
        return execute(paths);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 2;
    }
}
#else
int main(int argc, char** argv) {
    try {
        std::vector<std::filesystem::path> paths;
        for (int i = 1; i < argc; ++i) paths.emplace_back(argv[i]);
        return execute(paths);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 2;
    }
}
#endif
