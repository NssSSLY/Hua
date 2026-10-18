#pragma once
#include <cstddef>
#include <cstdint>
namespace hua::native_allocation {
struct Snapshot {std::uint64_t calls,requested,live,peak;};
Snapshot snapshot() noexcept;
// Test-only one-shot fault, disabled after the selected allocation throws.
void fail_after(std::size_t successful_allocations) noexcept;
void clear_failure() noexcept;
}
