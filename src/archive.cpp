#include "hua/archive.hpp"
#include "hua/stdlib.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <deque>
#include <fstream>
#include <limits>
#include <random>
#include <set>
#include <unordered_set>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
namespace hua {
namespace {
constexpr std::size_t max_file=128*1024*1024,max_text=16*1024*1024,max_instructions=1000000;
[[noreturn]] void bad(const SourceSpan& s,std::string msg){throw Diagnostic("E6002",s,"invalid .huab: "+msg);}
std::string utf8(const std::filesystem::path& p){auto s=p.u8string();return {s.begin(),s.end()};}
void valid_utf8(const std::string& s,const SourceSpan& site) {
    for(std::size_t i=0;i<s.size();) {
        auto b=static_cast<unsigned char>(s[i++]);if(b<128)continue;
        unsigned n;std::uint32_t v,min;
        if(b>=194&&b<=223){n=1;v=b&31;min=128;}else if(b>=224&&b<=239){n=2;v=b&15;min=2048;}else if(b>=240&&b<=244){n=3;v=b&7;min=65536;}else bad(site,"invalid UTF-8 string");
        if(n>s.size()-i)bad(site,"truncated UTF-8 string");
        while(n--){auto c=static_cast<unsigned char>(s[i++]);if((c&192)!=128)bad(site,"invalid UTF-8 continuation");v=(v<<6)|(c&63);}
        if(v<min||v>0x10ffff||(v>=0xd800&&v<=0xdfff))bad(site,"invalid UTF-8 code point");
    }
}
void small_name(const std::string& s,const SourceSpan& site,bool empty=false){if((!empty&&s.empty())||s.size()>4096||s.find('\0')!=std::string::npos)bad(site,"invalid name/type length");}
void type_string(const std::string& s,const SourceSpan& site){small_name(s,site,true);if(std::count_if(s.begin(),s.end(),[](char c){return c=='?'||c=='['||c=='<';})>192)bad(site,"type nesting limit exceeded");}
std::uint32_t crc32(std::string_view s) {
    std::uint32_t c=0xffffffff;for(unsigned char b:s){c^=b;for(unsigned k=0;k<8;++k)c=(c>>1)^(0xedb88320u&static_cast<std::uint32_t>(-static_cast<std::int32_t>(c&1)));}return ~c;
}
struct Writer {
    std::string bytes;
    void u8(std::uint8_t n){bytes+=static_cast<char>(n);}
    void u32(std::uint32_t n){for(unsigned i=0;i<4;++i)u8(static_cast<std::uint8_t>(n>>(8*i)));}
    void u64(std::uint64_t n){for(unsigned i=0;i<8;++i)u8(static_cast<std::uint8_t>(n>>(8*i)));}
    void count(std::size_t n){if(n>std::numeric_limits<std::uint32_t>::max())bad({},"count exceeds format");u32(static_cast<std::uint32_t>(n));}
    void str(const std::string& s,bool binary=false){if(!binary)valid_utf8(s,{});if(s.size()>max_text)bad({},"string exceeds 16 MiB");count(s.size());bytes+=s;}
};
struct Reader {
    std::string_view bytes;std::size_t offset{};SourceSpan site;std::uint32_t format{4};
    std::uint8_t u8(){if(offset==bytes.size())bad(site,"truncated data");return static_cast<std::uint8_t>(bytes[offset++]);}
    bool boolean(){auto n=u8();if(n>1)bad(site,"invalid boolean");return n!=0;}
    std::uint32_t u32(){std::uint32_t n=0;for(unsigned i=0;i<4;++i)n|=std::uint32_t(u8())<<(i*8);return n;}
    std::uint64_t u64(){std::uint64_t n=0;for(unsigned i=0;i<8;++i)n|=std::uint64_t(u8())<<(i*8);return n;}
    std::uint32_t count(std::size_t max){auto n=u32();if(n>max || n>bytes.size()-offset)bad(site,"oversized count");return n;}
    std::string str(bool binary=false){auto n=count(max_text);if(n>bytes.size()-offset)bad(site,"truncated string");std::string s(bytes.substr(offset,n));offset+=n;if(!binary)valid_utf8(s,site);return s;}
};
struct Sources {
    std::vector<const Source*> sources;
    std::unordered_map<std::string,std::uint32_t> ids;
    explicit Sources(const std::vector<const Source*>& src):sources(src){for(std::size_t i=0;i<src.size();++i)if(!ids.emplace(src[i]->filename(),static_cast<std::uint32_t>(i)).second)bad({},"duplicate source id");}
    void write(Writer& w,const SourceSpan& s) const {
        auto id=ids.find(s.file_id);if(id==ids.end())bad(s,"unknown source id");auto origin=sources[id->second];
        if(s.start_offset>s.end_offset||s.end_offset>origin->text().size())bad(s,"source offsets out of bounds");
        auto expected=origin->span(s.start_offset,s.end_offset);if(s.line!=expected.line||s.column!=expected.column)bad(s,"source coordinates mismatch");
        w.u32(id->second);w.count(s.start_offset);w.count(s.end_offset);w.count(s.line);w.count(s.column);
    }
    SourceSpan read(Reader& r) const {
        auto id=r.u32();auto start=r.u32(),end=r.u32(),line=r.u32(),column=r.u32();
        if(id>=sources.size()||start>end||end>sources[id]->text().size())bad(r.site,"invalid source span");
        auto s=sources[id]->span(start,end);if(s.line!=line||s.column!=column)bad(r.site,"source coordinates mismatch");return s;
    }
};
void value(Writer& w,const Value& v){auto kind=v.data.index();if(kind>4&&kind!=15)bad({},"non-scalar constant");w.u8(static_cast<std::uint8_t>(kind));switch(kind){case 1:w.u8(std::get<bool>(v.data));break;case 2:w.u64(std::bit_cast<std::uint64_t>(std::get<std::int64_t>(v.data)));break;case 3:w.u64(std::bit_cast<std::uint64_t>(std::get<double>(v.data)));break;case 4:w.str(std::get<std::string>(v.data));break;case 15:{auto n=std::get<NumericValue>(v.data);w.str(n.type);w.u8(static_cast<std::uint8_t>(n.number.index()));std::visit([&](auto x){w.u64(std::bit_cast<std::uint64_t>(x));},n.number);break;}default:break;}}
Value value(Reader& r){switch(r.u8()){case 0:return {};case 1:return Value(r.boolean());case 2:return Value(std::bit_cast<std::int64_t>(r.u64()));case 3:{auto d=std::bit_cast<double>(r.u64());if(!std::isfinite(d))bad(r.site,"non-finite constant");return Value(d);}case 4:return Value(r.str());case 15:{if(r.format<5)bad(r.site,"unsupported numeric constant");auto type=r.str();auto spec=numeric_spec(type);if(!spec||type=="int"||type=="float"||type=="@integer")bad(r.site,"unknown numeric constant type");auto kind=r.u8();auto bits=r.u64();NumericValue n{type,std::int64_t{}};if(kind==0&&spec->category=='i')n.number=std::bit_cast<std::int64_t>(bits);else if(kind==1&&spec->category=='u')n.number=bits;else if(kind==2&&spec->category=='f')n.number=std::bit_cast<double>(bits);else bad(r.site,"numeric constant payload mismatch");try{auto checked=convert_numeric(Value(n),type,r.site,true);if(auto real=std::get_if<double>(&n.number);real&&std::get<double>(std::get<NumericValue>(checked.data).number)!=*real)bad(r.site,"non-canonical f32 constant");return checked;}catch(const Diagnostic&){bad(r.site,"numeric constant out of range");}}default:bad(r.site,"invalid constant kind");}}
void code(Writer& w,const Code& c,const Sources& sources) {
    small_name(c.name,{});w.str(c.name);w.count(c.instructions.size());
    for(const auto& op:c.instructions) {
        small_name(op.text,op.span,true);for(const auto& name:op.names)small_name(name,op.span);
        if(op.names.size()>4096||op.contextual.size()>4096)bad(op.span,"instruction metadata limit exceeded");
        w.u8(static_cast<std::uint8_t>(op.op));sources.write(w,op.span);w.str(op.text);w.str(op.type);w.count(op.argument);w.count(op.target);w.u8(op.flag);value(w,op.constant);
        w.count(op.names.size());for(const auto& n:op.names)w.str(n);w.count(op.contextual.size());for(bool b:op.contextual)w.u8(b);
    }
}
Code code(Reader& r,const Sources& sources,std::size_t& total) {
    Code c;c.name=r.str();small_name(c.name,r.site);auto count=r.count(250000);total+=count;if(total>max_instructions)bad(r.site,"instruction count limit exceeded");c.instructions.reserve(count);
    for(std::size_t j=0;j<count;++j) {
        auto kind=r.u8();if(kind>static_cast<unsigned>(r.format==1?Op::ExternalInit:r.format<5?Op::CheckIndex:Op::Defer))bad(r.site,"unknown opcode");Instruction op{};op.op=static_cast<Op>(kind);op.span=sources.read(r);
        op.text=r.str();small_name(op.text,r.site,true);
        auto intrinsic=op.text.starts_with("$core$")?op.text.substr(6):op.text;if(op.op==Op::Load&&intrinsic.starts_with("$std$")&&(!standard_function(intrinsic)||r.format<standard_function(intrinsic)->since))bad(r.site,"unsupported standard library reference");if(op.op==Op::Load&&r.format<6&&(intrinsic=="task_start"||intrinsic=="task_await"||intrinsic=="task_group_begin"||intrinsic=="task_group_end"||intrinsic=="parallel_map"||intrinsic=="simd_check"))bad(r.site,"unsupported concurrency intrinsic");op.type=r.str();type_string(op.type,r.site);op.argument=r.u32();op.target=r.u32();op.flag=r.boolean();op.constant=value(r);
        auto names=r.count(4096);for(std::size_t i=0;i<names;++i){auto n=r.str();small_name(n,r.site);op.names.push_back(std::move(n));}
        auto flags=r.count(4096);for(std::size_t i=0;i<flags;++i)op.contextual.push_back(r.boolean());c.instructions.push_back(std::move(op));
    }
    return c;
}
unsigned required_format(const Bytecode& bytecode,const SemanticModel& model){
    unsigned format=6;
    auto type=[&](const std::string& value){if(value.find("std.error.Value")!=std::string::npos||value.find("Result<void,")!=std::string::npos)format=7;};
    std::function<void(const Node&)> declaration=[&](const Node& n){if(n.kind==NodeKind::TypeName||n.kind==NodeKind::GenericType)type(type_name(n));for(const auto& c:n.children)declaration(*c);};
    for(const auto& [_,n]:model.functions)declaration(*n);
    for(const auto& [_,n]:model.structures)declaration(*n);
    auto code=[&](const Code& c){for(const auto& op:c.instructions){type(op.type);auto name=op.text.starts_with("$core$")?op.text.substr(6):op.text;if(auto f=standard_function(name))format=std::max(format,f->since);}};
    code(bytecode.initializer);for(const auto& [_,c]:bytecode.functions)code(c);
    return format;
}

struct State {std::vector<bool> stack;std::size_t scopes{},iterators{};bool operator==(const State&)const=default;};
void verify(const Code& c,const SemanticModel& model) {
    if(c.instructions.empty() || c.instructions.size()>250000)bad({},"empty/oversized code chunk");
    // Validate unreachable instructions too, then walk all reachable abstract stack/scope states.
    const std::unordered_set<std::string> unary={"+","-","!","~"},binary={"+","-","*","/","//","%","**","&","|","^","<<",">>","==","!=","<",">","<=",">=","&&","||"},store={"=","+=","-=","*=","/=","//=","%=","**="};
    for(const auto& op:c.instructions) {
        if(static_cast<unsigned>(op.op)>static_cast<unsigned>(Op::Defer))bad(op.span,"unknown opcode");
        if(op.argument>4096)bad(op.span,"instruction argument limit exceeded");type_string(op.type,op.span);
        if(auto precise=std::get_if<NumericValue>(&op.constant.data)){auto spec=numeric_spec(precise->type);if(!spec||precise->type=="@integer"||precise->type=="int"||precise->type=="float")bad(op.span,"invalid numeric constant type");try{convert_numeric(op.constant,precise->type,op.span,true);}catch(const Diagnostic&){bad(op.span,"numeric constant out of range");}}
        if(op.constant.data.index()>4&&op.constant.data.index()!=15)bad(op.span,"non-scalar constant");if(auto f=std::get_if<double>(&op.constant.data);f&&!std::isfinite(*f))bad(op.span,"non-finite constant");
        if((op.op==Op::Jump||op.op==Op::JumpFalse||op.op==Op::JumpTrue||op.op==Op::IterNext)&&op.target>=c.instructions.size())bad(op.span,"jump target out of bounds");
        if(op.op==Op::IterNext && (op.names.empty()||op.names.size()>2))bad(op.span,"invalid loop bindings");
        if(op.op==Op::Slice && op.contextual.size()!=2)bad(op.span,"invalid slice bounds metadata");
        if((op.op==Op::Call||op.op==Op::Defer) && op.contextual.size()!=op.argument)bad(op.span,"invalid call argument metadata");
        if(op.op==Op::Bind && op.contextual.size()!=1)bad(op.span,"invalid binding metadata");
        if(op.op==Op::MakeMulti&&(op.argument<2||op.contextual.size()!=op.argument))bad(op.span,"invalid multiple-value metadata");
        if(op.op==Op::BindMulti){auto types=type_arguments(op.type,"multi");if(op.argument<2||op.names.size()!=op.argument||types.size()!=op.argument)bad(op.span,"invalid multiple binding metadata");std::set<std::string> seen;for(const auto& n:op.names)if(n!="_"&&!seen.insert(n).second)bad(op.span,"duplicate multiple binding");}
        if(op.op==Op::StoreMulti&&op.argument<2)bad(op.span,"invalid multiple assignment metadata");
        if(op.op==Op::MakeMap){auto ts=type_arguments(op.type,"map");if(ts.size()!=2||(ts[0]!="bool"&&ts[0]!="int"&&ts[0]!="string")||ts[1].empty()||ts[1]=="void"||ts[1].starts_with("Result<"))bad(op.span,"invalid map type metadata");}
        if(op.op==Op::Propagate){auto f=model.functions.find(c.name);std::string type;if(f!=model.functions.end())for(const auto& t:f->second->children)if(t->kind==NodeKind::ReturnTypes&&t->children.size()==1)type=type_name(*t->children[0]);if(type_arguments(type,"Result").size()!=2)bad(op.span,"propagation requires a Result-returning function");}
        if(op.op==Op::Unary && !unary.contains(op.text))bad(op.span,"unknown unary operator");
        if(op.op==Op::Binary && !binary.contains(op.text))bad(op.span,"unknown binary operator");
        if(op.op==Op::Store && !store.contains(op.text))bad(op.span,"unknown assignment operator");
        if(op.op==Op::MakeStruct&&!model.structures.contains(op.text))bad(op.span,"unknown struct metadata");
        if(op.op==Op::ExternalInit&&(!model.external||!model.external->module_exists(op.text)))bad(op.span,"unknown external initializer");
    }
    std::vector<std::optional<State>> states(c.instructions.size());std::deque<std::size_t> work;std::size_t slots=0;
    auto merge=[&](std::size_t pc,const State& s) {
        if(pc>=c.instructions.size())bad(c.instructions.back().span,"code falls off its end");
        if(s.stack.size()>4096||s.scopes>384||s.iterators>192)bad(c.instructions[pc].span,"VM state depth limit exceeded");
        if(states[pc]){if(*states[pc]!=s)bad(c.instructions[pc].span,"incompatible stack/scope states at branch merge");}
        else {slots+=s.stack.size()+1;if(slots>16000000)bad(c.instructions[pc].span,"verification state budget exceeded");states[pc]=s;work.push_back(pc);}
    };
    merge(0,{});
    while(!work.empty()) {
        auto pc=work.front();work.pop_front();auto s=*states[pc];const auto& op=c.instructions[pc];bool fall=true;
        auto pop=[&](bool location=false){if(s.stack.empty()||s.stack.back()!=location)bad(op.span,"operand stack underflow/type mismatch");s.stack.pop_back();};
        auto push=[&](bool location=false){s.stack.push_back(location);};
        switch(op.op) {
        case Op::Constant:case Op::Load:case Op::MakeArray:case Op::MakeStruct:case Op::MakeMap:push();break;
        case Op::Bind:case Op::Pop:case Op::BindMulti:pop();break;
        case Op::Unary:case Op::Member:case Op::Propagate:pop();push();break;
        case Op::Binary:case Op::Index:pop();pop();push();break;
        case Op::Jump:merge(op.target,s);fall=false;break;
        case Op::JumpFalse:case Op::JumpTrue:pop();merge(op.target,s);break;
        case Op::EnterScope:++s.scopes;break;
        case Op::LeaveScope:if(!s.scopes)bad(op.span,"scope underflow");--s.scopes;break;
        case Op::Unwind:if(op.argument>s.scopes)bad(op.span,"scope unwind underflow");s.scopes-=op.argument;break;
        case Op::LocateName:push(true);break;
        case Op::LocateField:pop();push(true);break;
        case Op::LocateIndex:pop();pop();push(true);break;
        case Op::Store:pop();pop(true);push();break;
        case Op::Slice:pop();pop();pop();push();break;
        case Op::CheckSlice:case Op::CheckIndex:if(s.stack.empty()||s.stack.back())bad(op.span,"slice target stack mismatch");break;
        case Op::ArrayAppend:case Op::InitField:pop();if(s.stack.empty()||s.stack.back())bad(op.span,"builder stack mismatch");break;
        case Op::MapInsert:pop();pop();if(s.stack.empty()||s.stack.back())bad(op.span,"map builder stack mismatch");break;
        case Op::MakeMulti:for(std::size_t j=0;j<op.argument;++j)pop();push();break;
        case Op::StoreMulti:pop();for(std::size_t j=0;j<op.argument;++j)pop(true);break;
        case Op::Closure:if(!model.functions.contains(op.text))bad(op.span,"unknown closure function");push();break;
        case Op::Defer:for(std::size_t j=0;j<op.argument;++j)pop();pop();break;
        case Op::Call:for(std::size_t j=0;j<op.argument;++j)pop();pop();push();break;
        case Op::Return:pop();if(!s.stack.empty())bad(op.span,"return operand stack mismatch");fall=false;break;
        case Op::RangeInit:pop();pop();pop();++s.iterators;break;
        case Op::SliceInit:pop();++s.iterators;break;
        case Op::IterNext:if(!s.iterators)bad(op.span,"iterator underflow");merge(op.target,s);++s.scopes;break;
        case Op::IterEnd:if(!s.iterators)bad(op.span,"iterator end underflow");--s.iterators;break;
        case Op::Fail:fall=false;break;
        case Op::ExternalInit:break;
        }
        if(fall)merge(pc+1,s);
    }
}
}
const Source* BytecodeImage::source(const std::string& id) const {for(const auto& s:sources)if(s->filename()==id)return s.get();return nullptr;}
void verify_bytecode(const Bytecode& b,const SemanticModel& model) {
    verify(b.initializer,model);std::size_t total=b.initializer.instructions.size();
    for(const auto& [fn,c]:b.functions){if(!fn||declared_name(*fn)!=c.name||!model.functions.contains(c.name)||model.functions.at(c.name)!=fn)bad(c.instructions.empty()?SourceSpan{}:c.instructions[0].span,"function metadata mismatch");verify(c,model);total+=c.instructions.size();}
    if(total>max_instructions)bad({},"instruction count limit exceeded");
    for(const auto& [name,fn]:model.functions) {
        if(fn->text.find(" external")!=std::string::npos){if(!model.external||!model.external->contains(name))bad(fn->span,"unregistered external function");}
        else if(!b.functions.contains(fn))bad(fn->span,"missing function code");
    }
}
void write_huab(const std::filesystem::path& path,const Bytecode& b,const SemanticModel& model,const std::vector<const Source*>& sources) {
    verify_bytecode(b,model);
    if(sources.empty()||sources.size()>128||model.structures.size()>16384||model.functions.size()>16384)bad({},"metadata table limit exceeded");
    std::size_t text_bytes=0,metadata=0;
    for(auto s:sources){if(!s||s->filename().empty()||s->filename().size()>16384||s->filename().find('\0')!=std::string::npos)bad({},"invalid source id");text_bytes+=s->text().size();}
    if(text_bytes>64*1024*1024)bad({},"source table exceeds 64 MiB");
    for(const auto& [name,n]:model.structures){small_name(name,n->span);if(n->children.size()>4096)bad(n->span,"field count limit exceeded");metadata+=n->children.size();for(const auto& f:n->children){small_name(f->text,f->span);type_string(type_name(*f->children[0]),f->span);}}
    for(const auto& [name,n]:model.functions){small_name(name,n->span);std::size_t ps=0,rs=0;for(const auto& c:n->children){if(c->kind==NodeKind::Parameter){++ps;small_name(declared_name(*c),c->span);type_string(c->children.empty()?"":type_name(*c->children[0]),c->span);}if(c->kind==NodeKind::ReturnTypes){rs+=c->children.size();for(const auto& t:c->children)type_string(type_name(*t),t->span);}}if(ps>4096||rs>4096)bad(n->span,"signature count limit exceeded");metadata+=ps+rs;}
    if(metadata>1000000)bad({},"metadata count budget exceeded");
    Sources src(sources);Writer w;w.count(sources.size());for(auto s:sources){w.str(s->filename());w.str(s->text());}
    std::vector<std::pair<std::string,const Node*>> structures(model.structures.begin(),model.structures.end()),functions(model.functions.begin(),model.functions.end());
    std::sort(structures.begin(),structures.end());std::sort(functions.begin(),functions.end());
    w.count(structures.size());for(const auto& [name,n]:structures) {
        w.str(name+(n->text.find(" interface")!=std::string::npos?" interface":n->text.find(" enum")!=std::string::npos?" enum":""));src.write(w,n->span);w.u8(n->text.find(" pub")!=std::string::npos);w.count(n->children.size());
        for(const auto& f:n->children){w.str(f->text);w.str(type_name(*f->children[0]));src.write(w,f->span);}
    }
    w.count(functions.size());for(const auto& [name,n]:functions) {
        w.str(name+(model.nested.contains(n)?" nested":"")+(n->text.find(" abstract")!=std::string::npos?" abstract":""));src.write(w,n->span);w.u8(n->text.find(" pub")!=std::string::npos);w.u8(n->text.find(" unsafe")!=std::string::npos);w.u8(n->text.find(" external")!=std::string::npos);
        w.u8(model.mutating.contains(name)&&model.mutating.at(name));std::vector<const Node*> ps,rs;
        for(const auto& c:n->children){if(c->kind==NodeKind::Parameter)ps.push_back(c.get());if(c->kind==NodeKind::ReturnTypes)for(const auto& t:c->children)rs.push_back(t.get());}
        w.count(ps.size());for(auto p:ps){w.str(declared_name(*p));w.str(p->children.empty()?"":type_name(*p->children[0]));w.u8(p->text.find(" mut")!=std::string::npos||(!p->children.empty()&&p->children[0]->kind==NodeKind::MutableType));src.write(w,p->span);}
        w.count(rs.size());for(auto t:rs)w.str(type_name(*t));
    }
    auto ex=model.external?model.external->modules():std::vector<ExternalModule>{};w.count(ex.size());
    for(const auto& m:ex) {
        w.str(m.id);w.u8(static_cast<std::uint8_t>(m.kind));w.str(m.artifact);src.write(w,m.span);w.str(m.wasm,true);w.count(m.exports.size());
        for(const auto& e:m.exports){w.str(e.name);w.str(e.linked_name);w.str(e.result);w.count(e.parameters.size());for(const auto& t:e.parameters)w.str(t);}
    }
    code(w,b.initializer,src);w.count(b.functions.size());for(const auto& [name,n]:functions)if(auto c=b.functions.find(n);c!=b.functions.end())code(w,c->second,src);
    if(w.bytes.size()>max_file-32)bad({},"payload exceeds 128 MiB");Writer header;header.bytes="HUAB\r\n\x1a\n";header.u32(required_format(b,model));header.u32(1);header.u64(w.bytes.size());header.u32(crc32(w.bytes));header.u32(0);
    auto tmp=path;std::random_device random;tmp+=".tmp-"+std::to_string(random())+"-"+std::to_string(random());
    try {
        std::ofstream out(tmp,std::ios::binary|std::ios::trunc);if(!out)throw std::runtime_error("cannot create .huab output");out.write(header.bytes.data(),header.bytes.size());out.write(w.bytes.data(),w.bytes.size());out.flush();if(!out)throw std::runtime_error("failed to write complete .huab output");out.close();
#ifdef _WIN32
        if(!MoveFileExW(tmp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::runtime_error("cannot replace .huab output");
#else
        std::filesystem::rename(tmp,path);
#endif
    }catch(...){std::error_code ec;std::filesystem::remove(tmp,ec);throw;}
}
BytecodeImage read_huab(const std::filesystem::path& path) {
    std::ifstream in(path,std::ios::binary|std::ios::ate);SourceSpan site{utf8(path),0,0,1,1};if(!in)bad(site,"cannot read file");auto size=in.tellg();if(size<32||size>static_cast<std::streamoff>(max_file))bad(site,"file size is outside 32 bytes..128 MiB");
    std::string data(static_cast<std::size_t>(size),'\0');in.seekg(0);if(!in.read(data.data(),size))bad(site,"truncated file");
    if(data.substr(0,8)!="HUAB\r\n\x1a\n")bad(site,"magic header mismatch");Reader header{std::string_view(data).substr(8,24),0,site};
    auto format=header.u32();if(format!=1&&format!=2&&format!=3&&format!=4&&format!=5&&format!=6&&format!=7)bad(site,"unsupported bytecode format version");if(header.u32()!=1)bad(site,"unsupported native ABI version");auto length=header.u64();auto crc=header.u32();if(header.u32()!=0)bad(site,"unknown header flags");
    if(length!=data.size()-32||crc32(std::string_view(data).substr(32))!=crc)bad(site,"payload length/checksum mismatch");Reader r{std::string_view(data).substr(32),0,site,format};BytecodeImage image;
    auto source_count=r.count(128);if(!source_count)bad(site,"missing source table");std::size_t text_bytes=0;
    for(std::size_t i=0;i<source_count;++i){auto file=r.str(),text=r.str();if(file.empty()||file.size()>16384||file.find('\0')!=std::string::npos)bad(site,"invalid source id");text_bytes+=text.size();if(text_bytes>64*1024*1024)bad(site,"source table exceeds 64 MiB");auto source=std::make_unique<Source>(file,std::move(text));if(image.source(file))bad(site,"duplicate source id");image.sources.push_back(std::move(source));}
    std::vector<const Source*> originals;for(const auto& s:image.sources)originals.push_back(s.get());Sources src(originals);std::size_t fields_total=0;
    auto structures=r.count(16384);for(std::size_t i=0;i<structures;++i) {
        auto raw=r.str();auto name=raw.substr(0,raw.find(' '));if(raw!=name&&format<5)bad(site,"unsupported structure kind");if(raw!=name&&raw!=name+" interface"&&raw!=name+" enum")bad(site,"unknown structure kind");small_name(name,site);auto span=src.read(r);bool pub=r.boolean();auto n=std::make_unique<Node>(NodeKind::Struct,span,raw+(pub?" pub":""));auto count=r.count(4096);fields_total+=count;if(fields_total>1000000)bad(site,"metadata count budget exceeded");std::set<std::string> fields;
        for(std::size_t j=0;j<count;++j){auto field=r.str(),type=r.str();small_name(field,site);type_string(type,site);auto fs=src.read(r);if(!fields.insert(field).second)bad(site,"duplicate field");auto f=std::make_unique<Node>(NodeKind::Field,fs,field);f->add(std::make_unique<Node>(NodeKind::TypeName,fs,type));n->add(std::move(f));}
        if(!image.model.structures.emplace(name,n.get()).second)bad(site,"duplicate struct metadata");image.declarations.push_back(std::move(n));
    }
    auto functions=r.count(16384);for(std::size_t i=0;i<functions;++i) {
        auto raw=r.str();auto name=raw.substr(0,raw.find(' '));if(raw!=name&&format<5)bad(site,"unsupported function kind");if(raw!=name&&raw!=name+" abstract"&&raw!=name+" nested")bad(site,"unknown function kind");small_name(name,site);auto span=src.read(r);bool pub=r.boolean(),unsafe=r.boolean(),external=r.boolean(),mut=r.boolean();
        auto n=std::make_unique<Node>(NodeKind::Function,span,raw+(pub?" pub":"")+(unsafe?" unsafe":"")+(external?" external":""));auto count=r.count(4096);fields_total+=count;if(fields_total>1000000)bad(site,"metadata count budget exceeded");std::set<std::string> params;
        if(name=="main"&&count)bad(site,"main requires no parameters");
        for(std::size_t j=0;j<count;++j){auto name_p=r.str(),type=r.str();small_name(name_p,site);type_string(type,site);bool writable=r.boolean();auto ps=src.read(r);if(!params.insert(name_p).second)bad(site,"duplicate parameter");auto p=std::make_unique<Node>(NodeKind::Parameter,ps,name_p+(writable?" mut":""));if(!type.empty())p->add(std::make_unique<Node>(NodeKind::TypeName,ps,type));n->add(std::move(p));}
        auto results=r.count(4096);fields_total+=results;if(fields_total>1000000)bad(site,"metadata count budget exceeded");if(results){auto returns=std::make_unique<Node>(NodeKind::ReturnTypes,span);for(std::size_t j=0;j<results;++j){auto t=r.str();type_string(t,site);returns->add(std::make_unique<Node>(NodeKind::TypeName,span,t));}n->add(std::move(returns));}
        if(image.model.structures.contains(name)||!image.model.functions.emplace(name,n.get()).second)bad(site,"duplicate function metadata");
        if(auto dot=name.find('.');dot!=std::string::npos){if(!image.model.structures.contains(name.substr(0,dot)))bad(site,"method owner missing");image.model.mutating[name]=mut;}
        if(raw==name+" nested")image.model.nested[n.get()]=true;
        image.declarations.push_back(std::move(n));
    }
    auto external_count=r.count(128);if(external_count)image.model.external=std::make_shared<ExternalRegistry>(std::filesystem::canonical(path).parent_path());
    std::unordered_set<std::string> external_names;
    for(std::size_t i=0;i<external_count;++i) {
        ExternalModule m{};m.id=r.str();small_name(m.id,site);auto kind=r.u8();if(kind>1)bad(site,"unknown external module kind");m.kind=static_cast<ExternalKind>(kind);m.artifact=r.str();m.span=src.read(r);m.wasm=r.str(true);auto count=r.count(4096);
        for(std::size_t j=0;j<count;++j){ExternalExport e;e.name=r.str();e.linked_name=r.str();e.result=r.str();auto ps=r.count(32);for(std::size_t k=0;k<ps;++k)e.parameters.push_back(r.str());auto f=image.model.functions.find(e.linked_name);if(f==image.model.functions.end()||f->second->text.find(" external")==std::string::npos||!external_names.insert(e.linked_name).second)bad(site,"external declaration metadata mismatch");
            std::vector<std::string> types;std::string result="void";for(const auto& c:f->second->children){if(c->kind==NodeKind::Parameter)types.push_back(c->children.empty()?"":type_name(*c->children[0]));if(c->kind==NodeKind::ReturnTypes){if(c->children.size()!=1)bad(site,"invalid external return metadata");result=type_name(*c->children[0]);}}
            if(types!=e.parameters||result!=e.result)bad(site,"external signature mismatch");m.exports.push_back(std::move(e));}
        image.model.external->add(std::move(m));
    }
    std::size_t instructions=0;image.bytecode.initializer=code(r,src,instructions);if(image.bytecode.initializer.name!="<initialize>")bad(site,"invalid initializer code name");
    auto chunks=r.count(16384);for(std::size_t j=0;j<chunks;++j){auto c=code(r,src,instructions);auto fn=image.model.functions.find(c.name);if(fn==image.model.functions.end()||fn->second->text.find(" external")!=std::string::npos||!image.bytecode.functions.emplace(fn->second,std::move(c)).second)bad(site,"missing/duplicate function code metadata");}
    if(r.offset!=r.bytes.size())bad(site,"trailing payload data");if(format<7&&required_format(image.bytecode,image.model)==7)bad(site,"error/void-result capability requires HUAB7");image.model.relations=interface_relations(image.model);verify_bytecode(image.bytecode,image.model);return image;
}
}
