#include "hua/performance.hpp"
#include "hua/file.hpp"
#include "hua/tasks.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <limits>
#include <mutex>
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
    return Value(ResultValue{ok,managed<Value>(std::move(payload)),std::move(type),std::move(err),true,false});
}
Value void_ok(){return Value(ResultValue{true,{},"void","std.error.Value",true,false});}
Value failed(FileHostFailure f,std::string_view op,std::string type){return result(Value(error(f,op)),false,std::move(type));}
Value io_failed(FileHostFailure f,std::string_view op,std::optional<I> count={},std::optional<BytesValue> partial={}){
    return result(Value(IOFailureValue{managed<const IOFailureData>(IOFailureData{error(f,op),count,std::move(partial)})}),
                  false,op=="read"?"Bytes?":"int","std.file.IOFailure");
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
class NativeFile final:public FileBackend {
    HANDLE handle_;bool append_;
public:
    explicit NativeFile(HANDLE h,bool append):handle_(h),append_(append){}
    void attach(HANDLE h){handle_=h;}
    ~NativeFile()override{if(handle_!=INVALID_HANDLE_VALUE)close();}
    FileHostIO read(std::span<char> b)override{
        DWORD n{};if(ReadFile(handle_,b.data(),static_cast<DWORD>(b.size()),&n,nullptr))return {static_cast<I>(n),{}};
        auto e=GetLastError();if(e==ERROR_HANDLE_EOF)return {I{0},{}};return {{},host_failure(e)};
    }
    FileHostIO write(std::span<const char> b)override{
        DWORD n{};OVERLAPPED end{};end.Offset=end.OffsetHigh=0xffffffff;
        if(WriteFile(handle_,b.data(),static_cast<DWORD>(b.size()),&n,append_?&end:nullptr))return {static_cast<I>(n),{}};
        auto e=GetLastError();return {{},host_failure(e)};
    }
    FileHostPosition position()override{
        LARGE_INTEGER zero{},pos{};if(SetFilePointerEx(handle_,zero,&pos,FILE_CURRENT))return {pos.QuadPart,{}};
        return {0,host_failure(GetLastError())};
    }
    FileHostPosition size()override{
        LARGE_INTEGER n{};if(GetFileSizeEx(handle_,&n))return {n.QuadPart,{}};
        return {0,host_failure(GetLastError())};
    }
    FileHostFailure seek(I n)override{
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
Opened native_open(const std::filesystem::path& path,Mode m){
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
    auto backend=std::make_unique<NativeFile>(INVALID_HANDLE_VALUE,m.append);
    auto h=CreateFileW(text.c_str(),(m.read?GENERIC_READ:0)|(m.write?GENERIC_WRITE:0),
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,m.create?OPEN_ALWAYS:OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h==INVALID_HANDLE_VALUE)return {{},host_failure(GetLastError())};
    backend->attach(h);
    BY_HANDLE_FILE_INFORMATION info{};
    if(GetFileType(h)!=FILE_TYPE_DISK)return {std::move(backend),failure("not_regular","only regular disk files are supported")};
    if(!GetFileInformationByHandle(h,&info))return {std::move(backend),host_failure(GetLastError())};
    if(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)return {std::move(backend),failure("not_regular","directories cannot be opened as File")};
    if(m.truncate&&!SetEndOfFile(h))return {std::move(backend),host_failure(GetLastError())};
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
    explicit FileNativeState(Mode m):mode(m){}
};
FileScope::FileScope(R::TaskId id,std::shared_ptr<R::QuotaLedger> r,std::optional<R::Cost> custom)
    :owner(id),root(r?r:R::QuotaLedger::root(limits(true))),ledger(root->child(custom.value_or(limits(false)))),protocol(ledger){}
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
}
Value FileScope::adopt(std::unique_ptr<FileBackend> backend,std::string_view text){
    auto m=mode(text);if(!m||!backend)return failed(failure("bad_mode","invalid native File adoption"),"open","std.file.File");
    auto native=std::make_shared<FileNativeState>(*m);native->backend=std::move(backend);
    auto h=admit(*this,native);if(!h)return failed(resource_failure(h.error),"open","std.file.File");
    return publish(shared_from_this(),*h.value,std::move(native));
}
Value invoke_file_standard(const StandardFunction& f,const std::vector<Value>& args,const SourceSpan& span,RuntimeContext* c){
    if(f.name=="error"||f.name=="transferred"||f.name=="partial"){
        auto io=std::get_if<IOFailureValue>(&args[0].data);if(!io||!io->data)runtime_error(span,"expected std.file.IOFailure","E4003");
        if(f.name=="error")return Value(io->data->error);
        if(f.name=="transferred")return io->data->transferred?Value(*io->data->transferred):Value{};
        return io->data->partial?Value(*io->data->partial):Value{};
    }
    if(f.name=="open"){
        auto path=std::get<std::string>(args[0].data);auto m=mode(std::get<std::string>(args[1].data));
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
            auto opened=native_open(p,*m);
            native->backend=std::move(opened.backend);
            if(opened.error){c->files->protocol.close(*h.value,c->task_id);c->files->pump();return failed(opened.error,f.name,"std.file.File");}
            return publish(c->files,*h.value,std::move(native));
        }catch(...){c->files->protocol.close(*h.value,c->task_id);c->files->pump();throw;}
    }
    auto val=std::get_if<FileValue>(&args[0].data);auto file=val?val->data:nullptr;
    bool io=f.name=="read"||f.name=="write";
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
    if(io){
        I n=f.name=="read"?as_int(args[1],span):static_cast<I>(std::get<BytesValue>(args[1].data).data->size());
        if(n<0||n>static_cast<I>(max_io))return bad(failure("limit","one File operation requires 0..1048576 bytes"));
        if(f.name=="read"?!native.mode.read:!native.mode.write)return bad(failure("permission_denied","File mode does not permit this operation"));
        if(!n)return result(f.name=="read"?Value(BytesValue{managed<const std::string>()}):Value(I{0}),true,f.name=="read"?"Bytes?":"int","std.file.IOFailure");
        std::shared_ptr<QuotaBytes> storage;std::shared_ptr<void> keepalive;
        if(f.name=="read"){
            auto permit=file->scope->ledger->reserve(R::cost(R::Metric::storage_bytes,static_cast<std::uint64_t>(n)));
            if(!permit)return bad(resource_failure(permit.error));
            try{storage=std::make_shared<QuotaBytes>(std::move(*permit.value),static_cast<std::size_t>(n));}
            catch(const std::bad_alloc&){return bad(failure("allocation","read allocation failed before native admission"));}
            keepalive=storage;
        }else keepalive=std::const_pointer_cast<std::string>(std::get<BytesValue>(args[1].data).data);
        auto admitted=file->scope->protocol.reserve(file->handle,c->task_id,{},R::Access::read,0,static_cast<std::size_t>(n),keepalive);
        if(!admitted)return bad(resource_failure(admitted.error));
        OperationGuard operation{file->scope->protocol,*admitted.value};
        if(auto e=file->scope->protocol.submit(operation.event))return bad(resource_failure(e));
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
        operation.submitted=true;FileHostIO observed;
        try{observed=f.name=="read"?native.backend->read(std::span<char>(storage->data.data(),static_cast<std::size_t>(n))):
            native.backend->write(std::span<const char>(*std::get<BytesValue>(args[1].data).data));}
        catch(...){observed={{},failure("io","host operation threw; progress unknown")};}
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
            issue->transferred=observed.transferred;issue->partial=bytes;return unsuccessful;
        }
        *std::get<ResultValue>(success.data).payload=f.name=="read"?(bytes?Value(*bytes):Value{}):Value(*observed.transferred);
        return success;
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
    if(f.name=="tell"){auto pos=native.backend->position();if(pos.error)return bad(pos.error);return result(Value(pos.value),true,"int");}
    if(f.name=="seek"){
        auto offset=as_int(args[1],span);auto whence=std::get<std::string>(args[2].data);FileHostPosition base{};
        if(whence=="current")base=native.backend->position();else if(whence=="end")base=native.backend->size();
        else if(whence!="start")return bad(failure("invalid_argument","whence must be start, current or end"));
        if(base.error)return bad(base.error);
        if(base.value<0||(offset>0&&base.value>std::numeric_limits<I>::max()-offset)||(offset<0&&offset< -base.value))
            return bad(failure("invalid_argument","seek target is negative or overflows int64"));
        auto target=base.value+offset;if(auto e=native.backend->seek(target))return bad(e);return result(Value(target),true,"int");
    }
    runtime_error(span,"unknown File operation","E6001");
}
}
