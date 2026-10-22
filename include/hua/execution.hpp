#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <ostream>
#include <string>
#include <vector>
namespace hua {
// Execution policy belongs to the invocation, never to a HUAB image.
// compiler profile: wall-clock supervision (600 s, enforced by the supervising tool) is the
// real limit; the step cap only guards against runaway loops. Default profile stays 1M.
inline constexpr std::uint64_t compiler_step_limit=100000000000ULL;
struct ExecutionLimits { std::uint64_t steps{1000000}; unsigned calls{128}; std::size_t collection_interval{256}; };
struct ExecutionMetrics {
    std::atomic<std::uint64_t> steps{0}; std::atomic<unsigned> peak_calls{0};
    void call(unsigned depth) { auto old=peak_calls.load();while(old<depth&&!peak_calls.compare_exchange_weak(old,depth)){} }
};
struct ExecutionOptions {
    ExecutionLimits limits; std::string profile{"default"}; bool report{}; std::size_t arguments{};
};
inline bool execution_options(const std::vector<std::string>& args,ExecutionOptions& out) {
    bool profile=false,report=false;out.arguments=args.size();
    for(std::size_t i=3;i<args.size();++i){
        if(args[i]=="--"){out.arguments=i+1;return true;}
        if(args[i]=="--execution-profile"&&!profile&&i+1<args.size()){
            profile=true;out.profile=args[++i];
            if(out.profile=="compiler"){out.limits.steps=compiler_step_limit;out.limits.collection_interval=65536;}
            else if(out.profile!="default")return false;
        }else if(args[i]=="--report-budget"&&!report){report=true;out.report=true;}
        else return false;
    }
    return true;
}
struct ExecutionReport {
    ExecutionOptions options;std::shared_ptr<ExecutionMetrics> metrics;std::ostream& output;
    ~ExecutionReport(){if(options.report)output<<"{\"schema\":\"hua.execution.budget.v1\",\"profile\":\""<<options.profile
        <<"\",\"per_executor_step_limit\":"<<options.limits.steps<<",\"call_depth_limit\":"<<options.limits.calls
        <<",\"collection_interval\":"<<options.limits.collection_interval<<",\"steps\":"<<metrics->steps.load()<<",\"peak_calls\":"<<metrics->peak_calls.load()<<"}\n";}
};
}
