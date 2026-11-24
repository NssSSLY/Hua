#pragma once
#include <atomic>
// Shared internal resource protocol. This is not a public Hua type or Native ABI.
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
namespace hua::resources {
using TaskId=std::uint64_t;
enum class Code {none, quota, overflow, allocation, closed, poisoned, wrong_owner, stale_owner,
                 stale_handle, busy, lease_conflict, bounds, wrong_executor, submit_failed, backend_failed};
struct Failure {Code code{Code::none};std::string message;explicit operator bool()const{return code!=Code::none;}};
template<class T> struct Outcome {
    std::optional<T> value;Failure error;
    explicit operator bool()const{return value.has_value();}
};
enum class Metric:std::size_t {handles,storage_bytes,pinned_bytes,operations,inflight_bytes,queue_items,queue_bytes,count};
using Cost=std::array<std::uint64_t,static_cast<std::size_t>(Metric::count)>;
Cost cost(Metric metric,std::uint64_t amount=1);
struct LedgerSnapshot {Cost limit{},used{},reserved{},returned{};};
class QuotaLedger;
class Reservation {
    std::shared_ptr<QuotaLedger> ledger_;Cost amount_{};
    Reservation(std::shared_ptr<QuotaLedger>,Cost);
    friend class QuotaLedger;
public:
    Reservation()=default;Reservation(const Reservation&)=delete;Reservation& operator=(const Reservation&)=delete;
    Reservation(Reservation&&)noexcept;Reservation& operator=(Reservation&&)noexcept;~Reservation();
    void release(Metric);void release();Cost amount()const{return amount_;}
};
class QuotaLedger:public std::enable_shared_from_this<QuotaLedger> {
    struct Impl;std::shared_ptr<Impl> impl_;
    explicit QuotaLedger(std::shared_ptr<Impl>);
    friend class Reservation;
    void release(const Cost&);
public:
    static std::shared_ptr<QuotaLedger> root(Cost limits);
    std::shared_ptr<QuotaLedger> child(Cost limits);
    Outcome<Reservation> reserve(Cost);
    LedgerSnapshot snapshot()const;
};
class ImmutableBytes {
    Reservation allocation_;std::vector<std::uint8_t> data_;
    ImmutableBytes(Reservation,std::span<const std::uint8_t>);
public:
    static Outcome<std::shared_ptr<const ImmutableBytes>> copy(std::shared_ptr<QuotaLedger>,std::span<const std::uint8_t>);
    std::span<const std::uint8_t> data()const{return data_;}
};
enum class Access {read,write};
class FixedStorage;
class Lease {
    std::shared_ptr<FixedStorage> storage_;Access access_{Access::read};std::size_t offset_{},length_{};
    Lease(std::shared_ptr<FixedStorage>,Access,std::size_t,std::size_t);
    friend class FixedStorage;
public:
    Lease()=default;Lease(const Lease&)=delete;Lease& operator=(const Lease&)=delete;
    Lease(Lease&&)noexcept;Lease& operator=(Lease&&)noexcept;~Lease();
    Outcome<std::vector<std::uint8_t>> read()const;
    Failure write(std::span<const std::uint8_t>);
    void release();explicit operator bool()const{return bool(storage_);}
};
class FixedStorage:public std::enable_shared_from_this<FixedStorage> {
    struct Impl;std::unique_ptr<Impl> impl_;
    FixedStorage(Reservation,std::size_t);
    friend class Lease;
public:
    ~FixedStorage();
    static Outcome<std::shared_ptr<FixedStorage>> allocate(std::shared_ptr<QuotaLedger>,std::size_t,bool fail_after_reserve=false);
    Outcome<Lease> lease(Access,std::size_t offset,std::size_t length);
    Outcome<std::vector<std::uint8_t>> copy(std::size_t offset,std::size_t length)const;
    Failure write(std::size_t offset,std::span<const std::uint8_t>);
    std::size_t size()const;std::size_t leases()const;
};
enum class Lifecycle {open,closing,closed,poisoned};
enum class Phase {reserved,submitted,cancel_requested,unknown,retired};
enum class ResultKind {success,failed,canceled};
struct Completion {
    ResultKind kind{ResultKind::success};Failure error;
    std::optional<std::uint64_t> transferred{0};
    std::shared_ptr<const ImmutableBytes> bytes;
};
struct CloseResult {Failure error;bool released{true};};
struct ResourceSnapshot {
    std::uint64_t id{},generation{},owner_epoch{};
    TaskId owner{};Lifecycle state{Lifecycle::open};std::size_t active{};
    bool quiescent{},physical_started{};std::size_t physical_attempts{};
};
struct Control;struct Operation;struct CloseRecord;
class Handle {
    std::shared_ptr<Control> control_;std::uint64_t epoch_{};
    Handle(std::shared_ptr<Control>,std::uint64_t);
    friend class ResourceDomain;friend class HandleTable;
public:
    Handle()=default;ResourceSnapshot snapshot()const;
};
class Event {
    std::shared_ptr<Operation> operation_;
    explicit Event(std::shared_ptr<Operation>);
    friend class ResourceDomain;
public:
    Event()=default;std::optional<Completion> observe()const;Phase phase()const;
};
class CloseTicket {
    std::shared_ptr<CloseRecord> record_;
    explicit CloseTicket(std::shared_ptr<CloseRecord>);
    friend class ResourceDomain;
public:
    CloseTicket()=default;std::optional<CloseResult> observe()const;
};
struct BoundaryHandle {std::size_t slot{};std::uint64_t generation{};};
class HandleTable {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    HandleTable();~HandleTable();
    BoundaryHandle publish(const Handle&);
    Outcome<Handle> resolve(BoundaryHandle);
    Failure retire(BoundaryHandle); // Caller must prove the underlying resource is Closed.
};
class ResourceDomain {
    struct Impl;std::unique_ptr<Impl> impl_;
public:
    explicit ResourceDomain(std::shared_ptr<QuotaLedger>,bool thread_affine=false,bool cpu_stop_proof=false);
    virtual ~ResourceDomain();
    Outcome<Handle> create(TaskId, std::function<CloseResult()> physical_close={},bool fail_after_reserve=false);
    Outcome<Event> reserve(const Handle&,TaskId,std::shared_ptr<FixedStorage> = {},Access=Access::read,
                           std::size_t offset=0,std::size_t length=0,std::shared_ptr<void> external_storage={});
    Failure submit(const Event&,bool fail_before_accept=false,bool acceptance_unknown=false);
    bool cancel(const Event&);
    bool settle(const Event&,Completion); // Backend confirmation: no further access to leased storage.
    Outcome<std::vector<std::uint8_t>> backend_read(const Event&)const;
    Failure backend_write(const Event&,std::span<const std::uint8_t>);
    Outcome<CloseTicket> close(const Handle&,TaskId);
    Failure pump(); // Executes physical close callbacks only on the configured CPU executor.
    // True once any resource was admitted; pump() is a no-op (apart from an ignored
    // executor check) while false, so callers may skip it on their hot path.
    const std::atomic<bool>& created_flag()const noexcept;
    Outcome<Handle> transfer_quiescent(const Handle&,TaskId from,TaskId to); // Internal protocol proof only.
    Failure confirm_released(const Handle&); // Host confirmation of a previously unknown release.
    void owner_exit(TaskId);void shutdown();
    std::size_t pending()const;std::size_t quarantined()const;
};
class CpuSimulator final : public ResourceDomain {
public:
    explicit CpuSimulator(std::shared_ptr<QuotaLedger> ledger,bool thread_affine=false)
        : ResourceDomain(std::move(ledger),thread_affine,true) {}
};
}
