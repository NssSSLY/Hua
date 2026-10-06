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
std::string type_name(const Node& n) {
    switch (n.kind) {
    case NodeKind::TypeName: return n.text;
    case NodeKind::MutableType: return type_name(*n.children[0]);
    case NodeKind::SliceType: return "[]" + type_name(*n.children[0]);
    case NodeKind::ArrayType: return "[" + n.text + "]" + type_name(*n.children[0]);
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
std::string value_type(const Value& v) {
    switch(v.data.index()) {
    case 0: return "nil"; case 1: return "bool"; case 2: return "int";
    case 3: return "float"; case 4: return "string";
    case 5: {const auto& s=std::get<SliceValue>(v.data);return (s.fixed?"["+std::to_string(s.length)+"]":"[]")+s.element_type;}
    case 6: return std::get<StructValue>(v.data).data->name;
    case 7: { const auto& c=std::get<Callable>(v.data); return c.function ? "fn:"+declared_name(*c.function) : "builtin:"+c.builtin; }
    case 8: {const auto& m=std::get<MapValue>(v.data);return "map<"+m.data->key_type+","+m.data->item_type+">";}
    case 9: {std::string out="multi<";const auto& vs=std::get<MultiValue>(v.data).data->values;for(std::size_t i=0;i<vs.size();++i){if(i)out+=',';out+=value_type(vs[i]);}return out+">";}
    case 10: {const auto& r=std::get<ResultValue>(v.data);return "Result<"+r.success_type+","+r.error_type+">";}
    case 15:{auto type=std::get<NumericValue>(v.data).type;return type=="@integer"?"int":type;}
    case 11:return "Json";
    case 12:return "Bytes";case 13:return "Buffer";case 14:return "List<"+std::get<ListValue>(v.data).data->element_type+">";
    case 16:return "Task<"+std::get<TaskValue>(v.data).data->result_type+">";
    default: return "";
    }
}
bool compatible(std::string_view expected, std::string_view actual) {
    if(expected.empty() || actual.empty() || expected==actual || (expected=="void" && actual=="nil")) return true;
    if(numeric_widening(expected,actual))return true;
    if(current_relations){auto found=current_relations->find(std::string(expected));if(found!=current_relations->end()&&found->second.contains(std::string(actual)))return true;}
    if(expected.ends_with('?')) return actual=="nil" || compatible(expected.substr(0,expected.size()-1),actual);
    for(auto name:{"map","Result","multi","List","Task"}) {
        auto es=type_arguments(expected,name),as=type_arguments(actual,name);
        if(!es.empty()&&!as.empty()){if(std::string_view(name)=="map"||std::string_view(name)=="List"||std::string_view(name)=="Task")return es==as;if(es.size()!=as.size())return false;for(std::size_t i=0;i<es.size();++i)if(!compatible(es[i],as[i]))return false;return true;}
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
    if(op=="+") { if(v.data.index()==2 || v.data.index()==3) return v; }
    if(op=="-") {
        if(auto p=std::get_if<I>(&v.data)) return Value(sub(0,*p,s));
        if(auto p=std::get_if<double>(&v.data)) return Value(-*p);
    }
    runtime_error(s,"unsupported unary operand for "+std::string(op),"E4003");
}
Value binary_value(std::string_view op,const Value& a,const Value& b,const SourceSpan& s,bool contextual_right) {
    if(contextual_right&&std::holds_alternative<NumericValue>(a.data)&&numeric_spec(value_type(b)))return binary_numeric(op,a,convert_numeric(b,value_type(a),s,true),s);
    if(std::holds_alternative<NumericValue>(a.data)||std::holds_alternative<NumericValue>(b.data))return binary_numeric(op,a,b,s);
    if(op=="==" || op=="!=") {
        bool same=false;
        if(a.data.index()==b.data.index()) {
            if(a.data.index()==0) same=true;
            else if(auto p=std::get_if<bool>(&a.data)) same=*p==std::get<bool>(b.data);
            else if(auto p=std::get_if<I>(&a.data)) same=*p==std::get<I>(b.data);
            else if(auto p=std::get_if<double>(&a.data)) same=*p==std::get<double>(b.data);
            else if(auto p=std::get_if<std::string>(&a.data)) same=*p==std::get<std::string>(b.data);
            else runtime_error(s,"aggregate equality is not implemented","E4008");
        } else if(a.data.index()!=0 && b.data.index()!=0) runtime_error(s,"equality requires matching types","E4003");
        return Value(op=="==" ? same : !same);
    }
    if(op=="**" && a.data.index()==3 && b.data.index()==2) {
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
    if(a.data.index()!=b.data.index()) runtime_error(s,"mixed types require an explicit conversion","E4003");
    if(op=="&&" || op=="||") return Value(op=="&&" ? as_bool(a,s)&&as_bool(b,s) : as_bool(a,s)||as_bool(b,s));
    if(auto p=std::get_if<std::string>(&a.data)) {
        const auto& q=std::get<std::string>(b.data);
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
    if(auto p=std::get_if<BufferValue>(&v.data))p->writable=false;
    if(auto p=std::get_if<ListValue>(&v.data))p->writable=false;
    if(auto p=std::get_if<SliceValue>(&v.data)) p->writable=false;
    if(auto p=std::get_if<StructValue>(&v.data)) p->writable=false;
    if(auto p=std::get_if<MapValue>(&v.data)) p->writable=false;
    if(auto p=std::get_if<ResultValue>(&v.data)) p->writable=false;
    if(auto p=std::get_if<MultiValue>(&v.data)){auto copy=managed<MultiData>(*p->data);for(auto& x:copy->values)x=read_only(std::move(x));p->data=std::move(copy);}
    return v;
}
Value copy_value(const Value& v,bool deep,unsigned depth) {
    if(depth>128) runtime_error({},"value nesting limit exceeded","E4099");
    if(auto p=std::get_if<StructValue>(&v.data)) {
        auto data=managed<StructData>(); data->name=p->data->name;
        for(const auto& [k,x]:p->data->fields) data->fields.emplace(k,copy_value(!p->writable && !deep ? read_only(x):x,deep,depth+1));
        return Value(StructValue{data,true});
    }
    if(auto p=std::get_if<ResultValue>(&v.data)) {auto r=*p;r.payload=managed<Value>(copy_value(!r.writable&&!deep?read_only(*r.payload):*r.payload,deep,depth+1));if(deep)r.writable=true;return Value(r);}
    if(auto p=std::get_if<MultiValue>(&v.data)) {auto m=managed<MultiData>();m->contextual=p->data->contextual;for(const auto& x:p->data->values)m->values.push_back(copy_value(x,deep,depth+1));return Value(MultiValue{m});}
    if(deep)if(auto p=std::get_if<BufferValue>(&v.data))return Value(BufferValue{managed<std::string>(*p->data),true});
    if(deep)if(auto p=std::get_if<ListValue>(&v.data)){auto data=managed<ListData>();data->element_type=p->data->element_type;for(const auto& x:p->data->values)data->values.push_back(copy_value(x,true,depth+1));return Value(ListValue{data,true});}
    if(deep) if(auto p=std::get_if<MapValue>(&v.data)){auto data=managed<MapData>();data->key_type=p->data->key_type;data->item_type=p->data->item_type;for(const auto& [k,x]:p->data->entries)data->entries.emplace(k,copy_value(x,true,depth+1));return Value(MapValue{data,true});}
    if(auto p=std::get_if<SliceValue>(&v.data);p&&(deep||p->fixed)) {
        auto storage=managed<std::vector<Value>>(); storage->reserve(p->length);
        for(std::size_t i=0;i<p->length;++i) storage->push_back(copy_value(sequence_read(*p,i),deep,depth+1));
        SliceValue result{storage,0,p->length,deep||p->writable,p->element_type};if(p->fixed)result=fixed_array(result,p->element_type,{},false);return Value(result);
    }
    return v;
}
Value enforce_type(Value v,const std::string& expected,const SourceSpan& span,bool contextual) {
    if(expected.empty()) return v;

    if(expected.ends_with('?')) {
        if(v.data.index()==0) return v;
        return enforce_type(std::move(v),expected.substr(0,expected.size()-1),span,contextual);
    }
    if(expected=="float" && contextual && v.data.index()==2) {
        I x=std::get<I>(v.data);
        if(x>9007199254740992LL || x< -9007199254740992LL) runtime_error(span,"integer literal cannot be represented exactly as float","E4002");
        return Value(static_cast<double>(x));
    }
    if(numeric_spec(expected)&&numeric_spec(value_type(v)))return convert_numeric(v,expected,span,contextual);
    auto result_types=type_arguments(expected,"Result");
    if(!result_types.empty()) {
        auto p=std::get_if<ResultValue>(&v.data);if(!p||result_types.size()!=2)runtime_error(span,"expected "+expected+", got "+value_type(v),"E4003");
        auto r=*p;auto inactive=r.ok?r.error_type:r.success_type;
        if(!compatible(result_types[r.ok?1:0],inactive))runtime_error(span,"incompatible Result type metadata","E4003");
        auto payload=enforce_type(*r.payload,result_types[r.ok?0:1],span,r.contextual);
        r.payload=managed<Value>(copy_value(r.writable?payload:read_only(payload)));r.success_type=result_types[0];r.error_type=result_types[1];r.contextual=false;return Value(r);
    }
    if(!(contextual&&expected.starts_with('[')&&v.data.index()==5)&&!compatible(expected,value_type(v))) runtime_error(span,"expected "+expected+", got "+value_type(v),"E4003");
    if(expected.starts_with('[') && v.data.index()==5) {
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
    if(std::holds_alternative<TaskValue>(v.data))return "<"+value_type(v)+">";
    if(auto p=std::get_if<NumericValue>(&v.data))return std::visit([](auto x)->std::string{if constexpr(std::is_same_v<decltype(x),std::uint64_t>)return std::to_string(x);else return show(Value(x));},p->number);
    if(auto p=std::get_if<BytesValue>(&v.data))return "<Bytes:"+std::to_string(p->data->size())+">";
    if(auto p=std::get_if<BufferValue>(&v.data))return "<Buffer:"+std::to_string(p->data->size())+">";
    if(auto p=std::get_if<ListValue>(&v.data))return "<List:"+std::to_string(p->data->values.size())+">";
    if(v.data.index()==0) return "nil";
    if(auto p=std::get_if<bool>(&v.data)) return *p ? "true":"false";
    if(auto p=std::get_if<I>(&v.data)) return std::to_string(*p);
    if(auto p=std::get_if<double>(&v.data)) { std::ostringstream out; out<<std::setprecision(15)<<*p; return out.str(); }
    if(auto p=std::get_if<std::string>(&v.data)) return *p;
    if(auto p=std::get_if<SliceValue>(&v.data)) {
        std::string out="["; for(std::size_t i=0;i<p->length;++i){if(i)out+=", ";out+=show(sequence_read(*p,i));}return out+"]";
    }
    if(auto p=std::get_if<StructValue>(&v.data)) {
        if(auto tag=p->data->fields.find("$case");tag!=p->data->fields.end()){auto name=std::get<std::string>(tag->second.data);std::string out=display_type(p->data->name)+"."+name+"(";bool first=true;for(const auto& [field,value]:p->data->fields)if(field.starts_with("$"+name+"#")&&field.substr(field.rfind('#')+1)!="void"){if(!first)out+=", ";first=false;out+=show(value);}return out+")";}
        std::string out=display_type(p->data->name)+"{"; bool first=true;
        for(const auto& [k,x]:p->data->fields){if(!first)out+=", ";first=false;out+=k+": "+show(x);}return out+"}";
    }
    if(auto p=std::get_if<MapValue>(&v.data)){std::string out="map{";bool first=true;for(const auto& [k,x]:p->data->entries){if(!first)out+=", ";first=false;out+=std::visit([](const auto& v){return show(Value(v));},k)+": "+show(x);}return out+"}";}
    if(auto p=std::get_if<MultiValue>(&v.data)){std::string out="(";for(std::size_t i=0;i<p->data->values.size();++i){if(i)out+=", ";out+=show(p->data->values[i]);}return out+")";}
    if(auto p=std::get_if<ResultValue>(&v.data))return std::string(p->ok?"ok(":"err(")+show(*p->payload)+")";
    if(std::holds_alternative<JsonValue>(v.data))return "<Json>";
    return "<function>";
}
std::vector<std::string> type_arguments(std::string_view t,std::string_view name) {
    if(!t.starts_with(std::string(name)+"<")||!t.ends_with('>'))return {};
    std::vector<std::string> out;std::size_t start=name.size()+1;unsigned depth=0;
    for(std::size_t i=start;i<t.size()-1;++i){if(t[i]=='<')++depth;else if(t[i]=='>'){if(!depth)return {};--depth;}else if(t[i]==','&&!depth){out.emplace_back(t.substr(start,i-start));start=i+1;}}
    if(depth)return {};out.emplace_back(t.substr(start,t.size()-1-start));return out;
}
Value make_multi(std::vector<Value> values,std::vector<bool> contextual) {auto m=managed<MultiData>();m->values=std::move(values);m->contextual=std::move(contextual);return Value(MultiValue{m});}
std::vector<Value> unpack_multi(const Value& v,std::size_t count,const SourceSpan& s) {auto p=std::get_if<MultiValue>(&v.data);if(!p||p->data->values.size()!=count)runtime_error(s,"multiple value count mismatch","E4003");return p->data->values;}
Value make_map(const std::string& type,const SourceSpan& s) {auto ts=type_arguments(type,"map");if(ts.size()!=2||(ts[0]!="bool"&&ts[0]!="int"&&ts[0]!="string"))runtime_error(s,"map keys require bool/int/string","E4003");auto data=managed<MapData>();data->key_type=ts[0];data->item_type=ts[1];return Value(MapValue{data,true});}
MapKey map_key(const MapValue& map,const Value& key,const SourceSpan& s) {auto value=enforce_type(key,map.data->key_type,s);if(auto p=std::get_if<bool>(&value.data))return *p;if(auto p=std::get_if<I>(&value.data))return *p;if(auto p=std::get_if<std::string>(&value.data))return *p;runtime_error(s,"map keys require bool/int/string","E4003");}
void map_put(MapValue& m,const Value& key,Value value,const SourceSpan& s,bool contextual) {if(!m.writable)runtime_error(s,"cannot mutate a read-only map","E4007");auto k=map_key(m,key,s);auto v=enforce_type(std::move(value),m.data->item_type,s,contextual);if(m.data->entries.size()>=1000000&&!m.data->entries.contains(k))runtime_error(s,"map entry limit exceeded","E4099");m.data->entries.insert_or_assign(k,copy_value(v));}
Value index_value(const Value& base,const Value& key,const SourceSpan& s) {
    if(auto m=std::get_if<MapValue>(&base.data)){auto found=m->data->entries.find(map_key(*m,key,s));if(found==m->data->entries.end())return {};return m->writable?found->second:read_only(found->second);}
    auto p=std::get_if<SliceValue>(&base.data);if(!p)runtime_error(s,"index requires a slice, array or map","E4003");auto i=as_int(key,s);if(i<0||static_cast<std::size_t>(i)>=p->length)runtime_error(s,"index out of bounds","E4006");return sequence_read(*p,static_cast<std::size_t>(i));
}
ValueLocation index_location(Value base,Value key,const SourceSpan& s) {
    if(auto m=std::get_if<MapValue>(&base.data))return {nullptr,m->data->item_type,m->writable,m->data,m->data,map_key(*m,key,s)};
    auto p=std::get_if<SliceValue>(&base.data);if(!p)runtime_error(s,"index assignment requires a slice or map","E4003");auto i=as_int(key,s);if(i<0||static_cast<std::size_t>(i)>=p->length)runtime_error(s,"index out of bounds","E4006");if(p->packed){ValueLocation where;where.type=p->element_type;where.writable=p->writable;where.owner=p->packed;where.sequence=*p;where.index=static_cast<std::size_t>(i);return where;}return {&(*p->storage)[p->start+static_cast<std::size_t>(i)],p->element_type,p->writable,p->storage,{},{}};
}
const Value& location_read(const ValueLocation& l,const SourceSpan& s) {if(l.sequence){l.scratch=sequence_read(*l.sequence,l.index);return l.scratch;}if(l.map){auto it=l.map->entries.find(*l.key);if(it==l.map->entries.end())runtime_error(s,"compound assignment requires an existing map key","E4006");return it->second;}return *l.value;}
void location_write(const ValueLocation& l,const Value& value) {if(l.sequence)sequence_write(*l.sequence,l.index,value);else if(l.map)l.map->entries.insert_or_assign(*l.key,copy_value(value));else *l.value=copy_value(value);}
Value enforce_borrow(Value value,const std::string& type,const SourceSpan& span){if(type.starts_with('[')){auto sequence=std::get_if<SliceValue>(&value.data);auto close=type.find(']');if(!sequence||sequence->element_type!=type.substr(close+1)||(close>1&&!sequence->fixed))runtime_error(span,"mut array/slice borrow requires identical element type and existing fixed layout","E4003");}return enforce_type(std::move(value),type,span);}
Value enforce_return(Value value,const Node& fn,const SourceSpan& s,bool contextual) {
    for(const auto& c:fn.children)if(c->kind==NodeKind::ReturnTypes){if(c->children.size()==1)value=enforce_type(std::move(value),type_name(*c->children[0]),s,contextual);else {auto values=unpack_multi(value,c->children.size(),s);auto data=std::get<MultiValue>(value.data).data;for(std::size_t i=0;i<values.size();++i)values[i]=enforce_type(std::move(values[i]),type_name(*c->children[i]),s,i<data->contextual.size()&&data->contextual[i]);value=make_multi(std::move(values));}}
    if(auto p=std::get_if<MultiValue>(&value.data))p->data->contextual.clear();if(auto p=std::get_if<ResultValue>(&value.data))p->contextual=false;
    return copy_value(value);
}
Value propagated_error(const Value& value) {auto r=std::get<ResultValue>(copy_value(value).data);r.success_type.clear();return Value(r);}
Value result_payload(const Value& v,bool success,const SourceSpan& s) {auto p=std::get_if<ResultValue>(&v.data);if(!p)runtime_error(s,"expected Result","E4003");if(p->ok!=success)runtime_error(s,success?"cannot unwrap err Result":"cannot unwrap_err ok Result","E4009");return copy_value(p->writable?*p->payload:read_only(*p->payload));}
Value aggregate_member(const Value& v,const std::string& name,const SourceSpan& s) {if(name=="len"){if(auto p=std::get_if<SliceValue>(&v.data))return Value(static_cast<I>(p->length));if(auto p=std::get_if<MapValue>(&v.data))return Value(static_cast<I>(p->data->entries.size()));if(auto p=std::get_if<std::string>(&v.data))return Value(static_cast<I>(p->size()));}runtime_error(s,"unknown aggregate member","E4003");}

}
