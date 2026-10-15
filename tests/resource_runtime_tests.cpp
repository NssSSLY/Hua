#include "hua/resources.hpp"
#include <algorithm>
#include <atomic>
#include <barrier>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>

using namespace hua::resources;
namespace {
std::size_t checks{};
void check(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
template<class T> T take(Outcome<T> result) {
    check(bool(result), "unexpected protocol failure");
    return std::move(*result.value);
}
Cost limits() { Cost v{}; v.fill(1000000); return v; }
auto ledger() { return QuotaLedger::root(limits()); }
std::uint64_t used(const std::shared_ptr<QuotaLedger>& l, Metric m) {
    return l->snapshot().used[static_cast<std::size_t>(m)];
}
void conserved(const std::shared_ptr<QuotaLedger>& l, bool empty = true) {
    auto s = l->snapshot();
    for (std::size_t i = 0; i < s.used.size(); ++i) {
        check(s.reserved[i] == s.used[i] + s.returned[i], "quota conservation violated");
        if (empty) check(s.used[i] == 0, "quota not returned");
    }
}
void finish(CpuSimulator& sim, const Handle& h, TaskId owner = 1) {
    auto ticket = take(sim.close(h, owner));
    check(!sim.pump(), "close pump failed");
    check(ticket.observe().has_value(), "close not completed");
    check(ticket.observe()->released, "release unknown");
}
void empty_pump_then_first_resource() {
    auto l=ledger();CpuSimulator sim(l,true);check(!sim.pump(),"empty owner pump failed");
    Failure wrong;std::thread worker([&]{wrong=sim.pump();});worker.join();
    check(wrong.code==Code::wrong_executor,"empty fast path bypassed executor ownership");
    unsigned closed=0;auto handle=take(sim.create(1,[&]{++closed;return CloseResult{};}));
    auto ticket=take(sim.close(handle,1));check(!ticket.observe(),"close ran outside pump");
    check(!sim.pump() && closed==1 && ticket.observe() && ticket.observe()->released,"first close skipped after empty fast path");
    check(!sim.pump() && closed==1,"closed callback repeated");conserved(l);
}
void quota_transactions() {
    auto cap = limits(); cap[static_cast<std::size_t>(Metric::storage_bytes)] = 8;
    auto root = QuotaLedger::root(cap); auto child = root->child(limits()); auto grandchild = child->child(limits());
    Cost c = cost(Metric::handles); c[static_cast<std::size_t>(Metric::storage_bytes)] = 9;
    auto before = root->snapshot();
    auto failed = grandchild->reserve(c);
    check(!failed && failed.error.code == Code::quota, "ancestor limit ignored");
    check(root->snapshot().reserved == before.reserved, "failed transaction changed parent");
    conserved(root); conserved(child); conserved(grandchild);
    c[static_cast<std::size_t>(Metric::storage_bytes)] = 8;
    auto reservation = take(grandchild->reserve(c));
    check(used(root, Metric::handles) == 1 && used(child, Metric::storage_bytes) == 8, "hierarchy not charged");
    Reservation moved = std::move(reservation); reservation.release();
    moved.release(Metric::storage_bytes); moved.release(Metric::storage_bytes);
    check(used(root, Metric::handles) == 1 && used(root, Metric::storage_bytes) == 0, "partial refund wrong");
    moved.release(); moved.release();
    conserved(root); conserved(child); conserved(grandchild);
}
void admission_rollback() {
    auto cap = limits(); cap[static_cast<std::size_t>(Metric::operations)] = 1;
    cap[static_cast<std::size_t>(Metric::queue_bytes)] = 4;
    auto l = QuotaLedger::root(cap); CpuSimulator sim(l); auto h = take(sim.create(1));
    auto storage = take(FixedStorage::allocate(l, 4));
    auto first = take(sim.reserve(h, 1, storage, Access::read, 0, 4));
    check(!sim.reserve(h, 1, storage, Access::read, 0, 4), "operation quota bypassed");
    check(h.snapshot().active == 1 && storage->leases() == 1, "failed admission published work");
    sim.submit(first); sim.settle(first, {});
    {
        auto exclusive = take(storage->lease(Access::write, 0, 4));
        auto conflict = sim.reserve(h, 1, storage, Access::read, 0, 4);
        check(!conflict && conflict.error.code == Code::lease_conflict, "lease failure not reported");
        check(h.snapshot().active == 0 && sim.pending() == 0, "lease failure published operation");
        check(used(l, Metric::operations) == 0 && used(l, Metric::queue_bytes) == 0, "lease failure not rolled back");
    }
    finish(sim, h); storage.reset(); conserved(l);
}
void accounting_overflow() {
    Cost cap{}; cap.fill(std::numeric_limits<std::uint64_t>::max());
    auto l = QuotaLedger::root(cap);
    auto r = take(l->reserve(cost(Metric::handles, std::numeric_limits<std::uint64_t>::max())));
    r.release();
    auto refused = l->reserve(cost(Metric::handles));
    check(!refused && refused.error.code == Code::overflow, "cumulative quota overflow not rejected");
    conserved(l);
}
void creation_rollback() {
    auto l = ledger(); CpuSimulator sim(l);
    check(!sim.create(1, {}, true), "resource fault ignored");
    check(!FixedStorage::allocate(l, 16, true), "storage fault ignored");
    conserved(l);
    auto h = take(sim.create(1)); finish(sim, h); conserved(l);
}
void alias_leases() {
    auto l = ledger();
    auto s = take(FixedStorage::allocate(l, 4)); auto alias = s;
    const std::uint8_t input[]{1, 2, 3, 4};
    check(!s->write(0, input), "initial write failed");
    auto a = take(s->lease(Access::read, 0, 2)); auto b = take(alias->lease(Access::read, 2, 2));
    check(!alias->lease(Access::write, 3, 1), "write lease bypassed read alias");
    check(alias->write(3, std::span(input, 1)).code == Code::lease_conflict, "CPU write bypassed lease");
    check(take(a.read()) == std::vector<std::uint8_t>({1, 2}), "leased read wrong");
    check(a.write(input).code == Code::lease_conflict, "read lease mutated storage");
    a.release(); b.release();
    auto w = take(alias->lease(Access::write, 1, 2));
    check(!s->copy(0, 1) && !s->lease(Access::read, 0, 1), "exclusive lease bypassed through alias");
    check(w.write(input).code == Code::bounds, "oversized backend write allowed");
    check(!w.write(std::span(input, 2)), "leased write failed");
    w.release();
    check(take(s->copy(0, 4)) == std::vector<std::uint8_t>({1, 1, 2, 4}), "storage content wrong");
    check(!s->lease(Access::read, std::numeric_limits<std::size_t>::max(), 1), "range overflow allowed");
    s.reset(); alias.reset(); conserved(l);
}
void one_close_and_error_record() {
    auto l = ledger(); CpuSimulator sim(l); int calls{};
    auto h = take(sim.create(1, [&] { ++calls; return CloseResult{Failure{Code::backend_failed, "flush failed, handle released"}, true}; }));
    auto first = take(sim.close(h, 1)); auto second = take(sim.close(h, 1));
    check(!first.observe() && !second.observe(), "close pretended immediate physical release");
    check(!sim.pump(), "pump failed");
    check(calls == 1 && h.snapshot().physical_attempts == 1, "physical close repeated");
    check(first.observe()->error.code == Code::backend_failed && second.observe()->error.code == Code::backend_failed, "joined close lost error");
    auto later = take(sim.close(h, 1));
    check(!later.observe()->error && first.observe()->error.code == Code::backend_failed, "idempotence erased original error");
    check(!sim.pump() && calls == 1, "duplicate release"); conserved(l);
}
void close_pending_lifetime() {
    auto l = ledger(); CpuSimulator sim(l); int calls{};
    auto h = take(sim.create(1, [&] { ++calls; return CloseResult{}; }));
    auto storage = take(FixedStorage::allocate(l, 8)); std::weak_ptr<FixedStorage> weak = storage;
    auto e = take(sim.reserve(h, 1, storage, Access::read, 0, 8)); check(!sim.submit(e), "submit failed");
    auto close = take(sim.close(h, 1));
    check(h.snapshot().state == Lifecycle::closing && !h.snapshot().quiescent, "close skipped barrier");
    check(!sim.reserve(h, 1), "new admission after close");
    check(!sim.pump() && calls == 0 && !close.observe(), "inflight resource released");
    storage.reset(); h = {}; Event backend = e; e = {};
    check(!weak.expired() && used(l, Metric::storage_bytes) == 8, "dropped wrappers freed backend storage");
    check(sim.cancel(backend) && used(l, Metric::inflight_bytes) == 8, "cancel refunded in-flight bytes");
    check(sim.settle(backend, {ResultKind::success, {}, 5, {}}), "backend settlement rejected");
    check(weak.expired(), "storage retained after final lease");
    check(!sim.pump() && calls == 1 && close.observe()->released, "quiescent close failed");
    check(backend.observe()->kind == ResultKind::success && backend.observe()->transferred == 5, "cancel incorrectly won completed success");
    conserved(l);
}
void event_and_payload_retention() {
    auto l = ledger(); CpuSimulator sim(l); auto h = take(sim.create(1));
    auto e = take(sim.reserve(h, 1)); check(!sim.settle(e, {}), "completion before submission accepted");
    check(!sim.submit(e), "submit failed");
    std::uint8_t data[]{8, 9, 10}; auto immutable = take(ImmutableBytes::copy(l, data)); data[0] = 0;
    check(sim.settle(e, {ResultKind::failed, {Code::backend_failed, "partial read"}, 3, immutable}), "partial completion failed");
    immutable.reset();
    auto observation = e.observe(); Event alias = e;
    check(alias.observe()->bytes == observation->bytes && alias.observe()->transferred == 3, "event repeated observation changed result");
    check(observation->bytes->data()[0] == 8, "immutable result shared mutable input");
    check(!sim.settle(e, {}) && !sim.cancel(e), "terminal result overwritten");
    finish(sim, h);
    e = {}; alias = {};
    check(used(l, Metric::storage_bytes) == 3, "observed bytes quota released early");
    observation.reset(); conserved(l);
}
void unknown_progress() {
    auto l = ledger(); CpuSimulator sim(l); auto h = take(sim.create(1));
    auto e = take(sim.reserve(h, 1)); sim.submit(e);
    check(sim.settle(e, {ResultKind::failed, {Code::backend_failed, "amount unknown"}, std::nullopt, {}}), "unknown progress settlement failed");
    check(!e.observe()->transferred, "unknown progress converted to zero");
    finish(sim, h); conserved(l);
}
void submission_failures() {
    auto l = ledger(); CpuSimulator sim(l); auto h = take(sim.create(1));
    auto storage = take(FixedStorage::allocate(l, 8));
    auto rejected = take(sim.reserve(h, 1, storage, Access::read, 0, 8));
    check(used(l, Metric::queue_bytes) == 8, "queue not charged");
    check(sim.submit(rejected, true).code == Code::submit_failed, "rejection not reported");
    check(rejected.observe()->kind == ResultKind::failed && storage->leases() == 0, "rejected submission retained operation");
    check(used(l, Metric::operations) == 0 && used(l, Metric::queue_bytes) == 0, "rejection quota leaked");
    auto unknown = take(sim.reserve(h, 1, storage, Access::write, 0, 8));
    check(sim.submit(unknown, false, true).code == Code::backend_failed, "unknown acceptance hidden");
    check(unknown.phase() == Phase::unknown && h.snapshot().state == Lifecycle::closing, "unknown submit not isolated");
    check(!sim.reserve(h, 1), "isolated resource allowed new submission");
    sim.cancel(unknown); sim.pump();
    check(storage->leases() == 1 && used(l, Metric::handles) == 1, "unknown submit released prematurely");
    sim.settle(unknown, {ResultKind::failed, {Code::backend_failed, "confirmed stopped"}, std::nullopt, {}});
    sim.pump(); storage.reset(); conserved(l);
}
void cancel_reserved() {
    auto l = ledger(); CpuSimulator sim(l); auto h = take(sim.create(1));
    auto e = take(sim.reserve(h, 1));
    check(sim.cancel(e) && e.observe()->kind == ResultKind::canceled, "queued cancel not terminal");
    check(sim.submit(e).code == Code::busy && !sim.settle(e, {}), "canceled reservation reached backend");
    finish(sim, h); conserved(l);
}
void wrong_owner_and_epoch() {
    auto l = ledger(); CpuSimulator sim(l); auto h = take(sim.create(1)); std::vector<Handle> aliases{h};
    check(sim.reserve(aliases[0], 2).error.code == Code::wrong_owner, "nested alias bypassed logical owner");
    check(sim.close(h, 2).error.code == Code::wrong_owner, "foreign task closed resource");
    auto e = take(sim.reserve(h, 1)); sim.submit(e);
    check(sim.transfer_quiescent(h, 1, 2).error.code == Code::busy, "inflight ownership transfer");
    sim.settle(e, {});
    HandleTable table; auto old_boundary = table.publish(h);
    auto moved = take(sim.transfer_quiescent(h, 1, 2));
    check(moved.snapshot().owner_epoch == 2 && used(l, Metric::handles) == 1, "transfer lost resource/quota identity");
    check(sim.reserve(h, 1).error.code == Code::stale_owner && sim.reserve(aliases[0], 2).error.code == Code::stale_owner, "old epoch authorized");
    auto boundary_alias = take(table.resolve(old_boundary));
    check(sim.reserve(boundary_alias, 2).error.code == Code::stale_owner, "boundary resolution renewed stale capability");
    auto fresh = take(sim.reserve(moved, 2)); sim.cancel(fresh); finish(sim, moved, 2);
    check(!table.retire(old_boundary), "closed boundary retirement failed"); conserved(l);
}
void stale_generation() {
    auto l = ledger(); CpuSimulator sim(l); HandleTable table;
    auto h = take(sim.create(1)); auto old = table.publish(h);
    check(table.retire(old).code == Code::busy, "live handle slot reused");
    finish(sim, h); check(!table.retire(old), "retire failed");
    auto next = take(sim.create(1)); auto fresh = table.publish(next);
    check(fresh.slot == old.slot && fresh.generation != old.generation, "generation not advanced on reuse");
    check(table.resolve(old).error.code == Code::stale_handle, "stale generation resolved reused slot");
    check(take(table.resolve(fresh)).snapshot().id == next.snapshot().id, "new generation unresolved");
    finish(sim, next); check(!table.retire(fresh), "second retire failed"); conserved(l);
}
void owner_exit_protocol() {
    auto l = ledger(); CpuSimulator sim(l); auto h = take(sim.create(7));
    auto s = take(FixedStorage::allocate(l, 16)); auto e = take(sim.reserve(h, 7, s, Access::read, 0, 16));
    sim.submit(e); sim.owner_exit(7); s.reset(); sim.pump();
    check(sim.pending() == 1 && used(l, Metric::storage_bytes) == 16 && used(l, Metric::handles) == 1, "owner exit freed in-flight state");
    check(!sim.create(7) && !sim.reserve(h, 7), "exited task reopened admission");
    check(sim.transfer_quiescent(h, 7, 8).error.code == Code::closed, "exited owner transferred capability");
    sim.settle(e, {ResultKind::canceled, {}, std::nullopt, {}}); sim.pump();
    check(sim.pending() == 0 && h.snapshot().state == Lifecycle::closed, "owner cleanup incomplete");
    auto other = take(sim.create(8)); finish(sim, other, 8); conserved(l);
}
void poison_and_confirmation() {
    auto l = ledger(); CpuSimulator sim(l); int calls{};
    auto h = take(sim.create(1, [&] { ++calls; return CloseResult{{Code::backend_failed, "unknown release"}, false}; }));
    auto ticket = take(sim.close(h, 1)); sim.pump();
    check(h.snapshot().state == Lifecycle::poisoned && sim.quarantined() == 1, "unknown release not poisoned");
    check(ticket.observe()->error.code == Code::backend_failed && !ticket.observe()->released, "poison result hidden");
    check(sim.close(h, 1).error.code == Code::poisoned && !sim.reserve(h, 1), "poison reopened");
    sim.pump(); check(calls == 1 && used(l, Metric::handles) == 1, "unknown close retried/refunded");
    check(!sim.confirm_released(h) && h.snapshot().state == Lifecycle::closed, "host release confirmation failed");
    check(ticket.observe()->error.code == Code::backend_failed && !ticket.observe()->released, "historical close result rewritten");
    check(!take(sim.close(h, 1)).observe()->error, "confirmed duplicate close failed");
    check(sim.confirm_released(h).code == Code::busy, "duplicate host confirmation refunded twice");
    conserved(l);
}
void thrown_close() {
    auto l = ledger(); CpuSimulator sim(l);
    auto h = take(sim.create(1, []() -> CloseResult { throw std::runtime_error("backend threw"); }));
    auto t = take(sim.close(h, 1)); sim.pump();
    check(h.snapshot().state == Lifecycle::poisoned && t.observe()->error.code == Code::backend_failed, "throw lost close state");
    sim.confirm_released(h); conserved(l);
}
void reentrant_close() {
    auto l = ledger(); CpuSimulator sim(l); Handle h; Handle created;
    h = take(sim.create(1, [&] {
        check(h.snapshot().active == 0, "callback not quiescent");
        conserved(l, false); check(sim.pending() == 0, "callback under registry lock");
        check(!sim.pump(), "reentrant pump failed");
        created = take(sim.create(2)); return CloseResult{};
    }));
    finish(sim, h); finish(sim, created, 2); conserved(l);
}
void affine_executor() {
    auto l = ledger(); CpuSimulator sim(l, true); auto h = take(sim.create(1));
    auto s = take(FixedStorage::allocate(l, 4)); auto e = take(sim.reserve(h, 1, s, Access::write, 0, 4));
    Failure wrong_submit, wrong_write, wrong_pump;
    std::thread foreign([&] {
        wrong_submit = sim.submit(e);
        const std::uint8_t data[]{1};
        wrong_write = sim.backend_write(e, data); wrong_pump = sim.pump();
    }); foreign.join();
    check(wrong_submit.code == Code::wrong_executor && wrong_write.code == Code::wrong_executor &&
          wrong_pump.code == Code::wrong_executor, "backend affinity ignored");
    check(!sim.submit(e), "owner executor submit failed");
    const std::uint8_t data[]{1, 2}; check(!sim.backend_write(e, data), "executor write failed");
    sim.settle(e, {ResultKind::success, {}, 2, {}}); finish(sim, h);
    check(take(s->copy(0, 4)) == std::vector<std::uint8_t>({1, 2, 0, 0}), "short write fabricated bytes");
    s.reset(); conserved(l);
}
void foreign_runtime() {
    auto l = ledger(); CpuSimulator a(l), b(l); auto h = take(a.create(1)); auto e = take(a.reserve(h, 1));
    check(!b.reserve(h, 1) && !b.close(h, 1) && !b.cancel(e) && !b.settle(e, {}), "cross-runtime capability accepted");
    check(b.submit(e).code == Code::wrong_executor, "foreign backend submit accepted");
    a.cancel(e); finish(a, h); conserved(l);
}
void quota_contention() {
    auto cap = limits(); cap[static_cast<std::size_t>(Metric::handles)] = 1;
    auto l = QuotaLedger::root(cap); auto a = l->child(limits()), b = l->child(limits());
    std::barrier begin(3), held(3); std::atomic<int> granted{}; std::atomic<int> quota_errors{};
    auto run = [&](std::shared_ptr<QuotaLedger> child) {
        begin.arrive_and_wait(); auto r = child->reserve(cost(Metric::handles));
        if (r) ++granted; else if (r.error.code == Code::quota) ++quota_errors;
        held.arrive_and_wait();
    };
    std::thread x(run, a), y(run, b); begin.arrive_and_wait(); held.arrive_and_wait(); x.join(); y.join();
    check(granted == 1 && quota_errors == 1, "ancestor transaction overcommitted");
    conserved(l); conserved(a); conserved(b);
}
void sequential_schedules() {
    // Enumerate close/submit/cancel/completion order, with backend confirmation only after acceptance.
    std::array<int, 4> order{0, 1, 2, 3}; std::size_t schedules{};
    do {
        auto l = ledger(); CpuSimulator sim(l); int releases{};
        auto h = take(sim.create(1, [&] { ++releases; return CloseResult{}; }));
        auto e = take(sim.reserve(h, 1));
        bool close_seen = false;
        for (auto action : order) {
            if (action == 0) { take(sim.close(h, 1)); close_seen = true; }
            if (action == 1) {
                auto result = sim.submit(e);
                if (close_seen) check(bool(result), "close barrier allowed later backend submission");
            }
            if (action == 2) sim.cancel(e);
            if (action == 3) sim.settle(e, {ResultKind::success, {}, 0, {}});
            sim.pump();
        }
        if (!e.observe()) sim.settle(e, {ResultKind::success, {}, 0, {}});
        if (!e.observe()) sim.cancel(e);
        finish(sim, h); check(releases == 1 && sim.pending() == 0, "schedule violated unique release/retirement");
        conserved(l); ++schedules;
    } while (std::next_permutation(order.begin(), order.end()));
    check(schedules == 24, "schedules not exhaustive");
}
void threaded_races() {
    for (int i = 0; i < 200; ++i) {
        auto l = ledger(); CpuSimulator sim(l); std::atomic<int> releases{};
        auto h = take(sim.create(1, [&] { ++releases; return CloseResult{}; }));
        auto e = take(sim.reserve(h, 1)); std::barrier start(3);
        std::thread submit([&] { start.arrive_and_wait(); sim.submit(e); });
        std::thread close([&] { start.arrive_and_wait(); sim.close(h, 1); });
        start.arrive_and_wait(); submit.join(); close.join();
        if (!e.observe()) {
            std::barrier completion_start(3);
            std::thread cancel([&] { completion_start.arrive_and_wait(); sim.cancel(e); });
            std::thread complete([&] { completion_start.arrive_and_wait(); sim.settle(e, {ResultKind::success, {}, 1, {}}); });
            completion_start.arrive_and_wait(); cancel.join(); complete.join();
            check(e.observe()->kind == ResultKind::success, "cancel request overwrote backend success");
        }
        finish(sim, h); check(releases == 1 && h.snapshot().active == 0, "thread race violated lifetime");
        check(!sim.settle(e, {}), "second settlement accepted"); conserved(l);
    }
}
void dropped_observers() {
    auto l = ledger(); std::weak_ptr<FixedStorage> weak; int closes{};
    {
        CpuSimulator sim(l);
        auto h = take(sim.create(1, [&] { ++closes; return CloseResult{}; }));
        auto s = take(FixedStorage::allocate(l, 8)); weak = s;
        auto e = take(sim.reserve(h, 1, s, Access::read, 0, 8)); sim.submit(e);
        e = {}; s.reset(); h = {};
        check(!weak.expired() && sim.pending() == 1, "registry failed to retain work without Event observers");
        sim.owner_exit(1); sim.pump();
        check(closes == 0 && used(l, Metric::inflight_bytes) == 8, "observer loss caused early close/refund");
    }
    check(weak.expired() && closes == 1, "CPU backend-stop did not drain abandoned work");
    conserved(l);
}
void runtime_shutdown() {
    auto l = ledger(); Handle h; Event e;
    {
        CpuSimulator sim(l); h = take(sim.create(1));
        auto s = take(FixedStorage::allocate(l, 8)); e = take(sim.reserve(h, 1, s, Access::read, 0, 8));
        sim.submit(e); sim.shutdown();
        check(sim.pending() == 1 && !sim.create(2), "shutdown faked backend completion");
        // CPU model destructor has its own backend-stop proof; external IO adapters cannot use it.
    }
    check(e.observe()->kind == ResultKind::canceled && h.snapshot().state == Lifecycle::closed, "CPU backend-stop cleanup failed");
    conserved(l);
}
}
int main() {
    const std::pair<const char*, void(*)()> cases[]{
        {"empty_pump_then_first_resource", empty_pump_then_first_resource},
        {"quota_transactions", quota_transactions}, {"accounting_overflow", accounting_overflow},
        {"creation_rollback", creation_rollback}, {"admission_rollback", admission_rollback}, {"alias_leases", alias_leases},
        {"one_close_and_error_record", one_close_and_error_record}, {"close_pending_lifetime", close_pending_lifetime},
        {"event_and_payload_retention", event_and_payload_retention}, {"unknown_progress", unknown_progress},
        {"submission_failures", submission_failures}, {"cancel_reserved", cancel_reserved},
        {"wrong_owner_and_epoch", wrong_owner_and_epoch}, {"stale_generation", stale_generation},
        {"owner_exit_protocol", owner_exit_protocol}, {"poison_and_confirmation", poison_and_confirmation},
        {"thrown_close", thrown_close}, {"reentrant_close", reentrant_close}, {"affine_executor", affine_executor},
        {"foreign_runtime", foreign_runtime}, {"quota_contention", quota_contention},
        {"sequential_schedules", sequential_schedules}, {"threaded_races", threaded_races},
        {"runtime_shutdown", runtime_shutdown}, {"dropped_observers", dropped_observers}
    };
    std::size_t passed{};
    for (const auto& [name, test] : cases) {
        try { test(); ++passed; std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& e) { std::cerr << "FAIL " << name << ": " << e.what() << '\n'; return 1; }
    }
    std::cout << passed << " resource cases, " << checks << " checks; 24 schedules and 200 threaded iterations\n";
}
