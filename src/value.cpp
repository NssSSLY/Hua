#include "hua/enum_value.hpp"
#include "hua/performance.hpp"
#include <set>
#include <unordered_map>
#include <unordered_set>
#include "hua/location.hpp"
#include "hua/value.hpp"
#include "hua/tasks.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
namespace hua {
using I = std::int64_t;
namespace {thread_local const TypeRelations* current_relations=nullptr;}
TypeRelationScope::TypeRelationScope(const TypeRelations& relations):previous(current_relations){current_relations=&relations;}
TypeRelationScope::~TypeRelationScope(){current_relations=previous;}
std::vector<std::pair<std::string,std::vector<std::string>>> enum_cases(const Node& n){
    std::map<std::string,std::vector<std::string>> cases;
    for(const auto& f:n.children)if(f->text.starts_with('$')&&f->text.find('#')!=std::string::npos){auto first=f->text.find('#'),second=f->text.find('#',first+1);auto name=f->text.substr(1,first-1),type=f->text.substr(second+1);if(type=="void"){cases[name];continue;}cases[name].push_back(type);}
    return {cases.begin(),cases.end()};
}
[[noreturn]] void runtime_error(const SourceSpan& span, std::string message, std::string code) {
    throw Diagnostic(std::move(code), span, std::move(message));
}
std::string declared_name(const Node& n) { return n.text.substr(0, n.text.find(' ')); }
std::optional<std::uint64_t> fixed_extent(std::string_view text) {
    unsigned base=10;
    if(text.starts_with("0x")){base=16;text.remove_prefix(2);}
    else if(text.starts_with("0o")){base=8;text.remove_prefix(2);}
    else if(text.starts_with("0b")){base=2;text.remove_prefix(2);}
    if(text.empty())return {};
    std::uint64_t count=0;bool digit=false;
    for(std::size_t i=0;i<text.size();++i){auto c=text[i];
        if(c=='_'){if(!digit||i+1==text.size())return {};digit=false;continue;}
        unsigned value=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:base;
        if(value>=base||count>(UINT64_MAX-value)/base)return {};
        count=count*base+value;digit=true;
    }
    return count;
}
std::string canonical_type(std::string_view type) {
    if(type.find('[')==std::string_view::npos)return std::string(type);
    if(type.ends_with('?'))return canonical_type(type.substr(0,type.size()-1))+"?";
    if(type.starts_with('[')){
        auto close=type.find(']');if(close==std::string_view::npos)return std::string(type);
        auto count=fixed_extent(type.substr(1,close-1));
        return "["+(count?std::to_string(*count):std::string(type.substr(1,close-1)))+"]"+canonical_type(type.substr(close+1));
    }
    for(auto name:{"map","Result","multi","List","Deque","Task","ref","ptr"}){
        auto parts=type_arguments(type,name);if(parts.empty())continue;
        std::string result=std::string(name)+"<";
        for(std::size_t i=0;i<parts.size();++i){if(i)result+=',';result+=canonical_type(parts[i]);}
        return result+">";
    }
    return std::string(type); // Nominal names retain their declared identity.
}
std::string type_name(const Node& n) {
    HUA_PROFILE_COUNT(type_name_calls);
    switch (n.kind) {
    case NodeKind::TypeName: return canonical_type(n.text);
    case NodeKind::MutableType: return type_name(*n.children[0]);
    case NodeKind::SliceType: return "[]" + type_name(*n.children[0]);
    case NodeKind::ArrayType: return canonical_type("[" + n.text + "]" + type_name(*n.children[0]));
    case NodeKind::OptionalType: return type_name(*n.children[0]) + "?";
    case NodeKind::GenericType: {
        std::string result = n.text + "<";
        for (std::size_t i=0; i<n.children.size(); ++i) { if(i) result+=','; result+=type_name(*n.children[i]); }
        if(n.text=="Result" && n.children.size()==1)result+=",string";
        return result + ">";
    }
    default: return "";
    }
}
std::string display_type(std::string type) {
    for(auto start=type.find('$');start!=std::string::npos;start=type.find('$',start)) {
        auto end=type.find('$',start+1);if(end==std::string::npos)break;
        type.erase(start,end-start+1);
    }
    return type;
}
const std::string* BuiltinName::intern(std::string text){
    static const std::string empty;
    if(text.empty())return &empty;
    // Node-based set: element addresses stay valid; never shrinks (finite builtin names).
    static std::mutex mutex;static std::unordered_set<std::string> names;
    std::lock_guard lock(mutex);return &*names.insert(std::move(text)).first;
}
BuiltinName::BuiltinName():text_(intern({})){}
std::string value_type(const Value& v) {
    switch(value_kind(v)) {
    case 0: return "nil"; case 1: return "bool"; case 2: return "int";
    case 3: return "float"; case 4: return "string";
    case 5: {const auto& s=std::get<SliceValue>(v.data);return (s.fixed?"["+std::to_string(s.length)+"]":"[]")+s.element_type;}
    case 6: return std::get<StructValue>(v.data).data->name;
    case 21:return std::get<PlainRecord>(v.data).layout->name;
    case 22:return "std.random.Rng";
    case 23:return "Deque<"+std::get<DequeValue>(v.data).data->element_type+">";
    case 7: { const auto& c=std::get<Callable>(v.data); return c.function ? "fn:"+declared_name(*c.function) : "builtin:"+c.builtin.str(); }
    case 8: {const auto& m=std::get<MapValue>(v.data);return "map<"+m.data->key_type+","+m.data->item_type+">";}
    case 9: {std::string out="multi<";const auto& vs=std::get<MultiValue>(v.data).data->values;for(std::size_t i=0;i<vs.size();++i){if(i)out+=',';out+=value_type(vs[i]);}return out+">";}
    case 10: {const auto& r=std::get<ResultValue>(v.data);return "Result<"+r.success_type+","+r.error_type+">";}
    case 15:{auto type=std::get<NumericValue>(v.data).type;return type=="@integer"?"int":type;}
    case 11:return "Json";
    case 12:return "Bytes";case 13:return "Buffer";case 14:return "List<"+std::get<ListValue>(v.data).data->element_type+">";
    case 18:return "void";
    case 17:return "std.error.Value";
    case 19:return "std.file.File";case 20:return "std.file.IOFailure";
    case 16:return "Task<"+std::get<TaskValue>(v.data).data->result_type+">";
    default: return "";
    }
}
bool compatible(std::string_view expected, std::string_view actual) {
    if(expected.find('[')!=std::string_view::npos||actual.find('[')!=std::string_view::npos){
        auto e=canonical_type(expected),a=canonical_type(actual);
        if(e!=expected||a!=actual)return compatible(e,a);
    }
    if(expected.empty() || actual.empty() || expected==actual || (expected=="void" && actual=="nil")) return true;
    if(numeric_widening(expected,actual))return true;
    if(current_relations){auto found=current_relations->find(std::string(expected));if(found!=current_relations->end()&&found->second.contains(std::string(actual)))return true;}
    if(expected.ends_with('?')) return actual=="nil" || compatible(expected.substr(0,expected.size()-1),actual);
    for(auto name:{"map","Result","multi","List","Deque","Task"}) {
        auto es=type_arguments(expected,name),as=type_arguments(actual,name);
        if(!es.empty()&&!as.empty()){if(std::string_view(name)=="map"||std::string_view(name)=="List"||std::string_view(name)=="Deque"||std::string_view(name)=="Task")return es==as;if(std::string_view(name)=="Result"&&es[0]=="void"&&as[0]=="nil")return false;if(es.size()!=as.size())return false;for(std::size_t i=0;i<es.size();++i)if(!compatible(es[i],as[i]))return false;return true;}
    }
    if(expected.starts_with('[') && actual.starts_with('[')) {
        auto close=expected.find(']');
        auto other=actual.find(']');if(close==std::string_view::npos||other==std::string_view::npos)return false;if(close>1&&other>1&&expected.substr(1,close-1)!=actual.substr(1,other-1))return false;return compatible(expected.substr(close+1),actual.substr(other+1));
    }
    return false;
}
bool contextual_literal(const Node& n){return numeric_literal(n)||n.kind==NodeKind::Array;}
bool numeric_literal(const Node& n) {
    return n.kind==NodeKind::Integer || n.kind==NodeKind::Float ||
        (n.kind==NodeKind::Unary && (n.text=="-" || n.text=="+") && numeric_literal(*n.children[0]));
}
std::int64_t as_int(const Value& v,const SourceSpan& s) {
    if(auto number=std::get_if<NumericValue>(&v.data)){if(numeric_spec(number->type)->category=='f')runtime_error(s,"expected integer, got "+number->type,"E4003");return std::get<I>(convert_numeric(v,"int",s,true).data);}
    if(auto p=std::get_if<I>(&v.data)) return *p;
    runtime_error(s,"expected int, got "+value_type(v),"E4003");
}
bool as_bool(const Value& v,const SourceSpan& s) {
    if(auto p=std::get_if<bool>(&v.data)) return *p;
    runtime_error(s,"condition/logical operand must be bool","E4003");
}
Value literal(const Node& n) {
    if(n.kind==NodeKind::Unary) {
        if(n.text=="-" && n.children[0]->kind==NodeKind::Integer) {
            auto text=n.children[0]->text;std::erase(text,'_');
            int base=10;
            if(text.starts_with("0x")){base=16;text.erase(0,2);}
            else if(text.starts_with("0b")){base=2;text.erase(0,2);}
            else if(text.starts_with("0o")){base=8;text.erase(0,2);}
            std::uint64_t number{};
            auto [end,err]=std::from_chars(text.data(),text.data()+text.size(),number,base);
            if(err==std::errc{} && end==text.data()+text.size() && number==(std::uint64_t{1}<<63))return Value(std::numeric_limits<I>::min());
        }
        return unary_value(n.text,literal(*n.children[0]),n.span);
    }
    if(n.kind==NodeKind::Nil) return {};
    if(n.kind==NodeKind::Boolean) return Value(n.text=="true");
    if(n.kind==NodeKind::String) return Value(n.text);
    std::string text=n.text; std::erase(text,'_');
    if(n.kind==NodeKind::Float) {
        double number{};
        auto [end,err]=std::from_chars(text.data(),text.data()+text.size(),number);
        if(err!=std::errc{} || end!=text.data()+text.size() || !std::isfinite(number))
            runtime_error(n.span,"floating literal is outside finite float64 range","E4002");
        return Value(number);
    }
    int base=10;
    if(text.starts_with("0x")){base=16;text.erase(0,2);}
    else if(text.starts_with("0b")){base=2;text.erase(0,2);}
    else if(text.starts_with("0o")){base=8;text.erase(0,2);}
    I number{};
    auto [end,err]=std::from_chars(text.data(),text.data()+text.size(),number,base);
    if(err!=std::errc{} || end!=text.data()+text.size()) runtime_error(n.span,"integer literal is outside signed int64 range","E4002");
    return Value(number);
}
namespace {
I add(I a,I b,const SourceSpan& s) {
    if((b>0 && a>std::numeric_limits<I>::max()-b)||(b<0 && a<std::numeric_limits<I>::min()-b)) runtime_error(s,"integer overflow","E4002");
    return a+b;
}
I sub(I a,I b,const SourceSpan& s) {
    if((b<0 && a>std::numeric_limits<I>::max()+b)||(b>0 && a<std::numeric_limits<I>::min()+b)) runtime_error(s,"integer overflow","E4002");
    return a-b;
}
I mul(I a,I b,const SourceSpan& s) {
    const I hi=std::numeric_limits<I>::max(),lo=std::numeric_limits<I>::min();
    if(a && b && ((a>0 && (b>0 ? a>hi/b : b<lo/a)) || (a<0 && (b>0 ? a<lo/b : a<hi/b))))
        runtime_error(s,"integer overflow","E4002");
    return a*b;
}
Value finite(double x,const SourceSpan& s) {
    if(!std::isfinite(x)) runtime_error(s,"floating result is not finite","E4002");
    return Value(x);
}
}
Value unary_value(std::string_view op,const Value& v,const SourceSpan& s) {
    if(std::holds_alternative<NumericValue>(v.data))return unary_numeric(op,v,s);
    if(op=="!") return Value(!as_bool(v,s));
    if(op=="~") return Value(~as_int(v,s));
    if(op=="+") { if(value_kind(v)==2 || value_kind(v)==3) return v; }
    if(op=="-") {
        if(auto p=std::get_if<I>(&v.data)) return Value(sub(0,*p,s));
        if(auto p=std::get_if<double>(&v.data)) return Value(-*p);
    }
    runtime_error(s,"unsupported unary operand for "+std::string(op),"E4003");
}
namespace {
bool is_record_value(const Value& v){return std::holds_alternative<StructValue>(v.data)||std::holds_alternative<PlainRecord>(v.data);}
// Structural equality for == / != (both static checkers already require equal
// operand types). Records compare type, enum variant and every field by name;
// maps compare key sets and values; sequences and lists compare element-wise;
// Results compare branch and payload. Kinds without a value notion of equality
// (functions, tasks, files, buffers, JSON, errors) keep E4008.
bool values_equal(const Value& a,const Value& b,const SourceSpan& s,unsigned depth){
    if(depth>128)runtime_error(s,"value nesting limit exceeded","E4099");
    if(is_record_value(a)||is_record_value(b)){
        if(!is_record_value(a)||!is_record_value(b))runtime_error(s,"equality requires matching types","E4003");
        Value left=a,right=b;auto* x=materialize_record(left);auto* y=materialize_record(right);
        if(x->data->name!=y->data->name)runtime_error(s,"equality requires matching types","E4003");
        const auto* vx=x->data->variant;const auto* vy=y->data->variant;
        if((vx==nullptr)!=(vy==nullptr)||(vx&&vx->name!=vy->name))return false; // variant identity by name
        if(x->data->fields.size()!=y->data->fields.size())return false;
        for(const auto& [name,item]:std::as_const(x->data->fields)){auto other=std::as_const(y->data->fields).find(name);
            if(other==std::as_const(y->data->fields).end()||!values_equal(item,other->second,s,depth+1))return false;}
        return true;
    }
    if(value_kind(a)!=value_kind(b)){
        if(value_kind(a)==0||value_kind(b)==0)return false;
        runtime_error(s,"equality requires matching types","E4003");
    }
    if(value_kind(a)==0)return true;
    if(auto p=std::get_if<bool>(&a.data))return *p==std::get<bool>(b.data);
    if(auto p=std::get_if<I>(&a.data))return *p==std::get<I>(b.data);
    if(auto p=std::get_if<double>(&a.data))return *p==std::get<double>(b.data);
    if(auto p=hua::string_data(&a.data))return *p==hua::string_value(b.data);
    if(auto p=std::get_if<NumericValue>(&a.data)){const auto& q=std::get<NumericValue>(b.data);return p->type==q.type&&p->number==q.number;}
    if(auto p=std::get_if<SliceValue>(&a.data)){const auto& q=std::get<SliceValue>(b.data);if(p->length!=q.length)return false;
        for(std::size_t i=0;i<p->length;++i)if(!values_equal(sequence_read(*p,i),sequence_read(q,i),s,depth+1))return false;return true;}
    if(auto p=std::get_if<MapValue>(&a.data)){const auto& q=std::get<MapValue>(b.data);if(p->data==q.data)return true;
        if(p->data->entries.size()!=q.data->entries.size())return false;
        for(const auto& [key,item]:p->data->entries){auto other=q.data->entries.find(key);
            if(other==q.data->entries.end()||!values_equal(item,other->second,s,depth+1))return false;}
        return true;}
    if(auto p=std::get_if<DequeValue>(&a.data)){const auto& q=std::get<DequeValue>(b.data);if(p->data==q.data)return true;
        const auto& x=p->data->values;const auto& y=q.data->values;if(x.size()!=y.size())return false;
        for(std::size_t i=0;i<x.size();++i)if(!values_equal(x[i],y[i],s,depth+1))return false;return true;}
    if(auto p=std::get_if<ListValue>(&a.data)){const auto& q=std::get<ListValue>(b.data);if(p->data==q.data)return true;
        const auto& x=p->data->values;const auto& y=q.data->values;if(x.size()!=y.size())return false;
        for(std::size_t i=0;i<x.size();++i)if(!values_equal(x[i],y[i],s,depth+1))return false;return true;}
    if(auto p=std::get_if<ResultValue>(&a.data)){const auto& q=std::get<ResultValue>(b.data);if(p->ok!=q.ok)return false;
        if(!p->payload||!q.payload)return !p->payload&&!q.payload;return values_equal(*p->payload,*q.payload,s,depth+1);}
    if(auto p=std::get_if<BytesValue>(&a.data))return *p->data==*std::get<BytesValue>(b.data).data;
    runtime_error(s,"equality is not implemented for this value kind","E4008");
}
}
Value binary_value(std::string_view op,const Value& a,const Value& b,const SourceSpan& s,bool contextual_right) {
    if(std::holds_alternative<VoidValue>(a.data)||std::holds_alternative<VoidValue>(b.data))runtime_error(s,"void is not an operand","E4003");
    if(contextual_right&&std::holds_alternative<NumericValue>(a.data)&&numeric_spec(value_type(b)))return binary_numeric(op,a,convert_numeric(b,value_type(a),s,true),s);
    if(std::holds_alternative<NumericValue>(a.data)||std::holds_alternative<NumericValue>(b.data))return binary_numeric(op,a,b,s);
    if(op=="==" || op=="!=") {
        bool same=false;
        if(value_kind(a)==value_kind(b)) {
            if(value_kind(a)==0) same=true;
            else if(auto p=std::get_if<bool>(&a.data)) same=*p==std::get<bool>(b.data);
            else if(auto p=std::get_if<I>(&a.data)) same=*p==std::get<I>(b.data);
            else if(auto p=std::get_if<double>(&a.data)) same=*p==std::get<double>(b.data);
            else if(auto p=hua::string_data(&a.data)) same=*p==hua::string_value(b.data);
            else same=values_equal(a,b,s,0);
        } else if(is_record_value(a)&&is_record_value(b)) same=values_equal(a,b,s,0); // StructValue vs packed PlainRecord
        else if(value_kind(a)!=0 && value_kind(b)!=0) runtime_error(s,"equality requires matching types","E4003");
        return Value(op=="==" ? same : !same);
    }
    if(op=="**" && value_kind(a)==3 && value_kind(b)==2) {
        double base=std::get<double>(a.data), result=1; I power=std::get<I>(b.data);
        if(power<0 && base==0)runtime_error(s,"zero cannot have a negative exponent","E4004");
        if(power<0)base=1/base;
        std::uint64_t magnitude=power<0 ? static_cast<std::uint64_t>(-(power+1))+1:static_cast<std::uint64_t>(power);
        while(magnitude) {
            if(magnitude&1)result=std::get<double>(finite(result*base,s).data);
            magnitude>>=1;
            if(magnitude)base=std::get<double>(finite(base*base,s).data);
        }
        return finite(result,s);
    }
    if(value_kind(a)!=value_kind(b)) runtime_error(s,"mixed types require an explicit conversion","E4003");
    if(op=="&&" || op=="||") return Value(op=="&&" ? as_bool(a,s)&&as_bool(b,s) : as_bool(a,s)||as_bool(b,s));
    if(auto p=hua::string_data(&a.data)) {
        const auto& q=hua::string_value(b.data);
        if(op=="+") return Value(*p+q);
        if(op=="<") return Value(*p<q); if(op=="<=") return Value(*p<=q);
        if(op==">") return Value(*p>q); if(op==">=") return Value(*p>=q);
    }
    if(auto p=std::get_if<I>(&a.data)) {
        I x=*p,y=std::get<I>(b.data);
        if(op=="+") return Value(add(x,y,s)); if(op=="-") return Value(sub(x,y,s));
        if(op=="*") return Value(mul(x,y,s));
        if(op=="/" || op=="//" || op=="%") {
            if(!y) runtime_error(s,"division by zero","E4004");
            if(op=="/") return finite(static_cast<double>(x)/static_cast<double>(y),s);
            if(x==std::numeric_limits<I>::min() && y==-1) {
                if(op=="%")return Value(I{0});
                runtime_error(s,"integer division overflow","E4002");
            }
            I q=x/y,r=x%y;
            if(r && ((r<0)!=(y<0))) { --q; r+=y; }
            return Value(op=="//" ? q : r);
        }
        if(op=="**") {
            if(y<0) runtime_error(s,"negative integer exponent requires explicit float operands","E4005");
            I result=1;
            while(y) { if(y&1) result=mul(result,x,s); y>>=1; if(y) x=mul(x,x,s); }
            return Value(result);
        }
        if(op=="&") return Value(x&y); if(op=="|") return Value(x|y); if(op=="^") return Value(x^y);
        if(op=="<<" || op==">>") {
            if(y<0 || y>=64) runtime_error(s,"shift count must be in 0..64","E4005");
            if(op==">>") return Value(x>>y);
            for(I i=0;i<y;++i) x=mul(x,2,s);
            return Value(x);
        }
        if(op=="<") return Value(x<y); if(op=="<=") return Value(x<=y);
        if(op==">") return Value(x>y); if(op==">=") return Value(x>=y);
    }
    if(auto p=std::get_if<double>(&a.data)) {
        double x=*p,y=std::get<double>(b.data);
        if(op=="+") return finite(x+y,s); if(op=="-") return finite(x-y,s); if(op=="*") return finite(x*y,s);
        if(op=="/") { if(y==0) runtime_error(s,"division by zero","E4004"); return finite(x/y,s); }
        if(op=="**") return finite(std::pow(x,y),s);
        if(op=="<") return Value(x<y); if(op=="<=") return Value(x<=y);
        if(op==">") return Value(x>y); if(op==">=") return Value(x>=y);
    }
    runtime_error(s,"operator "+std::string(op)+" is not defined for "+value_type(a),"E4003");
}
Value read_only(Value v) {
    if(auto p=std::get_if<PlainRecord>(&v.data))p->writable=false;
    if(auto p=std::get_if<BufferValue>(&v.data))p->writable=false;
    if(auto p=std::get_if<RngValue>(&v.data))p->writable=false;
    if(auto p=std::get_if<ListValue>(&v.data))p->writable=false;
    if(auto p=std::get_if<DequeValue>(&v.data))p->writable=false;
    if(auto p=std::get_if<SliceValue>(&v.data)) p->writable=false;
    if(auto p=std::get_if<StructValue>(&v.data)) p->writable=false;
    if(auto p=std::get_if<MapValue>(&v.data)) p->writable=false;
    if(auto p=std::get_if<ResultValue>(&v.data)) p->writable=false;
    if(auto p=std::get_if<MultiValue>(&v.data)){auto copy=managed<MultiData>(*p->data);for(auto& x:copy->values)x=read_only(std::move(x));p->data=std::move(copy);}
    return v;
}
namespace {
struct FileVisit {
    const void* object;std::size_t kind,start{},length{};
    bool operator==(const FileVisit&) const=default;
};
struct FileVisitHash {
    std::size_t operator()(const FileVisit& v) const {
        auto h=std::hash<const void*>{}(v.object);
        for(auto x:{v.kind,v.start,v.length})h^=x+0x9e3779b9+(h<<6)+(h>>2);
        return h;
    }
};
struct FileScan {std::unordered_set<FileVisit,FileVisitHash> visited;};
// Closure guards reenter contains_file; they must share this scan's visited graph.
thread_local FileScan* active_file_scan=nullptr;
bool scan_file(const Value& v,unsigned depth,FileScan& scan){
    if(depth>128)runtime_error({},"value nesting limit exceeded","E4099");
    if(std::holds_alternative<FileValue>(v.data))return true;
    auto first=[&](const void* object,std::size_t start=0,std::size_t length=0){
        return scan.visited.insert({object,value_kind(v),start,length}).second;
    };
    auto nested=[&](const Value& x){return scan_file(x,depth+1,scan);};
    if(auto p=std::get_if<Callable>(&v.data)){
        if(p->receiver&&nested(Value(*p->receiver)))return true;
        if(p->closure&&!first(p->closure.get()))return false;
        return p->resource_guard&&p->resource_guard(depth+1);
    }
    if(auto p=std::get_if<StructValue>(&v.data);p&&first(p->data.get()))
        for(const auto& [_,x]:p->data->fields)if(nested(x))return true;
    if(auto p=std::get_if<MapValue>(&v.data);p&&first(p->data.get()))
        for(const auto& [_,x]:p->data->entries)if(nested(x))return true;
    if(auto p=std::get_if<ListValue>(&v.data);p&&first(p->data.get()))
        for(const auto& x:p->data->values)if(nested(x))return true;
    if(auto p=std::get_if<DequeValue>(&v.data);p&&first(p->data.get()))
        for(const auto& x:p->data->values)if(nested(x))return true;
    // Different views of one backing can expose different resources.
    if(auto p=std::get_if<SliceValue>(&v.data);p&&!p->packed&&first(p->storage.get(),p->start,p->length))
        for(std::size_t i=0;i<p->length;++i)if(nested(sequence_copy_source(*p,i)))return true;
    if(auto p=std::get_if<ResultValue>(&v.data);p&&p->payload&&first(p->payload.get())&&nested(*p->payload))return true;
    if(auto p=std::get_if<MultiValue>(&v.data);p&&first(p->data.get()))
        for(const auto& x:p->data->values)if(nested(x))return true;
    return false;
}
}
bool contains_file(const Value& v,unsigned depth){
    if(active_file_scan)return scan_file(v,depth,*active_file_scan);
    FileScan scan;
    struct Scope {
        FileScan* previous;
        explicit Scope(FileScan& scan):previous(active_file_scan){active_file_scan=&scan;}
        ~Scope(){active_file_scan=previous;}
    } scope(scan);
    return scan_file(v,depth,scan);
}
namespace {
// These payloads have no mutable value surface. Sharing the Result box cannot
// change unwrap snapshots; contextual conversions still go through validation.
bool immutable_payload(const Value& v){
    auto kind=value_kind(v);return kind<=4||kind==12||kind==15||kind==17;
}
}
Value copy_value(const Value& v,bool deep,unsigned depth,bool resource_alias) {
    HUA_PROFILE_COUNT(copy_const);if(depth==0)HUA_PROFILE_KIND(copy_const_kinds,v);
#ifdef HUA_INTERNAL_PROFILE
    if(deep){HUA_PROFILE_COUNT(deep_clone_nodes);if(depth==0)HUA_PROFILE_COUNT(deep_clone_roots);}
#endif
    if(std::holds_alternative<StructValue>(v.data))HUA_PROFILE_COUNT(copy_struct);
    if(std::holds_alternative<ResultValue>(v.data))HUA_PROFILE_COUNT(copy_result);
    if(auto p=std::get_if<SliceValue>(&v.data);p&&p->fixed)HUA_PROFILE_COUNT(copy_fixed_array);
    if(deep&&!resource_alias&&depth==0&&contains_file(v))runtime_error({},"File cannot be cloned, including inside containers or closures","E4007");
    if(deep)if(auto p=std::get_if<IOFailureValue>(&v.data)){
        auto d=*p->data;d.error=std::get<ErrorValue>(copy_value(Value(d.error),true,depth+1,resource_alias).data);
        return Value(IOFailureValue{managed<const IOFailureData>(std::move(d))});
    }
    if(std::holds_alternative<VoidValue>(v.data))runtime_error({},"void has no copyable value","E4003");
    if(depth>128) runtime_error({},"value nesting limit exceeded","E4099");
    if(auto p=std::get_if<PlainRecord>(&v.data)){if(depth+p->layout->depth>128)runtime_error({},"value nesting limit exceeded","E4099");auto copy=*p;copy.writable=true;return Value(std::move(copy));}
    if(auto p=std::get_if<StructValue>(&v.data)){PlainRecord copy;if(pack_plain_record(*p,copy,depth))return Value(std::move(copy));}

    if(deep)if(auto p=std::get_if<ErrorValue>(&v.data)){auto data=*p->data;if(data.cause)data.cause=std::get<ErrorValue>(copy_value(Value(ErrorValue{data.cause}),true,depth+1,resource_alias).data).data;return Value(ErrorValue{managed<const ErrorData>(std::move(data))});}
    if(auto p=std::get_if<StructValue>(&v.data)) {
        require_complete_enum(*p->data);
        HUA_PROFILE_ADD(copy_struct_fields,p->data->fields.size());
#ifdef HUA_INTERNAL_PROFILE
        performance::record_struct_copy(p->data->name,p->data->fields.size());
#endif
        auto data=managed<StructData>(); data->name=p->data->name;
        data->enumeration=p->data->enumeration;data->variant=p->data->variant;
        data->fields=(!deep&&p->writable)?p->data->fields.snapshot(depth):p->data->fields.transform([&](const Value& x){return copy_value(!p->writable && !deep ? read_only(x):x,deep,depth+1,resource_alias);});
        return Value(StructValue{data,true});
    }
    if(auto p=std::get_if<ResultValue>(&v.data)) {auto r=*p;if(!deep&&r.payload&&immutable_payload(*r.payload))return Value(std::move(r));if(r.payload)r.payload=managed<Value>(copy_value(!r.writable&&!deep?read_only(*r.payload):*r.payload,deep,depth+1,resource_alias));if(deep)r.writable=true;return Value(r);}
    if(auto p=std::get_if<MultiValue>(&v.data)) {auto m=managed<MultiData>();m->contextual=p->data->contextual;m->values.reserve(p->data->values.size());for(const auto& x:p->data->values)m->values.push_back(copy_value(x,deep,depth+1,resource_alias));return Value(MultiValue{m});}
    if(deep)if(auto p=std::get_if<BufferValue>(&v.data))return Value(BufferValue{managed<std::string>(*p->data),true});
    if(deep)if(auto p=std::get_if<RngValue>(&v.data))return Value(RngValue{managed<RngState>(*p->data),true});
    if(deep)if(auto p=std::get_if<DequeValue>(&v.data)){auto data=managed<DequeData>();data->element_type=p->data->element_type;for(const auto& x:p->data->values)data->values.push_back(copy_value(x,deep,depth+1,resource_alias));return Value(DequeValue{data,true});}
    if(deep)if(auto p=std::get_if<ListValue>(&v.data)){auto data=managed<ListData>();data->element_type=p->data->element_type;data->values.reserve(p->data->values.size());for(const auto& x:p->data->values)data->values.push_back(copy_value(x,true,depth+1,resource_alias));return Value(ListValue{data,true});}
    if(deep) if(auto p=std::get_if<MapValue>(&v.data)){auto data=managed<MapData>();data->key_type=p->data->key_type;data->item_type=p->data->item_type;for(const auto& [k,x]:p->data->entries)data->entries.emplace(k,copy_value(x,true,depth+1,resource_alias));return Value(MapValue{data,true});}
    if(auto p=std::get_if<SliceValue>(&v.data);p&&(deep||p->fixed)) {
        auto storage=managed<std::vector<Value>>(); storage->reserve(p->length);
        for(std::size_t i=0;i<p->length;++i) storage->push_back(copy_value(sequence_copy_source(*p,i),deep,depth+1,resource_alias));
        SliceValue result{storage,0,p->length,deep||p->writable,p->element_type};if(p->fixed)result=fixed_array(result,p->element_type,{},false);return Value(result);
    }
    return v;
}
// A consumed, uniquely owned value needs no second backing allocation. Shared values
// retain the ordinary snapshot copy; writable/read-only descendants follow the same rule.
Value copy_value(Value&& v,bool deep,unsigned depth,bool resource_alias) {
    HUA_PROFILE_COUNT(copy_move);if(depth==0)HUA_PROFILE_KIND(copy_move_kinds,v);
    if(deep)return copy_value(static_cast<const Value&>(v),deep,depth,resource_alias);
    if(std::holds_alternative<VoidValue>(v.data))runtime_error({},"void has no copyable value","E4003");
    if(depth>128)runtime_error({},"value nesting limit exceeded","E4099");
    if(auto p=std::get_if<PlainRecord>(&v.data)){if(depth+p->layout->depth>128)runtime_error({},"value nesting limit exceeded","E4099");p->writable=true;return std::move(v);}
    // A shared struct takes the copying path, which makes its own packing attempt.
    if(auto p=std::get_if<StructValue>(&v.data);p&&p->data.use_count()!=1)return copy_value(static_cast<const Value&>(v),false,depth,resource_alias);
    if(auto p=std::get_if<StructValue>(&v.data)){PlainRecord copy;if(pack_plain_record(*p,copy,depth))return Value(std::move(copy));}

    if(auto p=std::get_if<StructValue>(&v.data)) {
        require_complete_enum(*p->data);
        p->data->fields.normalize_owned(!p->writable,depth,resource_alias);
        p->writable=true;return std::move(v);
    }
    if(auto p=std::get_if<ResultValue>(&v.data)) {
        if(p->payload) {
            if(p->payload.use_count()!=1)return copy_value(static_cast<const Value&>(v),false,depth,resource_alias);
            if(!p->writable)*p->payload=read_only(std::move(*p->payload));
            *p->payload=copy_value(std::move(*p->payload),false,depth+1,resource_alias);
        }
        return std::move(v);
    }
    if(auto p=std::get_if<MultiValue>(&v.data)) {
        if(p->data.use_count()!=1)return copy_value(static_cast<const Value&>(v),false,depth,resource_alias);
        for(auto& x:p->data->values)x=copy_value(std::move(x),false,depth+1,resource_alias);
        return std::move(v);
    }
    if(auto p=std::get_if<SliceValue>(&v.data);p&&p->fixed)
        return copy_value(static_cast<const Value&>(v),false,depth,resource_alias);
    return std::move(v);
}
Value enforce_type(Value v,const std::string& expected,const SourceSpan& span,bool contextual) {
    HUA_PROFILE_COUNT(enforce_type_calls);
    // Exact scalar/record matches leave the value unchanged on the general path
    // below; decide them without parsing type strings.
    switch(value_kind(v)){
    case 1:if(expected=="bool")return v;break;
    case 2:if(expected=="int")return v;break;
    case 3:if(expected=="float"&&std::isfinite(std::get<double>(v.data)))return v;break;
    case 4:if(expected=="string")return v;break;
    default:
        if(auto p=std::get_if<StructValue>(&v.data);p&&p->data->name==expected)return v;
        if(auto p=std::get_if<PlainRecord>(&v.data);p&&p->layout->name==expected)return v;
    }
    if(expected.find('[')!=std::string::npos){auto type=canonical_type(expected);if(type!=expected)return enforce_type(std::move(v),type,span,contextual);}
    if(std::holds_alternative<VoidValue>(v.data)&&expected!="void")runtime_error(span,"void has no usable value","E4003");
    if(expected.empty()) return v;

    if(expected.ends_with('?')) {
        if(value_kind(v)==0) return v;
        return enforce_type(std::move(v),expected.substr(0,expected.size()-1),span,contextual);
    }
    if(expected=="float" && contextual && value_kind(v)==2) {
        I x=std::get<I>(v.data);
        if(x>9007199254740992LL || x< -9007199254740992LL) runtime_error(span,"integer literal cannot be represented exactly as float","E4002");
        return Value(static_cast<double>(x));
    }
    if(numeric_spec(expected)&&numeric_spec(value_type(v)))return convert_numeric(v,expected,span,contextual);
    if(auto r=std::get_if<ResultValue>(&v.data);r&&!r->contextual&&r->payload&&immutable_payload(*r->payload)){
        std::string_view annotation(expected);
        auto a=std::string_view(r->success_type),b=std::string_view(r->error_type);
        if(annotation.size()==9+a.size()+b.size()&&annotation.starts_with("Result<")&&annotation.ends_with('>')&&
           annotation.substr(7,a.size())==a&&annotation[7+a.size()]==','&&annotation.substr(8+a.size(),b.size())==b&&
           value_type(*r->payload)==(r->ok?r->success_type:r->error_type)){
            enforce_type(*r->payload,r->ok?r->success_type:r->error_type,span,false);return v;
        }
    }
    auto result_types=type_arguments(expected,"Result");
    if(!result_types.empty()) {
        auto p=std::get_if<ResultValue>(&v.data);if(!p||result_types.size()!=2)runtime_error(span,"expected "+expected+", got "+value_type(v),"E4003");
        HUA_PROFILE_COUNT(result_constraints);
#ifdef HUA_INTERNAL_PROFILE
        performance::record_result_origin();
        if(p->ok)HUA_PROFILE_COUNT(result_ok_constraints);else HUA_PROFILE_COUNT(result_err_constraints);
        if(p->contextual)HUA_PROFILE_COUNT(result_contextual_constraints);
        if(!p->writable)HUA_PROFILE_COUNT(result_readonly_constraints);
        if(p->payload){
            if(p->payload.use_count()==1)HUA_PROFILE_COUNT(result_payload_unique);else HUA_PROFILE_COUNT(result_payload_shared);
            if(std::holds_alternative<StructValue>(p->payload->data))HUA_PROFILE_COUNT(result_payload_struct);
            if(std::holds_alternative<ResultValue>(p->payload->data))HUA_PROFILE_COUNT(result_payload_nested);
            if(auto array=std::get_if<SliceValue>(&p->payload->data);array&&array->fixed)HUA_PROFILE_COUNT(result_payload_fixed_array);
        }
#endif
        auto r=std::move(*p);auto inactive=r.ok?r.error_type:r.success_type;
        if(!compatible(result_types[r.ok?1:0],inactive))runtime_error(span,"incompatible Result type metadata","E4003");
        if(r.ok&&result_types[0]=="void"){HUA_PROFILE_COUNT(result_void_constraints);if(r.success_type!="void"||r.payload)runtime_error(span,"void success cannot carry a payload","E4003");r.error_type=result_types[1];return Value(r);}
        if(!r.payload)runtime_error(span,"Result payload is missing","E4003");
#ifdef HUA_INTERNAL_PROFILE
        const auto input_payload_type=value_type(*r.payload);
#endif
        if(!r.contextual&&immutable_payload(*r.payload)&&value_type(*r.payload)==result_types[r.ok?0:1]){
            // Keep finite-number/type validation, without rebuilding an unchanged box.
            enforce_type(*r.payload,result_types[r.ok?0:1],span,false);
            r.success_type=result_types[0];r.error_type=result_types[1];return Value(std::move(r));
        }
        // The wrapper is consumed, but a shared payload still belongs to its aliases.
        // Normalize an owned payload by move; copy_value below checks every descendant.
        auto input=r.payload.use_count()==1?std::move(*r.payload):*r.payload;
        auto payload=enforce_type(std::move(input),result_types[r.ok?0:1],span,r.contextual);
#ifdef HUA_INTERNAL_PROFILE
        if(value_type(payload)!=input_payload_type)HUA_PROFILE_COUNT(result_payload_type_changes);
#endif
        HUA_PROFILE_COUNT(result_materializations);
        r.payload=managed<Value>(copy_value(r.writable?std::move(payload):read_only(std::move(payload))));r.success_type=result_types[0];r.error_type=result_types[1];r.contextual=false;return Value(r);
    }
    if(!(contextual&&expected.starts_with('[')&&value_kind(v)==5)&&!compatible(expected,value_type(v))) runtime_error(span,"expected "+expected+", got "+value_type(v),"E4003");
    if(expected.starts_with('[') && value_kind(v)==5) {
        auto& slice=std::get<SliceValue>(v.data); auto close=expected.find(']');
        if(close>1) {
            Node count(NodeKind::Integer,span,expected.substr(1,close-1));
            if(as_int(literal(count),span)<0 || static_cast<std::size_t>(as_int(literal(count),span))!=slice.length)
                runtime_error(span,"fixed array length mismatch","E4003");
        }
        auto element=expected.substr(close+1);
        if(close>1){slice=fixed_array(slice,element,span,contextual);}
        else if(slice.element_type!=element){auto storage=managed<std::vector<Value>>();for(std::size_t i=0;i<slice.length;++i)storage->push_back(enforce_type(sequence_read(slice,i),element,span,contextual&&i<slice.contextual.size()&&slice.contextual[i]));slice=SliceValue{storage,0,slice.length,slice.writable,element};}
        else slice.fixed=false;
    }
    return v;
}
std::string show(const Value& v) {
    if(auto p=std::get_if<PlainRecord>(&v.data)){std::string text=display_type(p->layout->name)+"{";bool first=true;for(const auto& f:p->layout->fields){if(!first)text+=", ";first=false;text+=f.name+": "+show(*plain_record_field(*p,f.name));}return text+"}";}

    if(std::holds_alternative<VoidValue>(v.data))runtime_error({},"void has no displayable value","E4003");
    if(std::holds_alternative<FileValue>(v.data))return "<File>";
    if(std::holds_alternative<IOFailureValue>(v.data))return "<IOFailure>";
    if(std::holds_alternative<TaskValue>(v.data))return "<"+value_type(v)+">";
    if(auto p=std::get_if<NumericValue>(&v.data))return std::visit([](auto x)->std::string{if constexpr(std::is_same_v<decltype(x),std::uint64_t>)return std::to_string(x);else return show(Value(x));},p->number);
    if(auto p=std::get_if<BytesValue>(&v.data))return "<Bytes:"+std::to_string(p->data->size())+">";
    if(auto p=std::get_if<BufferValue>(&v.data))return "<Buffer:"+std::to_string(p->data->size())+">";
    if(std::holds_alternative<RngValue>(v.data))return "<Rng>";
    if(auto p=std::get_if<ListValue>(&v.data))return "<List:"+std::to_string(p->data->values.size())+">";
    if(auto p=std::get_if<DequeValue>(&v.data))return "<Deque:"+std::to_string(p->data->values.size())+">";
    if(value_kind(v)==0) return "nil";
    if(auto p=std::get_if<bool>(&v.data)) return *p ? "true":"false";
    if(auto p=std::get_if<I>(&v.data)) return std::to_string(*p);
    if(auto p=std::get_if<double>(&v.data)) { std::ostringstream out; out<<std::setprecision(15)<<*p; return out.str(); }
    if(auto p=hua::string_data(&v.data)) return *p;
    if(auto p=std::get_if<SliceValue>(&v.data)) {
        std::string out="["; for(std::size_t i=0;i<p->length;++i){if(i)out+=", ";out+=show(sequence_copy_source(*p,i));}return out+"]";
    }
    if(auto p=std::get_if<StructValue>(&v.data)) {
        if(auto tag=std::as_const(p->data->fields).find("$case");tag!=std::as_const(p->data->fields).end()){auto name=hua::string_value(tag->second.data);std::string out=display_type(p->data->name)+"."+name+"(";bool first=true;for(const auto& [field,value]:std::as_const(p->data->fields))if(field.starts_with("$"+name+"#")&&field.substr(field.rfind('#')+1)!="void"){if(!first)out+=", ";first=false;out+=show(value);}return out+")";}
        std::string out=display_type(p->data->name)+"{"; bool first=true;
        for(const auto& [k,x]:p->data->fields){if(!first)out+=", ";first=false;out+=k+": "+show(x);}return out+"}";
    }
    if(auto p=std::get_if<MapValue>(&v.data)){std::string out="map{";bool first=true;for(const auto& [k,x]:p->data->entries){if(!first)out+=", ";first=false;out+=std::visit([](const auto& v){return show(Value(v));},k)+": "+show(x);}return out+"}";}
    if(auto p=std::get_if<MultiValue>(&v.data)){std::string out="(";for(std::size_t i=0;i<p->data->values.size();++i){if(i)out+=", ";out+=show(p->data->values[i]);}return out+")";}
    if(auto p=std::get_if<ResultValue>(&v.data))return p->ok&&!p->payload?"ok()":std::string(p->ok?"ok(":"err(")+show(*p->payload)+")";
    if(auto p=std::get_if<ErrorValue>(&v.data))return p->data->domain+"."+p->data->code+": "+p->data->message;
    if(std::holds_alternative<JsonValue>(v.data))return "<Json>";
    return "<function>";
}
std::vector<std::string> type_arguments(std::string_view t,std::string_view name) {
    HUA_PROFILE_COUNT(type_argument_parses);
    if(!t.starts_with(std::string(name)+"<")||!t.ends_with('>'))return {};
    std::vector<std::string> out;std::size_t start=name.size()+1;unsigned depth=0;
    for(std::size_t i=start;i<t.size()-1;++i){if(t[i]=='<')++depth;else if(t[i]=='>'){if(!depth)return {};--depth;}else if(t[i]==','&&!depth){out.emplace_back(t.substr(start,i-start));start=i+1;}}
    if(depth)return {};out.emplace_back(t.substr(start,t.size()-1-start));return out;
}
Value make_multi(std::vector<Value> values,std::vector<bool> contextual) {auto m=managed<MultiData>();m->values=std::move(values);m->contextual=std::move(contextual);return Value(MultiValue{m});}
std::vector<Value> unpack_multi(const Value& v,std::size_t count,const SourceSpan& s) {auto p=std::get_if<MultiValue>(&v.data);if(!p||p->data->values.size()!=count)runtime_error(s,"multiple value count mismatch","E4003");return p->data->values;}
Value make_map(const std::string& type,const SourceSpan& s) {auto ts=type_arguments(canonical_type(type),"map");if(ts.size()!=2||(ts[0]!="bool"&&ts[0]!="int"&&ts[0]!="string"))runtime_error(s,"map keys require bool/int/string","E4003");auto data=managed<MapData>();data->key_type=ts[0];data->item_type=ts[1];return Value(MapValue{data,true});}
MapKey map_key(const MapValue& map,const Value& key,const SourceSpan& s) {auto value=enforce_type(key,map.data->key_type,s);if(auto p=std::get_if<bool>(&value.data))return *p;if(auto p=std::get_if<I>(&value.data))return *p;if(auto p=hua::string_data(&value.data))return *p;runtime_error(s,"map keys require bool/int/string","E4003");}
std::map<MapKey,Value>::iterator map_find(const MapValue& m,const Value& key,const SourceSpan& s){return m.data->entries.find(map_key(m,key,s));}
void map_put(MapValue& m,const Value& key,Value value,const SourceSpan& s,bool contextual) {if(!m.writable)runtime_error(s,"cannot mutate a read-only map","E4007");auto k=map_key(m,key,s);auto v=enforce_type(std::move(value),m.data->item_type,s,contextual);if(m.data->entries.size()>=1000000&&!m.data->entries.contains(k))runtime_error(s,"map entry limit exceeded","E4099");m.data->entries.insert_or_assign(k,copy_value(v));}
Value index_value(const Value& base,const Value& key,const SourceSpan& s) {
    if(auto m=std::get_if<MapValue>(&base.data)){auto found=map_find(*m,key,s);if(found==m->data->entries.end())return {};retain_record_alias(found->second);return m->writable?found->second:read_only(found->second);}
    auto p=std::get_if<SliceValue>(&base.data);if(!p)runtime_error(s,"index requires a slice, array or map","E4003");auto i=as_int(key,s);if(i<0||static_cast<std::size_t>(i)>=p->length)runtime_error(s,"index out of bounds","E4006");return sequence_read(*p,static_cast<std::size_t>(i));
}
ValueLocation index_location(Value base,Value key,const SourceSpan& s) {
    if(auto m=std::get_if<MapValue>(&base.data))return {nullptr,m->data->item_type,m->writable,m->data,m->data,map_key(*m,key,s)};
    auto p=std::get_if<SliceValue>(&base.data);if(!p)runtime_error(s,"index assignment requires a slice or map","E4003");auto i=as_int(key,s);if(i<0||static_cast<std::size_t>(i)>=p->length)runtime_error(s,"index out of bounds","E4006");if(p->packed){ValueLocation where;where.type=p->element_type;where.writable=p->writable;where.owner=p->packed;where.sequence=*p;where.index=static_cast<std::size_t>(i);return where;}return {&(*p->storage)[p->start+static_cast<std::size_t>(i)],p->element_type,p->writable,p->storage,{},{}};
}
const Value& location_read(const ValueLocation& l,const SourceSpan& s) {if(l.sequence){l.scratch=sequence_read(*l.sequence,l.index);return l.scratch;}if(l.map){auto it=l.map->entries.find(*l.key);if(it==l.map->entries.end())runtime_error(s,"compound assignment requires an existing map key","E4006");return it->second;}return *l.value;}
void prepare_map_writes(std::span<const ValueLocation> locations,const SourceSpan& span) {
    std::unordered_map<MapData*,std::set<MapKey>> additions;
    for(const auto& l:locations)if(l.map && !l.map->entries.contains(*l.key))additions[l.map.get()].insert(*l.key);
    for(const auto& [map,keys]:additions)
        if(map->entries.size()>1000000 || keys.size()>1000000-map->entries.size())
            runtime_error(span,"map entry limit exceeded","E4099");
}
void location_write(const ValueLocation& l,const Value& value,const SourceSpan& span) {
    if(l.sequence)sequence_write(*l.sequence,l.index,value);
    else if(l.map){
        if(l.map->entries.size()>=1000000 && !l.map->entries.contains(*l.key))runtime_error(span,"map entry limit exceeded","E4099");
        l.map->entries.insert_or_assign(*l.key,copy_value(value));
    }else *l.value=copy_value(value);
}
void location_write(const ValueLocation& l,const Value& value) {location_write(l,value,{});}
Value enforce_borrow(Value value,const std::string& type,const SourceSpan& span){if(std::holds_alternative<PlainRecord>(value.data))materialize_record(value);if(type.find('[')!=std::string::npos){auto normalized=canonical_type(type);if(normalized!=type)return enforce_borrow(std::move(value),normalized,span);}if(type.starts_with('[')){auto sequence=std::get_if<SliceValue>(&value.data);auto close=type.find(']');if(!sequence||sequence->element_type!=type.substr(close+1)||(close>1&&!sequence->fixed))runtime_error(span,"mut array/slice borrow requires identical element type and existing fixed layout","E4003");}return enforce_type(std::move(value),type,span);}
namespace {
template<class Type> Value return_value(Value value,std::size_t count,Type type,const SourceSpan& s,bool contextual){
    HUA_PROFILE_CONSTRAINT_SOURCE(returning);
    if(count==1)value=enforce_type(std::move(value),type(0),s,contextual);
    else if(count>1){auto values=unpack_multi(value,count,s);auto data=std::get<MultiValue>(value.data).data;for(std::size_t i=0;i<values.size();++i)values[i]=enforce_type(std::move(values[i]),type(i),s,i<data->contextual.size()&&data->contextual[i]);value=make_multi(std::move(values));}
    if(auto p=std::get_if<MultiValue>(&value.data))p->data->contextual.clear();if(auto p=std::get_if<ResultValue>(&value.data))p->contextual=false;
    return copy_value(std::move(value));
}
}
Value enforce_return(Value value,const Node& fn,const SourceSpan& s,bool contextual) {
    for(const auto& c:fn.children)if(c->kind==NodeKind::ReturnTypes)return return_value(std::move(value),c->children.size(),[&](std::size_t i){return type_name(*c->children[i]);},s,contextual);
    return return_value(std::move(value),0,[](std::size_t){return std::string{};},s,contextual);
}
Value enforce_return(Value value,const std::vector<std::string>& types,const SourceSpan& s,bool contextual) {
    return return_value(std::move(value),types.size(),[&](std::size_t i)->const std::string&{return types[i];},s,contextual);
}

Value record_failure_source(Value value,const SourceSpan& span,std::string message){
    auto result=std::get_if<ResultValue>(&value.data);
    if(!result||result->ok||span.file_id.empty())return value;
    auto trace=std::make_shared<SourceTrace>(result->failure_source?*result->failure_source:SourceTrace{});
    trace->add(span,std::move(message));result->failure_source=std::move(trace);return value;
}
Value propagated_error(const Value& value,const SourceSpan& span) {auto r=std::get<ResultValue>(copy_value(value).data);r.success_type.clear();return record_failure_source(Value(r),span,"Result propagated here");}
Value result_payload(const Value& v,bool success,const SourceSpan& s) {auto p=std::get_if<ResultValue>(&v.data);if(!p)runtime_error(s,"expected Result","E4003");if(p->ok!=success){Diagnostic error("E4009",s,success?"cannot unwrap err Result":"cannot unwrap_err ok Result");if(p->failure_source)error.add_trace(*p->failure_source);throw error;}if(success&&!p->payload&&p->success_type=="void")return Value(VoidValue{});if(!p->payload)runtime_error(s,"Result payload is missing","E4003");return copy_value(p->writable?*p->payload:read_only(*p->payload));}
Value aggregate_member(const Value& v,const std::string& name,const SourceSpan& s) {if(name=="len"){if(auto p=std::get_if<SliceValue>(&v.data))return Value(static_cast<I>(p->length));if(auto p=std::get_if<MapValue>(&v.data))return Value(static_cast<I>(p->data->entries.size()));if(auto p=hua::string_data(&v.data))return Value(static_cast<I>(p->size()));}runtime_error(s,"unknown aggregate member","E4003");}

}
