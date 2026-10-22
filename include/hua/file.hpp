#pragma once
#include "hua/resources.hpp"
#include "hua/stdlib.hpp"
#include <span>
namespace hua {
// Host seam for deterministic failures; not a Hua API or Native ABI.
struct FileHostFailure {
    const char* code{};const char* message{};const char* native_domain{};
    std::optional<std::int64_t> native_code;
    explicit operator bool()const{return code!=nullptr;}
};
struct FileHostIO {std::optional<std::int64_t> transferred;FileHostFailure error;};
struct FileHostPosition {std::int64_t value{};FileHostFailure error;};
struct FileHostClose {FileHostFailure error;bool released{true};};
struct FileBackend {
    virtual ~FileBackend()=default;
    virtual FileHostIO read(std::span<char>)=0;
    virtual FileHostIO write(std::span<const char>)=0;
    virtual FileHostPosition position()=0;
    virtual FileHostPosition size()=0;
    virtual FileHostFailure seek(std::int64_t)=0;
    virtual FileHostFailure flush()=0;
    virtual FileHostClose close()=0;
};
struct FileNativeState;
struct FileData {
    std::shared_ptr<class FileScope> scope;
    resources::Handle handle;
    std::shared_ptr<FileNativeState> native;
    ~FileData(); // Admission request only; never invokes a native close.
};
class FileScope:public std::enable_shared_from_this<FileScope> {
public:
    resources::TaskId owner;
    std::shared_ptr<resources::QuotaLedger> root,ledger;
    resources::ResourceDomain protocol;
    const std::atomic<bool>* created{}; // == &protocol.created_flag()
    explicit FileScope(resources::TaskId,std::shared_ptr<resources::QuotaLedger> = {},
                       std::optional<resources::Cost> limits = {});
    ~FileScope();
    Value adopt(std::unique_ptr<FileBackend>,std::string_view mode); // Native tests/host only.
    void pump();void finish();
    resources::LedgerSnapshot usage()const{return ledger->snapshot();}
};
resources::TaskId next_file_task_id();
void prepare_file_scope(RuntimeContext&,const std::shared_ptr<resources::QuotaLedger>& root={});
void file_checkpoint(RuntimeContext*);
void finish_file_scope(RuntimeContext*);
Value invoke_file_standard(const StandardFunction&,const std::vector<Value>&,const SourceSpan&,RuntimeContext*);
}
