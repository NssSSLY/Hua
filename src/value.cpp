#include "hua/value.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
namespace hua {
using I = std::int64_t;
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
    case 5: return "[]" + std::get<SliceValue>(v.data).element_type;
    case 6: return std::get<StructValue>(v.data).data->name;
    case 7: { const auto& c=std::get<Callable>(v.data); return c.function ? "fn:"+declared_name(*c.function) : "builtin:"+c.builtin; }
    default: return "";
    }
}
bool compatible(std::string_view expected, std::string_view actual) {
    if(expected.empty() || actual.empty() || expected==actual || (expected=="void" && actual=="nil")) return true;
    if(expected.ends_with('?')) return actual=="nil" || compatible(expected.substr(0,expected.size()-1),actual);
    if(expected.starts_with('[') && actual.starts_with("[]")) {
        auto close=expected.find(']');
        return close!=std::string_view::npos && compatible(expected.substr(close+1),actual.substr(2));
    }
    return false;
}
bool numeric_literal(const Node& n) {
    return n.kind==NodeKind::Integer || n.kind==NodeKind::Float ||
        (n.kind==NodeKind::Unary && (n.text=="-" || n.text=="+") && numeric_literal(*n.children[0]));
}
std::int64_t as_int(const Value& v,const SourceSpan& s) {
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
    if(op=="!") return Value(!as_bool(v,s));
    if(op=="~") return Value(~as_int(v,s));
    if(op=="+") { if(v.data.index()==2 || v.data.index()==3) return v; }
    if(op=="-") {
        if(auto p=std::get_if<I>(&v.data)) return Value(sub(0,*p,s));
        if(auto p=std::get_if<double>(&v.data)) return Value(-*p);
    }
    runtime_error(s,"unsupported unary operand for "+std::string(op),"E4003");
}
Value binary_value(std::string_view op,const Value& a,const Value& b,const SourceSpan& s) {
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
    if(auto p=std::get_if<SliceValue>(&v.data)) p->writable=false;
    if(auto p=std::get_if<StructValue>(&v.data)) p->writable=false;
    return v;
}
Value copy_value(const Value& v,bool deep,unsigned depth) {
    if(depth>128) runtime_error({},"value nesting limit exceeded","E4099");
    if(auto p=std::get_if<StructValue>(&v.data)) {
        auto data=std::make_shared<StructData>(); data->name=p->data->name;
        for(const auto& [k,x]:p->data->fields) data->fields.emplace(k,copy_value(!p->writable && !deep ? read_only(x):x,deep,depth+1));
        return Value(StructValue{data,true});
    }
    if(deep) if(auto p=std::get_if<SliceValue>(&v.data)) {
        auto storage=std::make_shared<std::vector<Value>>(); storage->reserve(p->length);
        for(std::size_t i=0;i<p->length;++i) storage->push_back(copy_value((*p->storage)[p->start+i],true,depth+1));
        return Value(SliceValue{storage,0,p->length,true,p->element_type});
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
    if(!compatible(expected,value_type(v))) runtime_error(span,"expected "+expected+", got "+value_type(v),"E4003");
    if(expected.starts_with('[') && v.data.index()==5) {
        auto& slice=std::get<SliceValue>(v.data); auto close=expected.find(']');
        if(close>1) {
            Node count(NodeKind::Integer,span,expected.substr(1,close-1));
            if(as_int(literal(count),span)<0 || static_cast<std::size_t>(as_int(literal(count),span))!=slice.length)
                runtime_error(span,"fixed array length mismatch","E4003");
        }
        auto element=expected.substr(close+1);
        for(std::size_t i=0;i<slice.length;++i) enforce_type((*slice.storage)[slice.start+i],element,span);
        slice.element_type=element;
    }
    return v;
}
std::string show(const Value& v) {
    if(v.data.index()==0) return "nil";
    if(auto p=std::get_if<bool>(&v.data)) return *p ? "true":"false";
    if(auto p=std::get_if<I>(&v.data)) return std::to_string(*p);
    if(auto p=std::get_if<double>(&v.data)) { std::ostringstream out; out<<std::setprecision(15)<<*p; return out.str(); }
    if(auto p=std::get_if<std::string>(&v.data)) return *p;
    if(auto p=std::get_if<SliceValue>(&v.data)) {
        std::string out="["; for(std::size_t i=0;i<p->length;++i){if(i)out+=", ";out+=show((*p->storage)[p->start+i]);}return out+"]";
    }
    if(auto p=std::get_if<StructValue>(&v.data)) {
        std::string out=display_type(p->data->name)+"{"; bool first=true;
        for(const auto& [k,x]:p->data->fields){if(!first)out+=", ";first=false;out+=k+": "+show(x);}return out+"}";
    }
    return "<function>";
}
}
