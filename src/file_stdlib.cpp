#include "hua/performance.hpp"
#include "hua/file.hpp"
#include "hua/tasks.hpp"
#include "hua/resume.hpp"
#include <cstdlib>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <limits>
#include <mutex>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#endif
namespace hua {
namespace {
using I=std::int64_t;namespace R=resources;
constexpr std::size_t max_io=1024*1024;
FileHostFailure failure(const char* code,const char* message){return {code,message,nullptr,{}};}
R::Cost limits(bool root){
    R::Cost out{};out[std::size_t(R::Metric::handles)]=root?256:64;
    out[std::size_t(R::Metric::storage_bytes)]=(root?64:16)*1024*1024;
    out[std::size_t(R::Metric::operations)]=64;out[std::size_t(R::Metric::inflight_bytes)]=(root?64:16)*1024*1024;
    out[std::size_t(R::Metric::queue_items)]=64;out[std::size_t(R::Metric::queue_bytes)]=(root?64:16)*1024*1024;
    return out;
}
struct Mode {bool read{},write{},append{},truncate{},create{};};
std::optional<Mode> mode(std::string_view s){
    if(s=="r")return Mode{true,false,false,false,false};
    if(s=="w")return Mode{false,true,false,true,true};
    if(s=="a")return Mode{false,true,true,false,true};
    if(s=="r+")return Mode{true,true,false,false,false};
    if(s=="w+")return Mode{true,true,false,true,true};
    if(s=="a+")return Mode{true,true,true,false,true};
    return {};
}
FileHostFailure resource_failure(const R::Failure& f){
    using C=R::Code;
    const char* code=f.code==C::quota?"limit":f.code==C::allocation?"allocation":
        f.code==C::wrong_owner||f.code==C::stale_owner?"wrong_owner":
        f.code==C::poisoned?"poisoned":f.code==C::closed?"closed":"resource";
    return failure(code,"resource admission rejected");
}
ErrorValue error(FileHostFailure f,std::string_view op){
    ErrorData d;d.domain="file";d.code=f.code?f.code:"io";d.message=f.message?f.message:"file operation failed";
    d.origin="std.file."+std::string(op);if(f.native_domain)d.native_domain=f.native_domain;d.native_code=f.native_code;
    return {managed<const ErrorData>(std::move(d))};
}
Value result(Value payload,bool ok,std::string type,std::string err="std.error.Value"){
    return Value(ResultValue{ok,result_box(std::move(payload)),std::move(type),std::move(err),true,false});
}
Value void_ok(){return Value(ResultValue{true,{},"void","std.error.Value",true,false});}
Value failed(FileHostFailure f,std::string_view op,std::string type){return result(Value(error(f,op)),false,std::move(type));}
Value io_failed(FileHostFailure f,std::string_view op,std::optional<I> count={},std::optional<BytesValue> partial={}){
    return result(Value(IOFailureValue{managed<const IOFailureData>(IOFailureData{error(f,op),count,std::move(partial)})}),
                  false,op=="read"||op=="read_exact"?"Bytes?":op=="read_all"?"Bytes":op=="read_line"?"string?":op=="write_all"?"void":"int","std.file.IOFailure");
}
struct QuotaBytes {R::Reservation reservation;std::string data;
    QuotaBytes(R::Reservation p,std::size_t n):reservation(std::move(p)),data(n,'\0'){}
};
#ifdef _WIN32
FileHostFailure host_failure(DWORD n){
    const char* code=n==ERROR_FILE_NOT_FOUND||n==ERROR_PATH_NOT_FOUND?"not_found":
        n==ERROR_ACCESS_DENIED||n==ERROR_SHARING_VIOLATION?"permission_denied":
        n==ERROR_DISK_FULL||n==ERROR_HANDLE_DISK_FULL?"no_space":
        n==ERROR_OPERATION_ABORTED?"interrupted":n==ERROR_WRITE_PROTECT?"read_only":"io";
    return {code,"native file operation failed","win32",static_cast<I>(n)};
}
// Only the completion thread observes the kernel packet. The task resumes
// through the shared scheduler and retains the request until that packet arrives.
struct FileCompletion : OVERLAPPED {
    std::atomic<bool> done{false};
    DWORD count{},error{};
    explicit FileCompletion(I offset) : OVERLAPPED{} {
        const auto value=static_cast<std::uint64_t>(offset);
        Offset=static_cast<DWORD>(value);
        OffsetHigh=static_cast<DWORD>(value>>32);
    }
};
class FileCompletionPort {
    HANDLE port=CreateIoCompletionPort(INVALID_HANDLE_VALUE,nullptr,0,1);
    std::jthread thread;
public:
    FileCompletionPort(){
        if(!port)return;
        try{
            thread=std::jthread([this]{
                for(;;){
                    DWORD bytes{};ULONG_PTR key{};OVERLAPPED* request{};
                    const bool ok=GetQueuedCompletionStatus(port,&bytes,&key,&request,INFINITE);
                    const auto error=ok?ERROR_SUCCESS:GetLastError();
                    if(!request)return;
                    auto& completion=*static_cast<FileCompletion*>(request);
                    completion.count=bytes;
                    completion.error=error;
                    completion.done.store(true,std::memory_order_release);
                    notify_task_scheduler();
                }
            });
        }catch(...){CloseHandle(port);throw;}
    }
    ~FileCompletionPort(){
        if(!port)return;
        if(!PostQueuedCompletionStatus(port,0,0,nullptr)){
            const auto handle=port;
            CloseHandle(handle);
            thread.join();
        }else{
            thread.join();
            CloseHandle(port);
        }
    }
    bool associate(HANDLE file) const {
        return port&&CreateIoCompletionPort(file,port,0,0)==port;
    }
};
FileCompletionPort& file_completion_port(){static FileCompletionPort port;return port;}

class NativeFile final:public FileBackend {
    HANDLE handle_;bool append_,overlapped_;
    I cursor_{};
    FileHostIO complete(bool reading,DWORD bytes,DWORD error){
        if(reading&&error==ERROR_HANDLE_EOF)return {I{0},{}};
        if(error)return {{},host_failure(error)};
        if(overlapped_){
            if(append_&&!reading){
                auto end=size();
                if(end.error)return {static_cast<I>(bytes),end.error};
                cursor_=end.value;
            }else cursor_+=static_cast<I>(bytes);
        }
        return {static_cast<I>(bytes),{}};
    }
    bool issue(bool reading,char* storage,std::size_t size,OVERLAPPED& request){
        if(reading)return ReadFile(handle_,storage,static_cast<DWORD>(size),nullptr,&request);
        return WriteFile(handle_,storage,static_cast<DWORD>(size),nullptr,&request);
    }
    FileHostIO blocking_io(bool reading,char* storage,std::size_t size){
        FileCompletion request(append_&&!reading?-1:cursor_);
        const auto event=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(!event)return {{},host_failure(GetLastError())};
        struct CloseEvent {HANDLE handle;~CloseEvent(){CloseHandle(handle);}} guard{event};
        // Low bit suppresses the IOCP packet for this blocking fallback.
        request.hEvent=reinterpret_cast<HANDLE>(reinterpret_cast<std::uintptr_t>(event)|1);
        if(!issue(reading,storage,size,request)){
            const auto error=GetLastError();
            if(error!=ERROR_IO_PENDING)return complete(reading,0,error);
        }
        DWORD bytes{};
        const auto ok=GetOverlappedResult(handle_,&request,&bytes,TRUE);
        return complete(reading,bytes,ok?ERROR_SUCCESS:GetLastError());
    }
public:
    explicit NativeFile(HANDLE h,bool append,bool overlapped):handle_(h),append_(append),overlapped_(overlapped){}
    bool overlapped() const {return overlapped_;}
    Routine<FileHostIO> transfer(bool reading,char* storage,std::size_t size,RuntimeContext& context){
        FileCompletion completion(append_&&!reading?-1:cursor_);
        const auto finished=[&completion]{return completion.done.load(std::memory_order_acquire);};
        // Allocate wait predicates before exposing OVERLAPPED to the OS.
        Suspend interruptible{[&]{return finished()||(!context.cleanup&&context.cancellation&&context.cancellation->stopped());}};
        Suspend confirmed{finished};
        if(!issue(reading,storage,size,completion)){
            const auto error=GetLastError();
            if(error!=ERROR_IO_PENDING)co_return complete(reading,0,error);
        }
        co_await std::move(interruptible);
        if(!finished()){
            CancelIoEx(handle_,&completion);
            co_await std::move(confirmed);
        }
        co_return complete(reading,completion.count,completion.error);
    }
    void attach(HANDLE h){handle_=h;}
    ~NativeFile()override{if(handle_!=INVALID_HANDLE_VALUE)close();}
    FileHostIO read(std::span<char> b)override{
        if(overlapped_)return blocking_io(true,b.data(),b.size());
        DWORD n{};if(ReadFile(handle_,b.data(),static_cast<DWORD>(b.size()),&n,nullptr))return {static_cast<I>(n),{}};
        auto e=GetLastError();if(e==ERROR_HANDLE_EOF)return {I{0},{}};return {{},host_failure(e)};
    }
    FileHostIO write(std::span<const char> b)override{
        if(overlapped_)return blocking_io(false,const_cast<char*>(b.data()),b.size());
        DWORD n{};OVERLAPPED end{};end.Offset=end.OffsetHigh=0xffffffff;
        if(WriteFile(handle_,b.data(),static_cast<DWORD>(b.size()),&n,append_?&end:nullptr))return {static_cast<I>(n),{}};
        auto e=GetLastError();return {{},host_failure(e)};
    }
    FileHostPosition position()override{
        if(overlapped_)return {cursor_,{}};
        LARGE_INTEGER zero{},pos{};if(SetFilePointerEx(handle_,zero,&pos,FILE_CURRENT))return {pos.QuadPart,{}};
        return {0,host_failure(GetLastError())};
    }
    FileHostPosition size()override{
        LARGE_INTEGER n{};if(GetFileSizeEx(handle_,&n))return {n.QuadPart,{}};
        return {0,host_failure(GetLastError())};
    }
    FileHostFailure seek(I n)override{
        if(overlapped_){cursor_=n;return {};}
        LARGE_INTEGER pos{};pos.QuadPart=n;if(SetFilePointerEx(handle_,pos,nullptr,FILE_BEGIN))return {};
        return host_failure(GetLastError());
    }
    FileHostFailure flush()override{if(FlushFileBuffers(handle_))return {};return host_failure(GetLastError());}
    FileHostClose close()override{
        auto h=handle_;handle_=INVALID_HANDLE_VALUE;
        if(CloseHandle(h))return {};return {host_failure(GetLastError()),false};
    }
};
#else
FileHostFailure host_failure(int n){
    const char* code=n==ENOENT?"not_found":n==EACCES||n==EPERM?"permission_denied":
        n==ENOSPC?"no_space":n==EINTR?"interrupted":n==EROFS?"read_only":"io";
    return {code,"native file operation failed","errno",static_cast<I>(n)};
}
class NativeFile final:public FileBackend {
    int fd_;
public:
    explicit NativeFile(int fd,bool):fd_(fd){}
    void attach(int fd){fd_=fd;}
    ~NativeFile()override{if(fd_>=0)close();}
    FileHostIO read(std::span<char> b)override{auto n=::read(fd_,b.data(),b.size());if(n>=0)return {n,{}};return {{},host_failure(errno)};}
    FileHostIO write(std::span<const char> b)override{auto n=::write(fd_,b.data(),b.size());if(n>=0)return {n,{}};return {{},host_failure(errno)};}
    FileHostPosition position()override{auto n=lseek(fd_,0,SEEK_CUR);if(n>=0)return {n,{}};return {0,host_failure(errno)};}
    FileHostPosition size()override{struct stat s{};if(!fstat(fd_,&s))return {s.st_size,{}};return {0,host_failure(errno)};}
    FileHostFailure seek(I n)override{if(lseek(fd_,n,SEEK_SET)>=0)return {};return host_failure(errno);}
    FileHostFailure flush()override{if(!fsync(fd_))return {};return host_failure(errno);}
    // POSIX failure may leave descriptor state unknown. Never retry a numeric fd after failure.
    FileHostClose close()override{auto fd=fd_;fd_=-1;if(!::close(fd))return {};return {host_failure(errno),false};}
};
#endif
struct Opened {std::unique_ptr<FileBackend> backend;FileHostFailure error;};
Opened native_open(const std::filesystem::path& path,Mode m,[[maybe_unused]] bool overlapped){
#ifdef _WIN32
    auto text=path.native();std::replace(text.begin(),text.end(),L'/',L'\\');
    if(text.starts_with(L"\\\\?\\")||text.starts_with(L"\\\\.\\"))
        return {{},failure("invalid_argument","device namespaces are unsupported")};
    std::size_t start=0;
    while(start<text.size()){
        auto end=text.find(L'\\',start);if(end==std::wstring::npos)end=text.size();
        auto part=text.substr(start,end-start);
        if(part.find(L':')!=std::wstring::npos && !(start==0&&part.size()==2&&part[1]==L':'))
            return {{},failure("invalid_argument","alternate streams and drive-relative paths are unsupported")};
        std::transform(part.begin(),part.end(),part.begin(),[](wchar_t c){return c>=L'a'&&c<=L'z'?c-(L'a'-L'A'):c;});
        auto base=part.substr(0,part.find(L'.'));while(!base.empty()&&(base.back()==L' '||base.back()==L'.'))base.pop_back();
        if(base==L"CON"||base==L"PRN"||base==L"AUX"||base==L"NUL"||base==L"CLOCK$"||
            (base.size()==4&&(base.starts_with(L"COM")||base.starts_with(L"LPT"))&&base[3]>=L'1'&&base[3]<=L'9')||
            (text.starts_with(L"\\\\")&&base==L"PIPE"))
            return {{},failure("not_regular","only regular disk files are supported")};
        start=end+1;
    }
    // Validate the opened handle before truncation: path prechecks alone are subject to races.
    auto backend=std::make_unique<NativeFile>(INVALID_HANDLE_VALUE,m.append,overlapped);
    auto h=CreateFileW(text.c_str(),(m.read?GENERIC_READ:0)|(m.write?GENERIC_WRITE:0),
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,m.create?OPEN_ALWAYS:OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|(overlapped?FILE_FLAG_OVERLAPPED:0),nullptr);
    if(h==INVALID_HANDLE_VALUE)return {{},host_failure(GetLastError())};
    backend->attach(h);
    BY_HANDLE_FILE_INFORMATION info{};
    if(GetFileType(h)!=FILE_TYPE_DISK)return {std::move(backend),failure("not_regular","only regular disk files are supported")};
    if(!GetFileInformationByHandle(h,&info))return {std::move(backend),host_failure(GetLastError())};
    if(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)return {std::move(backend),failure("not_regular","directories cannot be opened as File")};
    if(m.truncate&&!SetEndOfFile(h))return {std::move(backend),host_failure(GetLastError())};
    if(overlapped&&!file_completion_port().associate(h))
        return {std::move(backend),host_failure(GetLastError())};
#else
    int flags=m.read?(m.write?O_RDWR:O_RDONLY):O_WRONLY;
    if(m.create)flags|=O_CREAT;if(m.append)flags|=O_APPEND;
    auto backend=std::make_unique<NativeFile>(-1,m.append);
    auto fd=::open(path.c_str(),flags|O_CLOEXEC|O_NONBLOCK,0666);
    if(fd<0)return {{},host_failure(errno)};
    backend->attach(fd);
    struct stat info{};if(fstat(fd,&info))return {std::move(backend),host_failure(errno)};
    if(!S_ISREG(info.st_mode))return {std::move(backend),failure("not_regular","only regular disk files are supported")};
    if(m.truncate&&ftruncate(fd,0))return {std::move(backend),host_failure(errno)};
#endif
    if(m.append){auto end=backend->size();if(end.error)return {std::move(backend),end.error};if(auto e=backend->seek(end.value))return {std::move(backend),e};}
    return {std::move(backend),{}};
}
}
struct FileNativeState {
    std::mutex mutex;std::unique_ptr<FileBackend> backend;Mode mode;FileHostFailure first_close_error;
    // Read-ahead of read_line: bytes already taken from the host but not yet delivered.
    // Invisible to programs: read* consume it first, tell subtracts it, seek and write drop it.
    std::string pending;std::size_t pending_at{};std::optional<R::Reservation> pending_quota;std::size_t pending_charged{};
    std::size_t buffered()const{return pending.size()-pending_at;}
    void drop_pending(){
        if(pending.capacity()>1024*1024)std::string().swap(pending);else pending.clear();
        pending_at=0;pending_quota.reset();pending_charged=0;
    }
    explicit FileNativeState(Mode m):mode(m){}
};
FileScope::FileScope(R::TaskId id,std::shared_ptr<R::QuotaLedger> r,std::optional<R::Cost> custom)
    :owner(id),root(r?r:R::QuotaLedger::root(limits(true))),ledger(root->child(custom.value_or(limits(false)))),protocol(ledger),created(&protocol.created_flag()){}
FileScope::~FileScope(){try{finish();}catch(...){}}
void FileScope::pump(){protocol.pump();}
void FileScope::finish(){protocol.owner_exit(owner);pump();}
FileData::~FileData(){try{if(scope)scope->protocol.close(handle,scope->owner);}catch(...){}}
R::TaskId next_file_task_id(){
    static std::atomic<R::TaskId> next{1};auto n=next.load();
    do{if(n==std::numeric_limits<R::TaskId>::max())runtime_error({},"logical Task ID exhausted","E4099");}while(!next.compare_exchange_weak(n,n+1));
    return n;
}
void prepare_file_scope(RuntimeContext& c,const std::shared_ptr<R::QuotaLedger>& root){
    if(!c.task_id)c.task_id=next_file_task_id();
    if(!c.files)c.files=std::make_shared<FileScope>(c.task_id,root);
}
void file_checkpoint(RuntimeContext* c){HUA_PROFILE_COUNT(file_checkpoints);if(c&&c->files)c->files->pump();}
void finish_file_scope(RuntimeContext* c){if(c&&c->files)c->files->finish();}
namespace {
R::Outcome<R::Handle> admit(FileScope& scope,const std::shared_ptr<FileNativeState>& native){
    return scope.protocol.create(scope.owner,[native]{
        std::lock_guard lock(native->mutex);
        if(!native->backend)return R::CloseResult{};
        FileHostClose closed;
        try{closed=native->backend->close();}catch(...){closed={failure("io","host close threw; release unknown"),false};}
        if(!closed.released&&!closed.error)closed.error=failure("poisoned","physical release unknown");
        if(closed.error&&!native->first_close_error)native->first_close_error=closed.error;
        // Retain the backend after an uncertain close, rather than invoke its destructor.
        if(closed.released)native->backend.reset();
        return R::CloseResult{closed.error?R::Failure{R::Code::backend_failed,"native close failed"}:R::Failure{},closed.released};
    });
}
Value publish(const std::shared_ptr<FileScope>& scope,R::Handle h,std::shared_ptr<FileNativeState> native){
    try{auto d=managed<FileData>();d->scope=scope;d->handle=h;d->native=std::move(native);
        return result(Value(FileValue{d}),true,"std.file.File");
    }catch(...){scope->protocol.close(h,scope->owner);scope->pump();throw;}
}
FileHostFailure check(const std::shared_ptr<FileData>& f,RuntimeContext* c,bool open=true){
    if(!f||!c||!c->files||c->task_id!=f->scope->owner||c->files.get()!=f->scope.get())
        return failure("wrong_owner","File belongs to another logical Task");
    if(!open)return {};
    auto s=f->handle.snapshot().state;
    if(s==R::Lifecycle::poisoned)return failure("poisoned","physical release is unknown");
    if(s!=R::Lifecycle::open)return failure("closed","File admission is closed");
    return {};
}
struct OperationGuard {
    R::ResourceDomain& domain;R::Event event;bool submitted{};
    ~OperationGuard(){if(submitted)domain.settle(event,{R::ResultKind::failed,{},std::nullopt,{}});else domain.cancel(event);}
};
constexpr std::size_t read_ahead=64*1024,max_line=16*1024*1024,max_bulk=128*1024*1024;
static Routine<FileHostIO> file_io(FileNativeState& native,[[maybe_unused]] RuntimeContext& context,
    std::unique_lock<std::mutex>& lock,[[maybe_unused]] bool suspend,bool reading,char* data,std::size_t size){
    try{
#ifdef _WIN32
        if(suspend){
            auto& backend=static_cast<NativeFile&>(*native.backend);
            // The resource event and File owner retain the handle and storage.
            // Drop the mutex before suspension so VM workers can migrate the frame.
            lock.unlock();
            auto result=co_await backend.transfer(reading,data,size,context);
            lock.lock();
            co_return result;
        }
#endif
        co_return reading?native.backend->read(std::span<char>(data,size)):
                          native.backend->write(std::span<const char>(data,size));
    }catch(...){
        if(!lock.owns_lock())lock.lock();
        co_return FileHostIO{{},failure("io","host operation threw; progress unknown")};
    }
}
// One admitted host call of a bulk operation: the same reserve/submit/settle as read and write.
template<class Call> Routine<FileHostIO> guarded_io(FileData& file,RuntimeContext* c,std::size_t n,std::shared_ptr<void> keepalive,Call call){
    auto admitted=file.scope->protocol.reserve(file.handle,c->task_id,{},R::Access::read,0,n,std::move(keepalive));
    if(!admitted)co_return FileHostIO{I{0},resource_failure(admitted.error)};
    OperationGuard operation{file.scope->protocol,*admitted.value};
    if(auto e=file.scope->protocol.submit(operation.event))co_return FileHostIO{I{0},resource_failure(e)};
    operation.submitted=true;FileHostIO observed;
    try{observed=co_await call();}catch(...){observed={{},failure("io","host operation threw; progress unknown")};}
    if((observed.transferred&&(*observed.transferred<0||*observed.transferred>static_cast<I>(n)))||(!observed.error&&!observed.transferred))
        observed={{},failure("io","invalid host progress; progress unknown")};
    file.scope->protocol.settle(operation.event,{observed.error?R::ResultKind::failed:R::ResultKind::success,{},observed.transferred?std::optional<std::uint64_t>(*observed.transferred):std::nullopt,{}});
    operation.submitted=false;
    co_return observed;
}
// Reads at most n bytes into `out`.
static Routine<FileHostIO> read_chunk(FileData& file,FileNativeState& native,RuntimeContext* c,
    std::unique_lock<std::mutex>& lock,bool suspend,std::size_t n,std::string& out){
    auto permit=file.scope->ledger->reserve(R::cost(R::Metric::storage_bytes,n));
    if(!permit)co_return FileHostIO{I{0},resource_failure(permit.error)};
    auto storage=std::make_shared<QuotaBytes>(std::move(*permit.value),n);
    auto got=co_await guarded_io(file,c,n,storage,[&]{return file_io(native,*c,lock,suspend,true,storage->data.data(),n);});
    if(got.transferred&&*got.transferred>0)out.append(storage->data.data(),static_cast<std::size_t>(*got.transferred));
    co_return got;
}
// read_line's buffer is charged to the File quota, so one long line cannot go unaccounted.
FileHostFailure charge_pending(FileData& file,FileNativeState& native,std::size_t size){
    if(size<=native.pending_charged)return {};
    const auto doubled=std::max(size,native.pending_charged*2);
    native.pending_quota.reset();native.pending_charged=0;
    auto permit=file.scope->ledger->reserve(R::cost(R::Metric::storage_bytes,doubled));
    auto want=doubled;
    if(!permit){want=size;permit=file.scope->ledger->reserve(R::cost(R::Metric::storage_bytes,want));}
    if(!permit)return resource_failure(permit.error);
    native.pending_quota=std::move(*permit.value);native.pending_charged=want;
    return {};
}
// Puts the host cursor back at the logical position before a write.
FileHostFailure settle_read_ahead(FileNativeState& native){
    if(native.buffered()&&!native.mode.append){
        auto position=native.backend->position();if(position.error)return position.error;
        if(auto e=native.backend->seek(position.value-static_cast<I>(native.buffered())))return e;
    }
    native.drop_pending();
    return {};
}
Value ok_bytes(std::string text,const char* type){return result(Value(BytesValue{managed<const std::string>(std::move(text))}),true,type,"std.file.IOFailure");}
BytesValue partial_bytes(std::string text){return BytesValue{managed<const std::string>(std::move(text))};}
Routine<Value> bulk_operation(const std::string& name,FileData& file,FileNativeState& native,const std::vector<Value>& args,const SourceSpan& span,RuntimeContext* c,std::unique_lock<std::mutex>& lock,bool suspend){
    if(name=="write_all"){
        if(!native.mode.write)co_return io_failed(failure("permission_denied","File mode does not permit this operation"),name,I{0});
        if(auto e=settle_read_ahead(native))co_return io_failed(e,name,I{0});
        auto data=hua::get<BytesValue>(args[1].data).data;I total=0;
        for(std::size_t at=0;at<data->size();){
            if(at)task_checkpoint(c,span);
            const auto n=std::min(data->size()-at,max_io);
            auto wrote=co_await guarded_io(file,c,n,std::const_pointer_cast<std::string>(data),[&]{
                return file_io(native,*c,lock,suspend,false,const_cast<char*>(data->data()+at),n);
            });
            if(wrote.error)co_return io_failed(wrote.error,name,wrote.transferred?std::optional<I>(total+*wrote.transferred):std::nullopt);
            if(*wrote.transferred==0)co_return io_failed(failure("io","host write made no progress"),name,total);
            total+=*wrote.transferred;at+=static_cast<std::size_t>(*wrote.transferred);
        }
        co_return Value(ResultValue{true,{},"void","std.file.IOFailure",true,false});
    }
    if(!native.mode.read)co_return io_failed(failure("permission_denied","File mode does not permit this operation"),name,I{0});
    if(name=="read_line"){
        std::size_t scan=native.pending_at;
        for(;;){
            const auto newline=native.pending.find('\n',scan);
            const bool at_eof=scan==std::string::npos;
            if(newline!=std::string::npos||at_eof){
                const auto end=newline!=std::string::npos?newline:native.pending.size();
                auto line=native.pending.substr(native.pending_at,end-native.pending_at);
                native.pending_at=newline!=std::string::npos?newline+1:native.pending.size();
                if(native.pending_at==native.pending.size())native.drop_pending();
                if(newline!=std::string::npos&&!line.empty()&&line.back()=='\r')line.pop_back();
                if(!text_utf8_error(line).empty()){const auto size=static_cast<I>(line.size());co_return io_failed(failure("invalid_utf8","line is not valid UTF-8"),name,size,partial_bytes(std::move(line)));}
                co_return result(Value(std::move(line)),true,"string?","std.file.IOFailure");
            }
            if(native.buffered()>=max_line)co_return io_failed(failure("limit","line exceeds 16 MiB"),name,I{0});
            if(native.pending_at){native.pending.erase(0,native.pending_at);native.pending_at=0;}
            scan=native.pending.size();
            if(auto e=charge_pending(file,native,native.pending.size()+read_ahead))co_return io_failed(e,name,I{0});
            task_checkpoint(c,span);
            auto got=co_await read_chunk(file,native,c,lock,suspend,read_ahead,native.pending);
            if(got.error)co_return io_failed(got.error,name,I{0});
            if(*got.transferred==0){
                if(native.pending.empty())co_return result(Value{},true,"string?","std.file.IOFailure");
                scan=std::string::npos;
            }
        }
    }
    // read_all and read_exact: buffered bytes first, then the host in chunks up to max_io.
    const bool exact=name=="read_exact";
    const auto wanted=exact?as_int(args[1],span):static_cast<I>(max_bulk);
    if(wanted<0||wanted>static_cast<I>(max_bulk))co_return io_failed(failure("limit","one File operation requires 0..134217728 bytes"),name,I{0});
    const auto goal=static_cast<std::size_t>(wanted);std::string out;
    const auto take=std::min(goal,native.buffered());
    out.append(native.pending,native.pending_at,take);native.pending_at+=take;
    if(!native.buffered())native.drop_pending();
    while(!exact||out.size()<goal){
        if(!out.empty())task_checkpoint(c,span);
        if(!exact&&out.size()>goal){const auto size=static_cast<I>(out.size());co_return io_failed(failure("limit","file exceeds 128 MiB"),name,size,partial_bytes(std::move(out)));}
        // read_all probes one byte past the limit, so a file of exactly the limit still succeeds.
        const auto n=out.size()<goal?std::min(goal-out.size(),max_io):std::size_t{1};
        auto got=co_await read_chunk(file,native,c,lock,suspend,n,out);
        if(got.error){const auto size=static_cast<I>(out.size());co_return io_failed(got.error,name,size,out.empty()?std::optional<BytesValue>{}:std::optional<BytesValue>(partial_bytes(std::move(out))));}
        if(*got.transferred==0){
            if(!exact)break;
            if(out.empty())co_return result(Value{},true,"Bytes?","std.file.IOFailure");
            const auto size=static_cast<I>(out.size());co_return io_failed(failure("unexpected_eof","file ended before the requested count"),name,size,partial_bytes(std::move(out)));
        }
    }
    co_return ok_bytes(std::move(out),exact?"Bytes?":"Bytes");
}
}
Value FileScope::adopt(std::unique_ptr<FileBackend> backend,std::string_view text){
    auto m=mode(text);if(!m||!backend)return failed(failure("bad_mode","invalid native File adoption"),"open","std.file.File");
    auto native=std::make_shared<FileNativeState>(*m);native->backend=std::move(backend);
    auto h=admit(*this,native);if(!h)return failed(resource_failure(h.error),"open","std.file.File");
    return publish(shared_from_this(),*h.value,std::move(native));
}
static Routine<Value> transfer_file(const StandardFunction& f,const std::vector<Value>& args,
                                   const SourceSpan& span,RuntimeContext* c,std::shared_ptr<FileData> file,
                                   std::unique_lock<std::mutex> lock,[[maybe_unused]] bool allow_suspend){
    auto& native=*file->native;
    const auto bad=[&](FileHostFailure error){return io_failed(error,f.name,I{0});};
    I n=f.name=="read"?as_int(args[1],span):static_cast<I>(hua::get<BytesValue>(args[1].data).data->size());
    if(n<0||n>static_cast<I>(max_io))co_return bad(failure("limit","one File operation requires 0..1048576 bytes"));
    if(f.name=="read"?!native.mode.read:!native.mode.write)co_return bad(failure("permission_denied","File mode does not permit this operation"));
    if(!n)co_return result(f.name=="read"?Value(BytesValue{managed<const std::string>()}):Value(I{0}),true,f.name=="read"?"Bytes?":"int","std.file.IOFailure");
    if(f.name=="read"&&native.buffered()){
        const auto take=std::min(native.buffered(),static_cast<std::size_t>(n));
        auto bytes=native.pending.substr(native.pending_at,take);native.pending_at+=take;
        if(!native.buffered())native.drop_pending();
        co_return ok_bytes(std::move(bytes),"Bytes?");
    }
    if(f.name=="write")if(auto e=settle_read_ahead(native))co_return bad(e);
    std::shared_ptr<QuotaBytes> storage;std::shared_ptr<void> keepalive;
    if(f.name=="read"){
        auto permit=file->scope->ledger->reserve(R::cost(R::Metric::storage_bytes,static_cast<std::uint64_t>(n)));
        if(!permit)co_return bad(resource_failure(permit.error));
        try{storage=std::make_shared<QuotaBytes>(std::move(*permit.value),static_cast<std::size_t>(n));}
        catch(const std::bad_alloc&){co_return bad(failure("allocation","read allocation failed before native admission"));}
        keepalive=storage;
    }else keepalive=std::const_pointer_cast<std::string>(hua::get<BytesValue>(args[1].data).data);
    auto admitted=file->scope->protocol.reserve(file->handle,c->task_id,{},R::Access::read,0,static_cast<std::size_t>(n),keepalive);
    if(!admitted)co_return bad(resource_failure(admitted.error));
    OperationGuard operation{file->scope->protocol,*admitted.value};
    if(auto e=file->scope->protocol.submit(operation.event))co_return bad(resource_failure(e));
    operation.submitted=true;
    // Preallocate both outcomes and register retained storage before entering the OS.
    // No managed allocation after a read/write can erase confirmed progress.
    auto detail=managed<ErrorData>();detail->domain="file";detail->origin="std.file."+f.name;
    detail->code.reserve(128);detail->message.reserve(4096);detail->native_domain.reserve(128);
    auto issue=managed<IOFailureData>();issue->error={detail};
    auto success=result({},true,f.name=="read"?"Bytes?":"int","std.file.IOFailure");
    auto unsuccessful=result(Value(IOFailureValue{issue}),false,f.name=="read"?"Bytes?":"int","std.file.IOFailure");
    std::optional<BytesValue> bytes;
    if(storage){std::shared_ptr<const std::string> alias(storage,&storage->data);managed_heap().track(alias);bytes=BytesValue{std::move(alias)};}
    const bool reading=f.name=="read";
    auto* data=reading?storage->data.data():const_cast<char*>(hua::get<BytesValue>(args[1].data).data->data());
    auto observed=co_await file_io(native,*c,lock,allow_suspend,reading,data,static_cast<std::size_t>(n));
    if((observed.transferred&&(*observed.transferred<0||*observed.transferred>n))||(!observed.error&&!observed.transferred))
        observed={{},failure("io","invalid host progress; progress unknown")};
    if(storage&&observed.transferred&&*observed.transferred>0)storage->data.resize(static_cast<std::size_t>(*observed.transferred));
    else bytes.reset();
    file->scope->protocol.settle(operation.event,{observed.error?R::ResultKind::failed:R::ResultKind::success,{},observed.transferred?std::optional<std::uint64_t>(*observed.transferred):std::nullopt,{}});
    operation.submitted=false;
    if(observed.error){
        auto assign=[](std::string& to,const char* from,std::size_t max){auto text=std::string_view(from?from:"");to.assign(text.substr(0,max));};
        assign(detail->code,observed.error.code,128);assign(detail->message,observed.error.message,4096);
        assign(detail->native_domain,observed.error.native_domain,128);detail->native_code=observed.error.native_code;
        issue->transferred=observed.transferred;issue->partial=bytes;co_return unsuccessful;
    }
    *cget<ResultValue>(success.data).payload=f.name=="read"?(bytes?Value(*bytes):Value{}):Value(*observed.transferred);
    co_return success;
}
#ifdef _WIN32
bool file_has_completion(const std::vector<Value>& args){
    if(args.empty())return false;
    auto value=hua::get_if<FileValue>(&args[0].data);
    if(!value||!value->data)return false;
    const auto& file=*value->data;
    if(!file.native)return false;
    std::lock_guard lock(file.native->mutex);
    auto backend=dynamic_cast<const NativeFile*>(file.native->backend.get());
    return backend&&backend->overlapped();
}
Routine<Value> invoke_completion_file(const StandardFunction& f,const std::vector<Value>& args,
                                      const SourceSpan& span,RuntimeContext* c){
    auto file=hua::get<FileValue>(args[0].data).data;
    if(auto error=check(file,c))co_return io_failed(error,f.name,I{0});
    auto lock=std::unique_lock(file->native->mutex);
    if(f.since==29)co_return co_await bulk_operation(f.name,*file,*file->native,args,span,c,lock,true);
    co_return co_await transfer_file(f,args,span,c,std::move(file),std::move(lock),true);
}
#endif
Value invoke_file_standard(const StandardFunction& f,const std::vector<Value>& args,const SourceSpan& span,RuntimeContext* c){
    if(f.name=="error"||f.name=="transferred"||f.name=="partial"){
        auto io=hua::get_if<IOFailureValue>(&args[0].data);if(!io||!io->data)runtime_error(span,"expected std.file.IOFailure","E4003");
        if(f.name=="error")return Value(io->data->error);
        if(f.name=="transferred")return io->data->transferred?Value(*io->data->transferred):Value{};
        return io->data->partial?Value(*io->data->partial):Value{};
    }
    if(f.name=="open"){
        auto path=hua::string_value(args[0].data);auto m=mode(hua::string_value(args[1].data));
        if(!m)return failed(failure("bad_mode","mode must be r, w, a, r+, w+ or a+"),f.name,"std.file.File");
        if(path.empty()||path.size()>32767||path.find('\0')!=std::string::npos||!text_utf8_error(path).empty())
            return failed(failure("invalid_argument","path requires 1..32767 UTF-8 bytes without NUL"),f.name,"std.file.File");
#ifdef _WIN32
        if((path.size()>=2&&path[1]==':'&&(path.size()<3||(path[2]!='/'&&path[2]!='\\')))||((path[0]=='/'||path[0]=='\\')&&(path.size()<2||path[1]!=path[0])))
            return failed(failure("invalid_argument","drive-relative and root-relative Windows paths are unsupported"),f.name,"std.file.File");
#endif
        if(!c)return failed(failure("wrong_owner","File needs an execution scope"),f.name,"std.file.File");
        prepare_file_scope(*c);auto native=std::make_shared<FileNativeState>(*m);
        auto h=admit(*c->files,native);if(!h)return failed(resource_failure(h.error),f.name,"std.file.File");
        try{
            auto p=std::filesystem::path(std::u8string(path.begin(),path.end()));if(!p.is_absolute()&&!c->working_directory.empty())p=c->working_directory/p;
            bool overlapped=false;
#ifdef _WIN32
            static const bool use_iocp=[] {
                const auto* backend=std::getenv("HUA_TASK_FILE_BACKEND");
                return !backend||std::string_view(backend)!="blocking";
            }();
            overlapped=use_iocp&&c->in_task&&c->resumable;
#endif
            auto opened=native_open(p,*m,overlapped);
            native->backend=std::move(opened.backend);
            if(opened.error){c->files->protocol.close(*h.value,c->task_id);c->files->pump();return failed(opened.error,f.name,"std.file.File");}
            return publish(c->files,*h.value,std::move(native));
        }catch(...){c->files->protocol.close(*h.value,c->task_id);c->files->pump();throw;}
    }
    auto val=hua::get_if<FileValue>(&args[0].data);auto file=val?val->data:nullptr;
    const bool bulk=f.since==29;
    bool io=f.name=="read"||f.name=="write"||bulk;
    auto bad=[&](FileHostFailure e){return io?io_failed(e,f.name,I{0}):failed(e,f.name,type_arguments(f.result,"Result")[0]);};
    if(auto e=check(file,c,f.name!="state"&&f.name!="close_error"&&f.name!="close"))return bad(e);
    auto& native=*file->native;
    if(f.name=="close"){
        auto ticket=file->scope->protocol.close(file->handle,c->task_id);
        if(!ticket)return bad(resource_failure(ticket.error));
        file->scope->pump();auto observed=ticket.value->observe();
        if(!observed)return bad(failure("busy","File close is awaiting a native operation"));
        if(observed->error){
            std::lock_guard close_lock(native.mutex);
            return bad(native.first_close_error?native.first_close_error:failure("poisoned","physical release unknown"));
        }
        return void_ok();
    }
    std::unique_lock lock(file->native->mutex);
    if(f.name=="state"){
        auto s=file->handle.snapshot().state;
        return result(Value(std::string(s==R::Lifecycle::open?"open":s==R::Lifecycle::closing?"closing":s==R::Lifecycle::closed?"closed":"poisoned")),true,"string");
    }
    if(f.name=="close_error")return result(native.first_close_error?Value(error(native.first_close_error,"close")):Value{},true,"std.error.Value?");
    if(bulk){
        auto operation=bulk_operation(f.name,*file,native,args,span,c,lock,false);
        operation.handle.resume();
        return operation.take();
    }
    if(io){
        auto operation=transfer_file(f,args,span,c,file,std::move(lock),false);
        operation.handle.resume();
        return operation.take();
    }
    // All position/flush operations also reserve an operation and prevent close until native return.
    auto admitted=file->scope->protocol.reserve(file->handle,c->task_id);
    if(!admitted)return bad(resource_failure(admitted.error));
    OperationGuard operation{file->scope->protocol,*admitted.value};
    if(auto e=file->scope->protocol.submit(operation.event))return bad(resource_failure(e));
    operation.submitted=true;
    if(f.name=="flush"){
        if(!native.mode.write)return bad(failure("permission_denied","flush requires a writable File mode"));
        if(auto e=native.backend->flush())return bad(e);return void_ok();
    }
    if(f.name=="tell"){auto pos=native.backend->position();if(pos.error)return bad(pos.error);return result(Value(pos.value-static_cast<I>(native.buffered())),true,"int");}
    if(f.name=="seek"){
        auto offset=as_int(args[1],span);auto whence=hua::string_value(args[2].data);FileHostPosition base{};
        if(whence=="current"){base=native.backend->position();base.value-=static_cast<I>(native.buffered());}else if(whence=="end")base=native.backend->size();
        else if(whence!="start")return bad(failure("invalid_argument","whence must be start, current or end"));
        if(base.error)return bad(base.error);
        if(base.value<0||(offset>0&&base.value>std::numeric_limits<I>::max()-offset)||(offset<0&&offset< -base.value))
            return bad(failure("invalid_argument","seek target is negative or overflows int64"));
        auto target=base.value+offset;if(auto e=native.backend->seek(target))return bad(e);native.drop_pending();return result(Value(target),true,"int");
    }
    runtime_error(span,"unknown File operation","E6001");
}
}
