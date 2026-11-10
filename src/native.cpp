// Native backend prototype (2026-11-11): the linked, type-checked AST -> C++.
//
// The checker's type of every expression (SemanticModel::types) decides the C++ type, so
// generated code carries no run-time type information. A construct outside the subset
// rejects the whole program (NativeUnsupported); nothing is silently approximated.
//
// Value semantics: structs are C++ structs (copied when bound, passed or returned);
// slices, maps and lists are shared handles, as in the VM. A read-only struct parameter is
// passed by const reference when the callee has no `mut` parameter (nothing can then
// change the caller's variable during the call). Operands are evaluated left to right in
// GNU statement expressions. Each user call records its site (traces, depth limit) and
// each call and loop iteration charges the step budget.
#include "hua/native_backend.hpp"
#include "hua/runtime.hpp"
#include "hua/value.hpp"
#include <cstdio>
#include <deque>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
namespace hua {
namespace {
using N=NodeKind;
void dump(const Node& n,const SemanticModel& m,std::ostream& o,int depth){
    o<<std::string(depth*2,' ')<<node_name(n.kind);if(!n.text.empty())o<<' '<<n.text;
    if(auto t=m.types.find(&n);t!=m.types.end())o<<"  : "<<t->second;
    if(auto w=m.writable.find(&n);w!=m.writable.end()&&w->second)o<<" [w]";
    o<<'\n';for(const auto& c:n.children)dump(*c,m,o,depth+1);
}
struct Emitter {
    const Node& tree;const SemanticModel& m;
    std::ostringstream body,protos;
    std::vector<SourceSpan> spans;std::vector<std::string> literals;
    struct Local { std::string cpp,type; };
    std::vector<std::unordered_map<std::string,Local>> scopes;
    std::deque<const Node*> queue;std::set<const Node*> queued;
    std::unordered_map<std::string,const Node*> constants;
    unsigned counter{};
    std::string result_type; // current function: "" (void) or its declared return type
    [[noreturn]] void no(const Node& n,const std::string& what){throw NativeUnsupported{what,n.span};}
    std::string fresh(const std::string& base){std::string s="v_";for(char c:base)s+=std::isalnum(static_cast<unsigned char>(c))?c:'_';return s+"_"+std::to_string(counter++);}
    static std::string mangle(const std::string& prefix,const std::string& name){
        std::string s=prefix;char hex[8];
        for(unsigned char c:name){if(std::isalnum(c))s+=static_cast<char>(c);else{std::snprintf(hex,sizeof hex,"_%02x",c);s+=hex;}}
        return s;
    }
    std::string span(const SourceSpan& s){spans.push_back(s);return "SP["+std::to_string(spans.size()-1)+"]";}
    std::string literal(const std::string& text){literals.push_back(text);return "hn::Str(&L"+std::to_string(literals.size()-1)+")";}
    static std::string quoted(const std::string& text){
        std::string s="std::string(\"";char oct[8];
        for(unsigned char c:text){std::snprintf(oct,sizeof oct,"\\%03o",c);s+=oct;}
        return s+"\","+std::to_string(text.size())+")";
    }
    const std::string& ty(const Node& n){auto f=m.types.find(&n);if(f==m.types.end())no(n,"expression without a static type");return f->second;}
    bool is_struct(const std::string& t){auto f=m.structures.find(t);return f!=m.structures.end()&&f->second->text.find(" enum")==std::string::npos&&f->second->kind==N::Struct;}
    // map types are spelled `map[K]V` in source and `map<K,V>` by the checker.
    static bool is_map(const std::string& t){return t.rfind("map[",0)==0||t.rfind("map<",0)==0;}
    std::vector<std::string> map_parts(const std::string& t){
        if(t.starts_with("map<"))return type_arguments(t,"map");
        int depth=0;for(std::size_t i=3;i<t.size();++i){if(t[i]=='[')++depth;else if(t[i]==']'&&--depth==0)return {t.substr(4,i-4),t.substr(i+1)};}
        return {};
    }
    // The C++ type of a Hua type.
    std::string ct(const std::string& t,const Node& at){
        if(t=="int")return "hn::I";if(t=="float")return "hn::F";if(t=="bool")return "bool";if(t=="string")return "hn::Str";if(t=="void")return "hn::Unit";
        if(t.starts_with("[]"))return "hn::Slice<"+ct(t.substr(2),at)+">";
        if(is_map(t)){const auto a=map_parts(t);if(a.size()==2)return "hn::Map<"+ct(a[0],at)+","+ct(a[1],at)+">";}
        if(t.starts_with("List<")){const auto& a=type_arguments(t,"List");if(a.size()==1)return "hn::List<"+ct(a[0],at)+">";}
        if(t.starts_with("Result<")){const auto& a=type_arguments(t,"Result");
            if(a.size()==1)return "hn::Result<"+ct(a[0],at)+",hn::Str>";
            if(a.size()==2&&!a[0].empty()&&!a[1].empty())return "hn::Result<"+ct(a[0],at)+","+ct(a[1],at)+">";}
        if(t.size()>1&&t.back()=='?')return "hn::Opt<"+ct(t.substr(0,t.size()-1),at)+">";
        if(is_struct(t)&&(!structs_ready||usable_structs.contains(t)))return mangle("S_",t);
        no(at,"type `"+t+"`");
    }
    // Types the boxed bridge can carry in both directions (no handles with identity).
    bool value_like(const std::string& t){
        if(t=="int"||t=="float"||t=="bool"||t=="string"||t=="void")return true;
        if(t.starts_with("[]"))return value_like(t.substr(2));
        if(t.starts_with("Result<")){const auto& a=type_arguments(t,"Result");if(a.size()==1)return value_like(a[0]);return a.size()==2&&value_like(a[0])&&value_like(a[1]);}
        if(t.size()>1&&t.back()=='?')return value_like(t.substr(0,t.size()-1));
        return false;
    }
    const Local* find(const std::string& name){for(auto s=scopes.rbegin();s!=scopes.rend();++s)if(auto f=s->find(name);f!=s->end())return &f->second;return nullptr;}
    void need(const Node* fn){if(queued.insert(fn).second)queue.push_back(fn);}
    struct Signature { std::string name,result;std::vector<std::string> names,types;std::vector<bool> mut;bool any_mut{}; };
    Signature signature(const Node& fn){
        Signature s;s.name=declared_name(fn);
        if(fn.text.find(" external")!=std::string::npos||fn.text.find(" unsafe")!=std::string::npos||fn.text.find(" abstract")!=std::string::npos)no(fn,"external/unsafe/abstract function");
        if(s.name.find('.')!=std::string::npos||m.nested.contains(&fn))no(fn,"method or nested function");
        for(const auto& c:fn.children){
            if(c->kind==N::Parameter){
                if(c->children.empty())no(*c,"untyped parameter");
                const bool mut=c->text.find(" mut")!=std::string::npos||c->children[0]->kind==N::MutableType;
                s.names.push_back(declared_name(*c));s.types.push_back(type_name(*c->children[0]));s.mut.push_back(mut);s.any_mut=s.any_mut||mut;
            }else if(c->kind==N::ReturnTypes){
                if(c->children.size()>1)no(*c,"multiple return values");
                if(c->children.size()==1)s.result=type_name(*c->children[0]);
            }else if(c->kind==N::GenericParameters)no(*c,"generic function");
        }
        if(s.result=="void")s.result.clear();
        return s;
    }
    std::string parameter_type(const Signature& s,std::size_t i,const Node& at){
        const auto c=ct(s.types[i],at);
        if(s.mut[i])return c+"&";
        return is_struct(s.types[i])&&!s.any_mut?"const "+c+"&":c;
    }
    // `e` converted to Hua type `to` when the checker's type of the expression differs
    // (contextual numeric literals, ok()/err() whose other side is still open, nil).
    std::string coerce(const std::string& e,const std::string& from,const std::string& to,const Node& at){
        if(from==to)return e;
        return "static_cast<"+ct(to,at)+">("+e+")";
    }
    bool simple(const Node& n){return n.kind==N::Name||n.kind==N::Integer||n.kind==N::Float||n.kind==N::Boolean||n.kind==N::String||n.kind==N::Nil;}
    std::string scalar_literal(const Node& n){
        Value v=hua::literal(n);
        if(auto p=hua::get_if<std::int64_t>(&v.data)){if(*p==std::numeric_limits<std::int64_t>::min())return "(-9223372036854775807LL-1)";return "hn::I("+std::to_string(*p)+"LL)";}
        if(auto p=hua::get_if<double>(&v.data)){char buffer[64];std::snprintf(buffer,sizeof buffer,"%a",*p);return std::string("hn::F(")+buffer+")";}
        if(auto p=hua::get_if<bool>(&v.data))return *p?"true":"false";
        if(auto p=hua::string_data(&v.data))return literal(*p);
        if(v.data.index()==0)return "std::nullopt";
        no(n,"literal kind");
    }
    // An assignable C++ lvalue for an assignment target or a `mut` argument.
    std::string lvalue(const Node& n){
        if(n.kind==N::Name){auto l=find(n.text);if(!l)no(n,"assignment to a non-local name");return l->cpp;}
        if(n.kind==N::Member){if(!is_struct(ty(*n.children[0])))no(n,"field of a non-struct");return lvalue(*n.children[0])+"."+mangle("f_",n.text);}
        if(n.kind==N::Index&&ty(*n.children[0]).starts_with("[]"))return "hn::slot("+lvalue(*n.children[0])+","+expr(*n.children[1])+","+span(n.span)+")";
        no(n,"assignment target");
    }
    std::string local_type(const Node& n){
        if(n.kind==N::Name){auto l=find(n.text);if(!l)no(n,"unknown local");return l->type;}
        return ty(n);
    }
    std::string binary(const std::string& op,const std::string& l,const std::string& lt,const std::string& r,const std::string& rt,const std::string& out,const Node& at,bool contextual){
        const auto s=span(at.span);
        auto num=[&](const std::string& e,const std::string& t){return t=="int"?"static_cast<hn::F>("+e+")":e;};
        static const std::set<std::string> compare{"==","!=","<","<=",">",">="};
        if(lt=="int"&&rt=="int"){
            if(op=="+")return "hn::add("+l+","+r+","+s+")";if(op=="-")return "hn::sub("+l+","+r+","+s+")";if(op=="*")return "hn::mul("+l+","+r+","+s+")";
            if(op=="//")return "hn::floor_div("+l+","+r+","+s+")";if(op=="%")return "hn::mod("+l+","+r+","+s+")";
            if(compare.contains(op))return "("+l+op+r+")";
        }else if((lt=="float"||lt=="int")&&(rt=="float"||rt=="int")&&(out=="float"||out=="bool")){
            if(op=="+")return "hn::fadd("+num(l,lt)+","+num(r,rt)+","+s+")";if(op=="-")return "hn::fsub("+num(l,lt)+","+num(r,rt)+","+s+")";
            if(op=="*")return "hn::fmul("+num(l,lt)+","+num(r,rt)+","+s+")";if(op=="/")return "hn::fdiv("+num(l,lt)+","+num(r,rt)+","+s+")";
            if(compare.contains(op))return "("+num(l,lt)+op+num(r,rt)+")";
        }else if(lt=="bool"&&rt=="bool"&&(op=="=="||op=="!=")){return "("+l+op+r+")";
        }else if(lt=="string"&&rt=="string"){
            if(op=="+")return "hn::concat("+l+","+r+","+s+")";if(op=="==")return "("+l+"=="+r+")";if(op=="!=")return "(!("+l+"=="+r+"))";
        }
        return "hn::general_binary<"+ct(out,at)+">(\""+op+"\","+l+","+r+","+s+(contextual?",true":"")+")";
    }
    std::string expr(const Node& n){
        if(numeric_literal(n)||n.kind==N::String||n.kind==N::Boolean||n.kind==N::Nil)return scalar_literal(n);
        switch(n.kind){
        case N::Name:{
            if(auto l=find(n.text))return l->cpp;
            if(auto c=constants.find(n.text);c!=constants.end()){
                Value v=m.constants.at(c->second);
                if(auto p=hua::get_if<std::int64_t>(&v.data))return "hn::I("+std::to_string(*p)+"LL)";
                if(auto p=hua::get_if<bool>(&v.data))return *p?"true":"false";
                if(auto p=hua::string_data(&v.data))return literal(*p);
                if(auto p=hua::get_if<double>(&v.data)){char buffer[64];std::snprintf(buffer,sizeof buffer,"%a",*p);return std::string("hn::F(")+buffer+")";}
            }
            no(n,"name `"+n.text+"` (global or function value)");}
        case N::Unary:{
            const auto& t=ty(*n.children[0]);auto v=expr(*n.children[0]);
            if(n.text=="!"&&t=="bool")return "(!"+v+")";
            if(n.text=="-"&&t=="int")return "hn::neg("+v+","+span(n.span)+")";
            if(n.text=="-"&&t=="float")return "(-"+v+")";
            return "hn::general_unary<"+ct(ty(n),n)+">(\""+n.text+"\","+v+","+span(n.span)+")";}
        case N::Binary:{
            const auto& a=*n.children[0];const auto& b=*n.children[1];
            if(n.text=="&&"||n.text=="||")return "("+expr(a)+n.text+expr(b)+")";
            const auto& lt=ty(a);const auto& rt=ty(b);
            if(simple(a)&&simple(b))return binary(n.text,expr(a),lt,expr(b),rt,ty(n),n,numeric_literal(b));
            auto l=fresh("l"),r=fresh("r");
            return "({auto&& "+l+"="+expr(a)+";auto&& "+r+"="+expr(b)+";"+binary(n.text,l,lt,r,rt,ty(n),n,numeric_literal(b))+";})";}
        case N::Member:{
            const auto& t=ty(*n.children[0]);
            if(is_struct(t)){
                // Read the field in place when the object is itself a place; otherwise copy it out of the temporary.
                const auto& o=*n.children[0];
                if(o.kind==N::Name||o.kind==N::Member)return expr(o)+"."+mangle("f_",n.text);
                auto t0=fresh("o");return "({auto "+t0+"="+expr(o)+";auto "+t0+"f=std::move("+t0+"."+mangle("f_",n.text)+");"+t0+"f;})";
            }
            if(n.text=="len"&&(t=="string"||t.starts_with("[]")||is_map(t)))return "hn::len("+expr(*n.children[0])+")";
            no(n,"member `"+n.text+"` of `"+t+"`");}
        case N::Index:{
            const auto& t=ty(*n.children[0]);
            if(t.starts_with("[]")){auto b=fresh("b"),i=fresh("i");return "({auto&& "+b+"="+expr(*n.children[0])+";auto "+i+"="+expr(*n.children[1])+";auto "+b+"v=hn::at("+b+","+i+","+span(n.span)+");"+b+"v;})";}
            if(is_map(t)){const auto a=map_parts(t);auto b=fresh("b"),k=fresh("k");
                return "({auto&& "+b+"="+expr(*n.children[0])+";"+ct(a[0],n)+" "+k+"="+expr(*n.children[1])+";hn::at("+b+","+k+","+span(n.span)+");})";}
            no(n,"index into `"+t+"`");}
        case N::Array:{
            const auto& t=ty(n);if(!t.starts_with("[]"))no(n,"fixed array literal");
            const auto element=t.substr(2);std::string s="hn::Slice<"+ct(element,n)+">::of({";
            for(std::size_t i=0;i<n.children.size();++i){if(i)s+=",";s+=coerce(expr(*n.children[i]),ty(*n.children[i]),element,n);}
            return s+"})";}
        case N::StructLiteral:{
            if(!is_struct(n.text))no(n,"literal of `"+n.text+"`");
            const auto& decl=*m.structures.at(n.text);std::map<std::string,std::string> types;
            for(const auto& f:decl.children)if(f->kind==N::Field)types[f->text]=type_name(*f->children[0]);
            auto t=fresh("s");std::string s="({"+mangle("S_",n.text)+" "+t+"{};";
            for(const auto& c:n.children){auto f=types.find(c->text);if(f==types.end())no(*c,"unknown field");
                s+=t+"."+mangle("f_",c->text)+"="+coerce(expr(*c->children[0]),ty(*c->children[0]),f->second,*c)+";";}
            return s+t+";})";}
        case N::MapLiteral:{
            const auto t=type_name(*n.children[0]);const auto a=map_parts(t);if(a.size()!=2)no(n,"map literal type");
            auto v=fresh("m");std::string s="({auto "+v+"=hn::new_map<"+ct(a[0],n)+","+ct(a[1],n)+">();";
            for(std::size_t j=1;j<n.children.size();++j){const auto& e=*n.children[j];
                s+="hn::put("+v+","+coerce(expr(*e.children[0]),ty(*e.children[0]),a[0],e)+","+coerce(expr(*e.children[1]),ty(*e.children[1]),a[1],e)+","+span(e.span)+");";}
            return s+v+";})";}
        case N::Propagate:{
            if(!result_type.starts_with("Result<"))no(n,"propagation outside a Result function");
            auto t=fresh("p");return "({auto "+t+"="+expr(*n.children[0])+";if(!"+t+".ok)[[unlikely]]return hn::propagated(std::move("+t+".error),"+t+".trace,"+span(n.span)+");std::move("+t+".value);})";}
        case N::Call:return call(n,false);
        default:no(n,std::string("expression ")+std::string(node_name(n.kind)));
        }
    }
    // A call; as a statement its value (if any) is dropped.
    std::string call(const Node& n,bool statement){
        const auto& callee=*n.children[0];if(callee.kind!=N::Name)no(n,"call through a member or expression");
        const auto& kind=ty(callee);const std::size_t count=n.children.size()-1;
        auto arg=[&](std::size_t i)->const Node&{return *n.children[i+1];};
        if(kind.starts_with("fn:")){
            auto f=m.functions.find(kind.substr(3));if(f==m.functions.end())no(n,"unknown function");
            need(f->second);const auto sig=signature(*f->second);if(sig.names.size()!=count)no(n,"argument count");
            std::string s="({",list;
            for(std::size_t i=0;i<count;++i){
                if(i)list+=",";
                if(sig.mut[i]){list+=lvalue(arg(i));continue;}
                auto a=fresh("a");s+="auto&& "+a+"="+coerce(expr(arg(i)),ty(arg(i)),sig.types[i],arg(i))+";";list+=a;
            }
            auto d=fresh("d");s+="const unsigned "+d+"=hn::enter("+span(n.span)+");";
            const auto target=mangle("hf_",sig.name)+"("+list+")";
            if(sig.result.empty())return s+target+";hn::leave("+d+");hn::Unit{};})";
            auto r=fresh("r");return s+"auto "+r+"="+target+";hn::leave("+d+");std::move("+r+");})";
        }
        if(!kind.starts_with("builtin:"))no(n,"callee of kind `"+kind+"`");
        const std::string name=kind.substr(8);const auto s=span(n.span);
        auto a=[&](std::size_t i){return expr(arg(i));};
        if(name=="print"){std::string list;for(std::size_t i=0;i<count;++i){if(i)list+=",";list+="hn::to_value("+a(i)+")";}return "(hn::print("+s+",{"+list+"}),hn::Unit{})";}
        if(name=="len"&&count==1){const auto& t=ty(arg(0));if(t=="string"||t.starts_with("[]")||is_map(t))return "hn::len("+a(0)+")";}
        if(name=="str"&&count==1)return "hn::str("+a(0)+")";
        if(name=="ok"){
            if(count==0)return "hn::ok()";
            const auto& parts=type_arguments(ty(n),"Result");const auto payload=!parts.empty()&&!parts[0].empty()?parts[0]:ty(arg(0));
            return "hn::ok<"+ct(payload,n)+">("+coerce(a(0),ty(arg(0)),payload,n)+")";}
        if(name=="err"&&count==1)return "hn::err<"+ct(ty(arg(0)),n)+">("+a(0)+","+s+")";
        if((name=="is_ok"||name=="is_err"||name=="is_some"||name=="is_none")&&count==1)return "hn::"+name+"("+a(0)+")";
        if(name=="unwrap"&&count==1)return "hn::unwrap("+a(0)+","+s+")";
        if(name=="unwrap_or"&&count==2){auto x=fresh("u"),d=fresh("d");return "({auto "+x+"="+a(0)+";auto "+d+"="+coerce(a(1),ty(arg(1)),ty(n),n)+";hn::unwrap_or(std::move("+x+"),std::move("+d+"));})";}
        if(name=="has"&&count==2&&is_map(ty(arg(0)))){const auto parts=map_parts(ty(arg(0)));auto mv=fresh("m");
            return "({auto&& "+mv+"="+a(0)+";"+ct(parts[0],n)+" "+mv+"k="+a(1)+";hn::has("+mv+","+mv+"k);})";}
        if(name=="clone"&&count==1)return "hn::clone("+a(0)+")";
        if(name=="$std$list$from_slice"&&count==1)return "hn::list_from_slice("+a(0)+")";
        if(name=="$std$list$len"&&count==1)return "hn::list_len("+a(0)+")";
        if(name=="$std$list$snapshot"&&count==1)return "hn::list_snapshot("+a(0)+")";
        if(name=="$std$list$get"&&count==2){auto l=fresh("l"),i=fresh("i");return "({auto&& "+l+"="+a(0)+";auto "+i+"="+a(1)+";hn::list_get("+l+","+i+","+s+");})";}
        if(name=="$std$list$append"&&count==2){const auto& parts=type_arguments(ty(arg(0)),"List");auto l=fresh("l");
            return "({auto "+l+"="+a(0)+";"+ct(parts[0],n)+" "+l+"v="+a(1)+";hn::list_append("+l+",std::move("+l+"v),"+s+");})";}
        if((name=="$std$strings$starts_with"||name=="$std$strings$ends_with")&&count==2){auto x=fresh("s"),p=fresh("p");
            return "({auto&& "+x+"="+a(0)+";auto&& "+p+"="+a(1)+";hn::"+name.substr(13)+"("+x+","+p+");})";}
        if(name=="$std$strings$split"&&count==2){auto x=fresh("s"),p=fresh("p");return "({auto&& "+x+"="+a(0)+";auto&& "+p+"="+a(1)+";hn::split("+x+","+p+","+s+");})";}
        // Any other builtin runs the VM's implementation on boxed copies: only for
        // value-like types, and never for functions that call back or may suspend.
        const auto& info=builtin_info(name);
        if((info.standard&&info.standard->callback)||info.always_suspends||info.suspends_in_task||info.core==CoreBuiltin::task_start||info.core==CoreBuiltin::parallel_map||info.core==CoreBuiltin::task_await||info.core==CoreBuiltin::task_group_begin||info.core==CoreBuiltin::task_group_end)no(n,"builtin `"+name+"`");
        const auto& out=ty(n);if(!value_like(out))no(n,"builtin `"+name+"` returning `"+out+"`");
        std::string list,flags;
        for(std::size_t i=0;i<count;++i){if(!value_like(ty(arg(i))))no(arg(i),"builtin `"+name+"` with a `"+ty(arg(i))+"` argument");
            if(i){list+=",";flags+=",";}list+="hn::to_value("+a(i)+")";flags+=contextual_literal(arg(i))?"true":"false";}
        const auto site=fresh("site");
        const std::string boxed="hn::call_bridge("+site+","+s+",{"+list+"})";
        const std::string head="({static hn::Bridge "+site+"(\""+name+"\",{"+flags+"},\""+out+"\");";
        if(statement||out=="void")return head+boxed+";hn::Unit{};})";
        return head+"hn::from_value<"+ct(out,n)+">("+boxed+");})";
    }
    void line(const std::string& text){body<<text<<'\n';}
    void block(const Node& n){scopes.emplace_back();line("{");for(const auto& c:n.children)statement(*c);line("}");scopes.pop_back();}
    void declare(const std::string& name,const std::string& type,const std::string& init,const Node& at){
        if(scopes.back().contains(name))no(at,"redeclaration");
        auto cpp=fresh(name);line(ct(type,at)+" "+cpp+"="+init+";");scopes.back()[name]=Local{cpp,type};
    }
    void statement(const Node& n){
        switch(n.kind){
        case N::Block:block(n);break;
        case N::Let:case N::Var:{const auto& init=*n.children.back();const auto& t=ty(n);declare(n.text,t,coerce(expr(init),ty(init),t,n),n);break;}
        case N::ExpressionStatement:{
            const auto& e=*n.children[0];
            if(e.kind==N::Assignment||e.kind==N::Update){assignment(e);break;}
            if(e.kind==N::Call){line(call(e,true)+";");break;}
            line("(void)("+expr(e)+");");break;}
        case N::If:{
            line("if("+expr(*n.children[0])+")");statement_block(*n.children[1]);
            if(n.children.size()==3){line("else");statement_block(*n.children[2]);}break;}
        case N::While:{line("while(true){hn::step();if(!("+expr(*n.children[0])+"))break;");statement_block(*n.children[1]);line("}");break;}
        case N::For:loop(n);break;
        case N::Break:line("break;");break;
        case N::Continue:line("continue;");break;
        case N::Return:{
            if(n.children.size()>1)no(n,"multiple return values");
            if(n.children.empty()){line(result_type.empty()?"return;":"return {};");break;}
            const auto& e=*n.children[0];
            if(result_type.empty())no(n,"value returned from a void function");
            line("return "+coerce(expr(e),ty(e),result_type,n)+";");break;}
        default:no(n,std::string("statement ")+std::string(node_name(n.kind)));
        }
    }
    void statement_block(const Node& n){if(n.kind==N::Block)block(n);else{scopes.emplace_back();line("{");statement(n);line("}");scopes.pop_back();}}
    void assignment(const Node& n){
        const auto& target=*n.children[0];
        const std::string op=n.kind==N::Update?(n.text=="++"?"+=":"-="):n.text;
        const bool compound=op!="=";
        const auto target_type=local_type(target);
        std::string value,value_type;
        if(n.kind==N::Update){value="hn::I(1LL)";value_type="int";}else{value=expr(*n.children[1]);value_type=ty(*n.children[1]);}
        const bool literal_right=n.kind==N::Update||contextual_literal(*n.children[1]);
        if(target.kind==N::Index&&is_map(ty(*target.children[0]))){
            const auto parts=map_parts(ty(*target.children[0]));auto mv=fresh("m"),k=fresh("k"),v=fresh("v");
            line("{auto "+mv+"="+expr(*target.children[0])+";"+ct(parts[0],n)+" "+k+"="+expr(*target.children[1])+";");
            if(!compound)line(ct(parts[1],n)+" "+v+"="+coerce(value,value_type,parts[1],n)+";hn::put("+mv+",std::move("+k+"),std::move("+v+"),"+span(n.span)+");}");
            else line("auto& "+v+"=hn::slot("+mv+","+k+","+span(n.span)+");auto&& "+v+"r="+value+";"+v+"="+binary(op.substr(0,op.size()-1),v,parts[1],v+"r",value_type,parts[1],n,literal_right)+";}");
            return;
        }
        auto t=fresh("t");
        if(target.kind==N::Name&&!compound){line(lvalue(target)+"="+coerce(value,value_type,target_type,n)+";");return;}
        line("{auto& "+t+"="+lvalue(target)+";");
        if(!compound)line(t+"="+coerce(value,value_type,target_type,n)+";}");
        else line("auto&& "+t+"r="+value+";"+t+"="+binary(op.substr(0,op.size()-1),t,target_type,t+"r",value_type,target_type,n,literal_right)+";}");
    }
    void loop(const Node& n){
        const auto count=n.children.size()-2;const auto& iterable=*n.children[count];const auto& code=*n.children.back();
        scopes.emplace_back();line("{");
        if(iterable.kind==N::Range){
            if(count!=1)no(n,"range loop names");
            auto c=fresh("c"),e=fresh("e"),st=fresh("st"),done=fresh("done"),name=fresh(n.children[0]->text);
            line("hn::I "+c+"="+expr(*iterable.children[0])+";hn::I "+e+"="+expr(*iterable.children[1])+";hn::I "+st+"="+(iterable.children.size()==3?expr(*iterable.children[2]):std::string("1"))+";bool "+done+"=false;");
            line("if("+st+"==0)hn::fail("+span(iterable.span)+",\"range step cannot be zero\",\"E4005\");");
            const bool inclusive=iterable.text=="..=";
            line("while(!"+done+"&&("+st+">0?("+c+(inclusive?"<=":"<")+e+"):("+c+(inclusive?">=":">")+e+"))){hn::step();const hn::I "+name+"="+c+";");
            line(std::string("if(")+(inclusive?"("+c+"=="+e+")||":"")+"("+st+">0&&"+c+">std::numeric_limits<hn::I>::max()-"+st+")||("+st+"<0&&"+c+"<std::numeric_limits<hn::I>::min()-"+st+"))"+done+"=true;else "+c+"+="+st+";");
            scopes.back()[n.children[0]->text]=Local{name,"int"};
        }else{
            const auto& t=ty(iterable);
            if(t.starts_with("[]")){
                const auto element=t.substr(2);auto sl=fresh("sl"),ix=fresh("ix");
                line("auto "+sl+"="+expr(iterable)+";for(hn::I "+ix+"=0;"+ix+"<"+sl+".length;++"+ix+"){hn::step();");
                if(count==2){auto i=fresh(n.children[0]->text);line("const hn::I "+i+"="+ix+";");scopes.back()[n.children[0]->text]=Local{i,"int"};}
                auto v=fresh(n.children[count-1]->text);line("const "+ct(element,n)+" "+v+"=(*"+sl+".storage)[static_cast<std::size_t>("+sl+".start+"+ix+")];");
                scopes.back()[n.children[count-1]->text]=Local{v,element};
            }else if(is_map(t)){
                const auto parts=map_parts(t);auto mp=fresh("mp"),snap=fresh("snap"),kv=fresh("kv");
                line("auto "+mp+"="+expr(iterable)+";std::vector<std::pair<"+ct(parts[0],n)+","+ct(parts[1],n)+">> "+snap+"("+mp+".data->begin(),"+mp+".data->end());for(const auto& "+kv+":"+snap+"){hn::step();");
                auto k=fresh(n.children[0]->text);line("const "+ct(parts[0],n)+" "+k+"="+kv+".first;");scopes.back()[n.children[0]->text]=Local{k,parts[0]};
                if(count==2){auto v=fresh(n.children[1]->text);line("const "+ct(parts[1],n)+" "+v+"="+kv+".second;");scopes.back()[n.children[1]->text]=Local{v,parts[1]};}
            }else no(n,"loop over `"+t+"`");
        }
        for(const auto& c:code.children)statement(*c);
        line("}}");scopes.pop_back();
    }
    void function(const Node& fn){
        const auto sig=signature(fn);result_type=sig.result;
        std::string head=(sig.result.empty()?std::string("void"):ct(sig.result,fn))+" "+mangle("hf_",sig.name)+"(";
        scopes.clear();scopes.emplace_back();
        for(std::size_t i=0;i<sig.names.size();++i){auto cpp=fresh(sig.names[i]);if(i)head+=",";head+=parameter_type(sig,i,fn)+" "+cpp;scopes.back()[sig.names[i]]=Local{cpp,sig.types[i]};}
        head+=")";protos<<head<<";\n";line(head);
        block(*fn.children.back());
    }
    std::string structs(){
        std::ostringstream o;std::vector<const Node*> order;std::set<std::string> done,active;
        std::function<void(const std::string&)> visit=[&](const std::string& name){
            if(done.contains(name)||!is_struct(name))return;if(!active.insert(name).second)return;
            const auto& decl=*m.structures.at(name);
            for(const auto& f:decl.children)if(f->kind==N::Field){auto t=type_name(*f->children[0]);if(is_struct(t))visit(t);else if(t.size()>1&&t.back()=='?'&&is_struct(t.substr(0,t.size()-1)))visit(t.substr(0,t.size()-1));}
            done.insert(name);order.push_back(&decl);
        };
        std::vector<std::string> names;for(const auto& [name,decl]:m.structures)if(is_struct(name))names.push_back(name);
        std::sort(names.begin(),names.end());
        // Only structs whose field types are all in the subset are emitted; using any other
        // struct is rejected where its type is first needed.
        std::set<std::string> usable;
        for(const auto& name:names){try{const auto& decl=*m.structures.at(name);for(const auto& f:decl.children){if(f->kind==N::GenericParameters)throw NativeUnsupported{"generic struct",decl.span};if(f->kind==N::Field)(void)ct(type_name(*f->children[0]),*f);}usable.insert(name);}catch(const NativeUnsupported&){}}
        for(const auto& name:names)if(usable.contains(name))o<<"struct "<<mangle("S_",name)<<";\n";
        for(const auto& name:names)if(usable.contains(name))visit(name);
        for(const auto* decl:order){
            const auto name=declared_name(*decl);const auto key=[&]{for(const auto& [k,d]:m.structures)if(d==decl)return k;return name;}();
            if(!usable.contains(key))continue;
            const auto cpp=mangle("S_",key);std::string fields,names_list,types_list,boxing,cloning;
            for(const auto& f:decl->children)if(f->kind==N::Field){
                const auto t=type_name(*f->children[0]);const auto fc=mangle("f_",f->text);
                fields+="    "+ct(t,*f)+" "+fc+"{};\n";
                names_list+=(names_list.empty()?"":",")+quoted(f->text);types_list+=(types_list.empty()?"":",")+("{"+quoted(f->text)+","+quoted(t)+"}");
                boxing+="    d->fields.emplace("+quoted(f->text)+",to_value(v."+fc+"));\n";cloning+="    out."+fc+"=clone(v."+fc+");\n";
            }
            o<<"struct "<<cpp<<" {\n"<<fields<<"};\n";
            o<<"template<> struct TypeName<"<<cpp<<">{static std::string get(){return "<<quoted(key)<<";}};\n";
            o<<"inline Value to_value(const "<<cpp<<"& v){\n    static const auto layout=std::make_shared<const hua::RecordLayout>(std::vector<std::string>{"<<names_list<<"},"<<quoted(key)<<",std::map<std::string,std::string>{"<<types_list<<"});\n"
             <<"    auto d=hua::managed<hua::StructData>();d->name="<<quoted(key)<<";d->fields=hua::RecordFields(layout);\n"<<boxing<<"    return Value(hua::StructValue{d,true});\n}\n";
            o<<"inline "<<cpp<<" clone(const "<<cpp<<"& v){\n    "<<cpp<<" out{};\n"<<cloning<<"    return out;\n}\n";
        }
        usable_structs=std::move(usable);structs_ready=true;
        return o.str();
    }
    std::set<std::string> usable_structs;bool structs_ready{};
    std::string run(){
        for(const auto& c:tree.children){
            if(c->kind==N::Const)constants[c->text]=c.get();
            else if(c->kind==N::Let||c->kind==N::Var)no(*c,"module-level variable");
        }
        const auto struct_code=structs();
        auto main=m.functions.find("main");if(main==m.functions.end())no(tree,"program without main");
        const auto main_sig=signature(*main->second);if(!main_sig.names.empty())no(*main->second,"main with parameters");
        need(main->second);
        while(!queue.empty()){auto fn=queue.front();queue.pop_front();function(*fn);}
        const auto main_site=span(main->second->span);
        std::ostringstream o;
        o<<"// Generated by `hua native`. Do not edit.\n#include \"hua/native_rt.hpp\"\nnamespace hn {\n";
        o<<"static const hua::SourceSpan SP[]={\n";
        for(const auto& s:spans)o<<"    {"<<quoted(s.file_id)<<","<<s.start_offset<<","<<s.end_offset<<","<<s.line<<","<<s.column<<"},\n";
        o<<"};\n";
        for(std::size_t i=0;i<literals.size();++i)o<<"static StrRep L"<<i<<"(1u<<30,"<<quoted(literals[i])<<");\n";
        o<<struct_code<<protos.str()<<body.str();
        o<<"static Value entry(){\n";
        if(main_sig.result.empty())o<<"    hf_main();return Value{};\n";else o<<"    return to_value(hf_main());\n";
        o<<"}\n}\nint main(int argc,char** argv){return hn::run_main(argc,argv,hn::"<<main_site<<",&hn::entry);}\n";
        return o.str();
    }
};
}
std::string dump_typed_ast(const Node& tree,const SemanticModel& model){std::ostringstream o;dump(tree,model,o,0);return o.str();}
std::string emit_native(const Node& tree,const SemanticModel& model){Emitter e{tree,model};return e.run();}
}
