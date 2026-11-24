// Generic VM helper step profiler (zero struct layout assumptions; works on any selfhost HUAB).
#include "hua/archive.hpp"
#include "hua/vm.hpp"
#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <unordered_map>
#include <vector>

struct Work {
    std::uint64_t calls{}, inclusive{}, exclusive{};
};

struct Frame {
    const hua::Node* fn{};
    std::string key;
    std::uint64_t begin{}, children{};
};

struct Observer final : hua::VMCallObserver {
    hua::ExecutionMetrics& metrics;
    std::unordered_map<const hua::Node*, std::string> names;
    std::map<std::string, Work> work;
    std::vector<Frame> stack;

    Observer(const hua::SemanticModel& model, hua::ExecutionMetrics& m) : metrics(m) {
        for (const auto& [_, fn] : model.functions) {
            auto file = std::filesystem::path(fn->span.file_id).filename().string();
            auto name = hua::declared_name(*fn);
            auto split = name.rfind('$');
            if (split != std::string::npos) name.erase(0, split + 1);
            names.emplace(fn, file + "#" + name);
        }
    }
    void enter(const hua::Node* fn, const hua::Node*, const std::vector<hua::Value>&) override {
        auto it = names.find(fn);
        std::string k = (it != names.end()) ? it->second : "<anon>";
        stack.push_back(Frame{fn, std::move(k), metrics.steps.load(), 0});
    }
    void leave(const hua::Node* fn, const hua::Value*) override {
        if (stack.empty()) return;
        auto f = std::move(stack.back());
        stack.pop_back();
        auto delta = metrics.steps.load() - f.begin;
        auto& w = work[f.key];
        ++w.calls;
        w.inclusive += delta;
        w.exclusive += (delta >= f.children ? delta - f.children : 0);
        if (!stack.empty()) stack.back().children += delta;
    }
};

static std::string to_utf8(const std::filesystem::path& p) {
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 4) {
            std::cerr << "usage: c04_generic_helper_probe <compiler.huab> <input.hua> <output.huab>\n";
            return 2;
        }
        auto image = hua::read_huab(argv[1]);
        hua::RuntimeContext ctx;
        ctx.input = &std::cin;
        ctx.working_directory = std::filesystem::current_path();
        ctx.arguments = {to_utf8(argv[2]), to_utf8(argv[3])};
        ctx.execution_limits = {100000000000ULL, 128, 65536};
        ctx.execution_metrics = std::make_shared<hua::ExecutionMetrics>();
        Observer obs(image.model, *ctx.execution_metrics);
        auto rc = hua::report_entry(hua::VirtualMachine(image.model, std::cout, &ctx, &obs).run(image.bytecode), std::cerr);
        if (rc != 0) return rc;
        auto total = ctx.execution_metrics->steps.load();
        std::vector<std::pair<std::string, Work>> rows(obs.work.begin(), obs.work.end());
        std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.second.exclusive > b.second.exclusive; });
        std::cerr << "TOTAL_STEPS=" << total << "\n";
        std::size_t limit = std::min<std::size_t>(35, rows.size());
        for (std::size_t i = 0; i < limit; ++i) {
            const auto& [k, w] = rows[i];
            std::cerr << std::left << std::setw(38) << k
                      << " calls=" << std::right << std::setw(10) << w.calls
                      << " excl=" << std::setw(13) << w.exclusive
                      << " (" << std::fixed << std::setprecision(2) << (100.0 * w.exclusive / total) << "%)"
                      << " incl=" << std::setw(13) << w.inclusive
                      << " (" << std::fixed << std::setprecision(2) << (100.0 * w.inclusive / total) << "%)\n";
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 2;
    }
}
