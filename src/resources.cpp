#include "hua/performance.hpp"
#include "hua/resources.hpp"
#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>
#include <new>
#include <thread>
#include <unordered_set>
#include <utility>

namespace hua::resources {
namespace {
constexpr auto metrics = static_cast<std::size_t>(Metric::count);
std::size_t index(Metric m) { return static_cast<std::size_t>(m); }
Failure fail(Code c, const char* text) { return {c, text}; }
template<class T> Outcome<T> rejected(Code c, const char* text) { return {std::nullopt, fail(c, text)}; }
bool fits(std::size_t offset, std::size_t length, std::size_t size) {
    return offset <= size && length <= size - offset;
}
}
Cost cost(Metric m, std::uint64_t n) { Cost c{}; c[index(m)] = n; return c; }
struct QuotaLedger::Impl {
    std::shared_ptr<std::mutex> mutex;
    std::shared_ptr<Impl> parent;
    LedgerSnapshot counters;
};
QuotaLedger::QuotaLedger(std::shared_ptr<Impl> p) : impl_(std::move(p)) {}
std::shared_ptr<QuotaLedger> QuotaLedger::root(Cost limits) {
    auto p = std::make_shared<Impl>(); p->mutex = std::make_shared<std::mutex>(); p->counters.limit = limits;
    return std::shared_ptr<QuotaLedger>(new QuotaLedger(std::move(p)));
}
std::shared_ptr<QuotaLedger> QuotaLedger::child(Cost limits) {
    auto p = std::make_shared<Impl>(); p->mutex = impl_->mutex; p->parent = impl_; p->counters.limit = limits;
    return std::shared_ptr<QuotaLedger>(new QuotaLedger(std::move(p)));
}
Outcome<Reservation> QuotaLedger::reserve(Cost amount) {
    auto self = shared_from_this();
    std::lock_guard lock(*impl_->mutex);
    for (auto p = impl_; p; p = p->parent) {
        for (std::size_t i = 0; i < metrics; ++i) {
            if (amount[i] > std::numeric_limits<std::uint64_t>::max() - p->counters.reserved[i])
                return rejected<Reservation>(Code::overflow, "quota accounting overflow");
            if (amount[i] > p->counters.limit[i] - p->counters.used[i])
                return rejected<Reservation>(Code::quota, "quota exhausted");
        }
    }
    for (auto p = impl_; p; p = p->parent)
        for (std::size_t i = 0; i < metrics; ++i) {
            p->counters.used[i] += amount[i]; p->counters.reserved[i] += amount[i];
        }
    return {Reservation(std::move(self), amount), {}};
}
void QuotaLedger::release(const Cost& amount) {
    std::lock_guard lock(*impl_->mutex);
    for (auto p = impl_; p; p = p->parent)
        for (std::size_t i = 0; i < metrics; ++i) {
            p->counters.used[i] -= amount[i]; p->counters.returned[i] += amount[i];
        }
}
LedgerSnapshot QuotaLedger::snapshot() const { std::lock_guard lock(*impl_->mutex); return impl_->counters; }
Reservation::Reservation(std::shared_ptr<QuotaLedger> l, Cost c) : ledger_(std::move(l)), amount_(c) {}
Reservation::Reservation(Reservation&& other) noexcept
    : ledger_(std::move(other.ledger_)), amount_(std::exchange(other.amount_, {})) {}
Reservation& Reservation::operator=(Reservation&& other) noexcept {
    if (this != &other) { release(); ledger_ = std::move(other.ledger_); amount_ = std::exchange(other.amount_, {}); }
    return *this;
}
Reservation::~Reservation() { release(); }
void Reservation::release(Metric m) {
    if (!ledger_) return;
    Cost c{}; c[index(m)] = std::exchange(amount_[index(m)], 0); ledger_->release(c);
}
void Reservation::release() {
    if (!ledger_) return;
    ledger_->release(std::exchange(amount_, {})); ledger_.reset();
}
ImmutableBytes::ImmutableBytes(Reservation r, std::span<const std::uint8_t> data)
    : allocation_(std::move(r)), data_(data.begin(), data.end()) {}
Outcome<std::shared_ptr<const ImmutableBytes>> ImmutableBytes::copy(
    std::shared_ptr<QuotaLedger> ledger, std::span<const std::uint8_t> data) {
    auto permit = ledger->reserve(cost(Metric::storage_bytes, data.size()));
    if (!permit) return {std::nullopt, permit.error};
    try {
        return {std::shared_ptr<const ImmutableBytes>(new ImmutableBytes(std::move(*permit.value), data)), {}};
    } catch (const std::bad_alloc&) { return rejected<std::shared_ptr<const ImmutableBytes>>(Code::allocation, "byte allocation failed"); }
}
struct FixedStorage::Impl {
    mutable std::mutex mutex;
    Reservation allocation;
    std::vector<std::uint8_t> data;
    std::size_t readers{};
    bool writer{};
    Impl(Reservation r, std::size_t size) : allocation(std::move(r)), data(size) {}
};
FixedStorage::FixedStorage(Reservation r, std::size_t size) : impl_(std::make_unique<Impl>(std::move(r), size)) {}
FixedStorage::~FixedStorage() = default;
Outcome<std::shared_ptr<FixedStorage>> FixedStorage::allocate(
    std::shared_ptr<QuotaLedger> ledger, std::size_t size, bool fault) {
    auto permit = ledger->reserve(cost(Metric::storage_bytes, size));
    if (!permit) return {std::nullopt, permit.error};
    if (fault) return rejected<std::shared_ptr<FixedStorage>>(Code::allocation, "injected storage allocation failure");
    try { return {std::shared_ptr<FixedStorage>(new FixedStorage(std::move(*permit.value), size)), {}}; }
    catch (const std::bad_alloc&) { return rejected<std::shared_ptr<FixedStorage>>(Code::allocation, "storage allocation failed"); }
}
Outcome<Lease> FixedStorage::lease(Access access, std::size_t offset, std::size_t length) {
    std::lock_guard lock(impl_->mutex);
    if (!fits(offset, length, impl_->data.size())) return rejected<Lease>(Code::bounds, "lease outside storage");
    if (impl_->writer || (access == Access::write && impl_->readers))
        return rejected<Lease>(Code::lease_conflict, "conflicting storage lease");
    if (access == Access::read) ++impl_->readers; else impl_->writer = true;
    return {Lease(shared_from_this(), access, offset, length), {}};
}
Outcome<std::vector<std::uint8_t>> FixedStorage::copy(std::size_t offset, std::size_t length) const {
    std::lock_guard lock(impl_->mutex);
    if (!fits(offset, length, impl_->data.size())) return rejected<std::vector<std::uint8_t>>(Code::bounds, "copy outside storage");
    if (impl_->writer) return rejected<std::vector<std::uint8_t>>(Code::lease_conflict, "backend has exclusive write lease");
    return {std::vector<std::uint8_t>(impl_->data.begin() + offset, impl_->data.begin() + offset + length), {}};
}
Failure FixedStorage::write(std::size_t offset, std::span<const std::uint8_t> data) {
    std::lock_guard lock(impl_->mutex);
    if (!fits(offset, data.size(), impl_->data.size())) return fail(Code::bounds, "write outside storage");
    if (impl_->writer || impl_->readers) return fail(Code::lease_conflict, "storage is leased");
    std::copy(data.begin(), data.end(), impl_->data.begin() + offset); return {};
}
std::size_t FixedStorage::size() const { return impl_->data.size(); }
std::size_t FixedStorage::leases() const { std::lock_guard lock(impl_->mutex); return impl_->readers + impl_->writer; }
Lease::Lease(std::shared_ptr<FixedStorage> s, Access a, std::size_t o, std::size_t n)
    : storage_(std::move(s)), access_(a), offset_(o), length_(n) {}
Lease::Lease(Lease&& other) noexcept
    : storage_(std::move(other.storage_)), access_(other.access_), offset_(other.offset_), length_(other.length_) {}
Lease& Lease::operator=(Lease&& other) noexcept {
    if (this != &other) {
        release(); storage_ = std::move(other.storage_); access_ = other.access_; offset_ = other.offset_; length_ = other.length_;
    }
    return *this;
}
Lease::~Lease() { release(); }
void Lease::release() {
    if (!storage_) return;
    auto held = std::move(storage_);
    std::lock_guard lock(held->impl_->mutex);
    if (access_ == Access::read) --held->impl_->readers; else held->impl_->writer = false;
}
Outcome<std::vector<std::uint8_t>> Lease::read() const {
    if (!storage_) return rejected<std::vector<std::uint8_t>>(Code::closed, "lease retired");
    if (access_ != Access::read) return rejected<std::vector<std::uint8_t>>(Code::lease_conflict, "write lease cannot read");
    std::lock_guard lock(storage_->impl_->mutex);
    const auto& data = storage_->impl_->data;
    return {std::vector<std::uint8_t>(data.begin() + offset_, data.begin() + offset_ + length_), {}};
}
Failure Lease::write(std::span<const std::uint8_t> data) {
    if (!storage_) return fail(Code::closed, "lease retired");
    if (access_ != Access::write) return fail(Code::lease_conflict, "read lease cannot write");
    if (data.size() > length_) return fail(Code::bounds, "write exceeds lease");
    std::lock_guard lock(storage_->impl_->mutex);
    std::copy(data.begin(), data.end(), storage_->impl_->data.begin() + offset_); return {};
}
struct CloseRecord { mutable std::mutex mutex; std::optional<CloseResult> result; };
struct Control {
    mutable std::mutex mutex;
    ResourceSnapshot state;
    std::shared_ptr<const int> domain;
    Reservation handle;
    std::function<CloseResult()> physical_close;
    std::shared_ptr<CloseRecord> closing;
    std::vector<std::weak_ptr<Operation>> operations;
};
struct Operation {
    std::shared_ptr<void> external_storage;
    mutable std::mutex mutex;
    std::shared_ptr<Control> control;
    Reservation permit;
    std::optional<Lease> lease;
    Phase phase{Phase::reserved};
    std::optional<Completion> result;
};
Handle::Handle(std::shared_ptr<Control> c, std::uint64_t e) : control_(std::move(c)), epoch_(e) {}
ResourceSnapshot Handle::snapshot() const {
    if (!control_) return {};
    std::lock_guard lock(control_->mutex); auto s = control_->state; s.quiescent = s.active == 0; return s;
}
Event::Event(std::shared_ptr<Operation> p) : operation_(std::move(p)) {}
std::optional<Completion> Event::observe() const {
    if (!operation_) return std::nullopt;
    std::lock_guard lock(operation_->mutex); return operation_->result;
}
Phase Event::phase() const {
    if (!operation_) return Phase::retired;
    std::lock_guard lock(operation_->mutex); return operation_->phase;
}
CloseTicket::CloseTicket(std::shared_ptr<CloseRecord> r) : record_(std::move(r)) {}
std::optional<CloseResult> CloseTicket::observe() const {
    if (!record_) return std::nullopt;
    std::lock_guard lock(record_->mutex); return record_->result;
}
namespace {
Failure authorized(const Control& c, std::uint64_t epoch, TaskId actor) {
    if (epoch != c.state.owner_epoch) return fail(Code::stale_owner, "owner epoch expired");
    if (actor != c.state.owner) return fail(Code::wrong_owner, "resource belongs to another task");
    return {};
}
Failure open(const Control& c) {
    if (c.state.state == Lifecycle::poisoned) return fail(Code::poisoned, "resource release or submission is uncertain");
    if (c.state.state != Lifecycle::open) return fail(Code::closed, "resource admission closed");
    return {};
}
void start_close(Control& c) {
    if (c.state.state != Lifecycle::open) return;
    // Allocate before changing state so an allocation failure cannot leave a missing close record.
    auto record = std::make_shared<CloseRecord>();
    c.closing = std::move(record); c.state.state = Lifecycle::closing;
}
}
struct HandleTable::Impl {
    struct Slot { std::weak_ptr<Control> resource; std::uint64_t generation{1}, epoch{}; bool occupied{}; };
    std::mutex mutex; std::vector<Slot> slots;
};
HandleTable::HandleTable() : impl_(std::make_unique<Impl>()) {}
HandleTable::~HandleTable() = default;
BoundaryHandle HandleTable::publish(const Handle& h) {
    std::lock_guard lock(impl_->mutex);
    for (std::size_t i = 0; i < impl_->slots.size(); ++i) {
        auto& slot = impl_->slots[i];
        if (!slot.occupied && slot.generation != std::numeric_limits<std::uint64_t>::max()) {
            slot.resource = h.control_; slot.epoch = h.epoch_; slot.occupied = true; return {i, slot.generation};
        }
    }
    impl_->slots.push_back({h.control_, 1, h.epoch_, true}); return {impl_->slots.size() - 1, 1};
}
Outcome<Handle> HandleTable::resolve(BoundaryHandle h) {
    std::lock_guard lock(impl_->mutex);
    if (h.slot >= impl_->slots.size()) return rejected<Handle>(Code::stale_handle, "unknown boundary handle");
    const auto& slot = impl_->slots[h.slot];
    if (!slot.occupied || slot.generation != h.generation) return rejected<Handle>(Code::stale_handle, "stale boundary generation");
    auto c = slot.resource.lock();
    if (!c) return rejected<Handle>(Code::stale_handle, "resource no longer retained");
    std::lock_guard control_lock(c->mutex);
    // A boundary slot must preserve the capability epoch, rather than grant a new owner's authority.
    return {Handle(std::move(c), slot.epoch), {}};
}
Failure HandleTable::retire(BoundaryHandle h) {
    std::lock_guard lock(impl_->mutex);
    if (h.slot >= impl_->slots.size()) return fail(Code::stale_handle, "unknown boundary handle");
    auto& slot = impl_->slots[h.slot];
    if (!slot.occupied || slot.generation != h.generation) return fail(Code::stale_handle, "stale boundary generation");
    auto c = slot.resource.lock();
    if (!c) return fail(Code::stale_handle, "missing resource close proof");
    std::lock_guard control_lock(c->mutex);
    if (c->state.state != Lifecycle::closed) return fail(Code::busy, "retire requires known physical release");
    slot.resource.reset(); slot.occupied = false;
    if (slot.generation != std::numeric_limits<std::uint64_t>::max()) ++slot.generation;
    return {};
}
struct ResourceDomain::Impl {
    mutable std::mutex mutex;
    std::shared_ptr<QuotaLedger> ledger;
    std::shared_ptr<const int> domain{std::make_shared<const int>(0)};
    std::thread::id executor{std::this_thread::get_id()};
    bool affine{}, stopped{}, cpu_stop_proof{};
    // One-way publication: no resource or close work can exist before this is set.
    std::atomic<bool> has_created_resource{false};
    std::unordered_set<TaskId> exited;
    std::vector<std::shared_ptr<Control>> resources;
    std::vector<std::shared_ptr<Operation>> operations;
};
ResourceDomain::ResourceDomain(std::shared_ptr<QuotaLedger> ledger, bool affine, bool cpu_stop_proof) : impl_(std::make_unique<Impl>()) {
    impl_->ledger = std::move(ledger); impl_->affine = affine; impl_->cpu_stop_proof = cpu_stop_proof;
}
namespace {
// Unknown physical release must not be silently refunded when a runtime is destroyed.
// This process-lifetime quarantine is exceptional; a real backend needs its own recovery executor.
struct Quarantine { std::mutex mutex; std::vector<std::shared_ptr<Control>> resources; std::vector<std::shared_ptr<Operation>> operations; };
Quarantine& quarantine() { static auto* q = new Quarantine; return *q; }
std::atomic<std::uint64_t> next_resource{1};
}
ResourceDomain::~ResourceDomain() {
    shutdown();
    std::vector<std::shared_ptr<Operation>> operations;
    { std::lock_guard lock(impl_->mutex); operations = impl_->operations; }
    // This backend has no asynchronous native worker. Stopping the CPU model proves no future
    // backend access, so its remaining operations can retire. This is not a native IO cancellation contract.
    if (impl_->cpu_stop_proof)
        for (auto& p : operations) settle(Event(p), {ResultKind::canceled, {}, std::nullopt, {}});
    pump();
    auto& q = quarantine();
    std::lock_guard lock(q.mutex);
    for (const auto& p : operations) if (Event(p).phase() != Phase::retired) q.operations.push_back(p);
    for (const auto& c : impl_->resources) {
        std::lock_guard control_lock(c->mutex);
        if (c->state.state != Lifecycle::closed) q.resources.push_back(c);
    }
}
Outcome<Handle> ResourceDomain::create(TaskId owner, std::function<CloseResult()> close, bool fault) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopped || impl_->exited.contains(owner)) return rejected<Handle>(Code::closed, "task or runtime admission closed");
    auto permit = impl_->ledger->reserve(cost(Metric::handles));
    if (!permit) return {std::nullopt, permit.error};
    if (fault) return rejected<Handle>(Code::allocation, "injected creation failure");
    try {
        auto c = std::make_shared<Control>();
        // IDs never wrap or re-enter circulation; ordinary accesses still use strong references.
        auto id = next_resource.load();
        do {
            if (id == std::numeric_limits<std::uint64_t>::max()) return rejected<Handle>(Code::overflow, "resource ID exhausted");
        } while (!next_resource.compare_exchange_weak(id, id + 1));
        c->state.id = id; c->state.generation = 1; c->state.owner_epoch = 1; c->state.owner = owner;
        c->domain = impl_->domain; c->handle = std::move(*permit.value); c->physical_close = std::move(close);
        impl_->resources.push_back(c);
        impl_->has_created_resource.store(true,std::memory_order_release);
        return {Handle(std::move(c), 1), {}};
    } catch (const std::bad_alloc&) { return rejected<Handle>(Code::allocation, "resource allocation failed"); }
}
Outcome<Event> ResourceDomain::reserve(const Handle& h, TaskId actor, std::shared_ptr<FixedStorage> storage,
                                    Access access, std::size_t offset, std::size_t length, std::shared_ptr<void> external_storage) {
    if (!h.control_ || h.control_->domain != impl_->domain) return rejected<Event>(Code::wrong_executor, "foreign resource");
    std::lock_guard runtime_lock(impl_->mutex);
    if (impl_->stopped || impl_->exited.contains(actor)) return rejected<Event>(Code::closed, "task or runtime admission closed");
    auto c = h.control_;
    std::lock_guard lock(c->mutex);
    if (auto e = authorized(*c, h.epoch_, actor)) return {std::nullopt, e};
    if (auto e = open(*c)) return {std::nullopt, e};
    if (!storage && (offset || (length && !external_storage))) return rejected<Event>(Code::bounds, "missing storage");
    Cost amount{}; amount[index(Metric::operations)] = 1; amount[index(Metric::queue_items)] = 1;
    amount[index(Metric::inflight_bytes)] = length; amount[index(Metric::queue_bytes)] = length;
    auto permit = impl_->ledger->reserve(amount);
    if (!permit) return {std::nullopt, permit.error};
    std::optional<Lease> lease;
    if (storage) {
        auto granted = storage->lease(access, offset, length);
        if (!granted) return {std::nullopt, granted.error};
        lease = std::move(*granted.value);
    }
    try {
        auto p = std::make_shared<Operation>(); p->control = c; p->permit = std::move(*permit.value); p->lease = std::move(lease); p->external_storage = std::move(external_storage);
        // Reserve list capacity before publishing either reference or counting admission.
        impl_->operations.reserve(impl_->operations.size() + 1); c->operations.reserve(c->operations.size() + 1);
        impl_->operations.push_back(p); c->operations.push_back(p); ++c->state.active;
        return {Event(std::move(p)), {}};
    } catch (const std::bad_alloc&) { return rejected<Event>(Code::allocation, "operation allocation failed"); }
}
namespace {
void retire_operation(Operation& p, Completion result) {
    // Caller owns the operation lock. No backend entry can pass this point concurrently.
    p.result = std::move(result); p.phase = Phase::retired;
    p.lease.reset(); p.external_storage.reset(); p.permit.release();
    std::lock_guard control_lock(p.control->mutex);
    --p.control->state.active;
    std::erase_if(p.control->operations, [&](const auto& weak) { auto v = weak.lock(); return !v || v.get() == &p; });
}
}
Failure ResourceDomain::submit(const Event& e, bool rejected_submit, bool unknown) {
    auto p = e.operation_;
    if (!p || p->control->domain != impl_->domain) return fail(Code::wrong_executor, "foreign operation");
    if (impl_->affine && std::this_thread::get_id() != impl_->executor) return fail(Code::wrong_executor, "wrong submit executor");
    Failure failure;
    {
        std::lock_guard lock(p->mutex);
        if (p->phase != Phase::reserved) return fail(Code::busy, "operation already submitted or retired");
        {
            std::lock_guard control_lock(p->control->mutex);
            // Admission and Closing share this exact linearization lock.
            failure = open(*p->control);
            if (!failure && !rejected_submit) {
                if (unknown) start_close(*p->control);
                p->phase = unknown ? Phase::unknown : Phase::submitted;
            }
        }
        p->permit.release(Metric::queue_items); p->permit.release(Metric::queue_bytes);
        if (!failure && !rejected_submit)
            return unknown ? fail(Code::backend_failed, "submission acceptance unknown; resource isolated") : Failure{};
        if (!failure) failure = fail(Code::submit_failed, "backend rejected before acceptance");
        retire_operation(*p, {ResultKind::failed, failure, 0, {}});
    }
    std::lock_guard runtime_lock(impl_->mutex); std::erase(impl_->operations, p);
    return failure;
}
bool ResourceDomain::cancel(const Event& e) {
    auto p = e.operation_;
    if (!p || p->control->domain != impl_->domain) return false;
    {
        std::lock_guard lock(p->mutex);
        if (p->phase == Phase::retired) return false;
        if (p->phase != Phase::reserved) { p->phase = Phase::cancel_requested; return true; }
        // A never-submitted reservation is retired atomically; it cannot subsequently enter the backend.
        retire_operation(*p, {ResultKind::canceled, {}, 0, {}});
    }
    std::lock_guard runtime_lock(impl_->mutex); std::erase(impl_->operations, p);
    return true;
}
bool ResourceDomain::settle(const Event& e, Completion result) {
    auto p = e.operation_;
    if (!p || p->control->domain != impl_->domain) return false;
    {
        std::lock_guard lock(p->mutex);
        if (p->phase == Phase::retired || p->phase == Phase::reserved) return false;
        retire_operation(*p, std::move(result));
    }
    std::lock_guard runtime_lock(impl_->mutex);
    std::erase(impl_->operations, p); return true;
}
Outcome<std::vector<std::uint8_t>> ResourceDomain::backend_read(const Event& e) const {
    auto p = e.operation_;
    if (!p || p->control->domain != impl_->domain) return rejected<std::vector<std::uint8_t>>(Code::wrong_executor, "foreign operation");
    if (impl_->affine && std::this_thread::get_id() != impl_->executor) return rejected<std::vector<std::uint8_t>>(Code::wrong_executor, "wrong CPU executor");
    std::lock_guard lock(p->mutex);
    if (p->phase == Phase::reserved || p->phase == Phase::retired || !p->lease)
        return rejected<std::vector<std::uint8_t>>(Code::closed, "backend operation not active");
    return p->lease->read();
}
Failure ResourceDomain::backend_write(const Event& e, std::span<const std::uint8_t> data) {
    auto p = e.operation_;
    if (!p || p->control->domain != impl_->domain) return fail(Code::wrong_executor, "foreign operation");
    if (impl_->affine && std::this_thread::get_id() != impl_->executor) return fail(Code::wrong_executor, "wrong CPU executor");
    std::lock_guard lock(p->mutex);
    if (p->phase == Phase::reserved || p->phase == Phase::retired || !p->lease)
        return fail(Code::closed, "backend operation not active");
    return p->lease->write(data);
}
Outcome<CloseTicket> ResourceDomain::close(const Handle& h, TaskId actor) {
    auto c = h.control_;
    if (!c || c->domain != impl_->domain) return rejected<CloseTicket>(Code::wrong_executor, "foreign resource");
    std::vector<std::shared_ptr<Operation>> operations;
    std::shared_ptr<CloseRecord> record;
    {
        std::lock_guard lock(c->mutex);
        if (auto e = authorized(*c, h.epoch_, actor)) return {std::nullopt, e};
        if (c->state.state == Lifecycle::poisoned) return rejected<CloseTicket>(Code::poisoned, "physical release is unknown");
        if (c->state.state == Lifecycle::closed) {
            record = std::make_shared<CloseRecord>(); record->result = CloseResult{};
            return {CloseTicket(std::move(record)), {}};
        }
        start_close(*c); record = c->closing;
        for (auto& weak : c->operations) if (auto p = weak.lock()) operations.push_back(std::move(p));
    }
    for (auto& p : operations) cancel(Event(p));
    return {CloseTicket(std::move(record)), {}};
}
const std::atomic<bool>& ResourceDomain::created_flag()const noexcept{return impl_->has_created_resource;}
Failure ResourceDomain::pump() {
    HUA_PROFILE_COUNT(resource_pumps);
    if (impl_->affine && std::this_thread::get_id() != impl_->executor) return fail(Code::wrong_executor, "wrong close executor");
    if (!impl_->has_created_resource.load(std::memory_order_acquire)) { HUA_PROFILE_COUNT(resource_fast_returns);return {}; }
    std::vector<std::shared_ptr<Control>> resources;
    { std::lock_guard lock(impl_->mutex); resources = impl_->resources; }
    HUA_PROFILE_ADD(resource_candidates,resources.size());
    for (const auto& c : resources) {
        {
            std::lock_guard lock(c->mutex);
            if (c->state.state != Lifecycle::closing || c->state.active || c->state.physical_started) continue;
            c->state.physical_started = true; ++c->state.physical_attempts;
        }
        CloseResult result;
        // Physical callback runs outside runtime, control, operation, storage and ledger locks.
        try { if (c->physical_close) result = c->physical_close(); }
        catch (...) { result = {fail(Code::backend_failed, "close callback threw; physical release unknown"), false}; }
        if (!result.released && !result.error) result.error = fail(Code::backend_failed, "physical release unknown");
        {
            std::lock_guard lock(c->mutex);
            c->state.state = result.released ? Lifecycle::closed : Lifecycle::poisoned;
            if (result.released) c->handle.release();
            std::lock_guard record_lock(c->closing->mutex); c->closing->result = std::move(result);
        }
    }
    // Closed controls remain reachable through ordinary Handle aliases, without accumulating in the domain.
    { std::lock_guard lock(impl_->mutex); std::erase_if(impl_->resources, [](const auto& c) {
        std::lock_guard control_lock(c->mutex); return c->state.state == Lifecycle::closed;
    }); }
    return {};
}
Outcome<Handle> ResourceDomain::transfer_quiescent(const Handle& h, TaskId from, TaskId to) {
    auto c = h.control_;
    if (!c || c->domain != impl_->domain) return rejected<Handle>(Code::wrong_executor, "foreign resource");
    std::lock_guard runtime_lock(impl_->mutex);
    if (impl_->stopped || impl_->exited.contains(from) || impl_->exited.contains(to))
        return rejected<Handle>(Code::closed, "source or destination admission closed");
    std::lock_guard lock(c->mutex);
    if (auto e = authorized(*c, h.epoch_, from)) return {std::nullopt, e};
    if (auto e = open(*c)) return {std::nullopt, e};
    if (c->state.active) return rejected<Handle>(Code::busy, "transfer requires quiescence");
    if (c->state.owner_epoch == std::numeric_limits<std::uint64_t>::max()) return rejected<Handle>(Code::overflow, "owner epoch exhausted");
    ++c->state.owner_epoch; c->state.owner = to;
    return {Handle(c, c->state.owner_epoch), {}};
}
Failure ResourceDomain::confirm_released(const Handle& h) {
    auto c = h.control_;
    if (!c || c->domain != impl_->domain) return fail(Code::wrong_executor, "foreign resource");
    std::lock_guard lock(c->mutex);
    if (c->state.state != Lifecycle::poisoned) return fail(Code::busy, "resource is not awaiting release confirmation");
    c->state.state = Lifecycle::closed; c->handle.release(); return {};
}
void ResourceDomain::owner_exit(TaskId owner) {
    std::vector<Handle> resources;
    {
        std::lock_guard lock(impl_->mutex); impl_->exited.insert(owner);
        for (const auto& c : impl_->resources) {
            std::lock_guard control_lock(c->mutex);
            if (c->state.owner == owner) resources.push_back(Handle(c, c->state.owner_epoch));
        }
    }
    for (const auto& h : resources) close(h, owner);
}
void ResourceDomain::shutdown() {
    std::vector<Handle> resources;
    {
        std::lock_guard lock(impl_->mutex); impl_->stopped = true;
        for (const auto& c : impl_->resources) {
            std::lock_guard control_lock(c->mutex); resources.push_back(Handle(c, c->state.owner_epoch));
        }
    }
    for (const auto& h : resources) close(h, h.snapshot().owner);
}
std::size_t ResourceDomain::pending() const { std::lock_guard lock(impl_->mutex); return impl_->operations.size(); }
std::size_t ResourceDomain::quarantined() const {
    std::vector<std::shared_ptr<Control>> resources;
    { std::lock_guard lock(impl_->mutex); resources = impl_->resources; }
    std::size_t count{};
    for (const auto& c : resources) { std::lock_guard lock(c->mutex); count += c->state.state == Lifecycle::poisoned; }
    return count;
}
}
