#include "hua/sema.hpp"
#include <algorithm>
#include <functional>
#include <unordered_set>
namespace hua {
namespace {
using N=NodeKind;
[[noreturn]] void error(const Node& n,std::string message,std::string code="E3001") { throw Diagnostic(std::move(code),n.span,std::move(message)); }
std::string result_type(const Node& fn) {
    for(const auto& c:fn.children) if(c->kind==N::ReturnTypes) return c->children.size()==1 ? type_name(*c->children[0]):"multi";
    return "";
}
std::vector<const Node*> parameters(const Node& fn) {
    std::vector<const Node*> out; for(const auto& c:fn.children) if(c->kind==N::Parameter)out.push_back(c.get());return out;
}
bool mutable_parameter(const Node& p) { return p.text.find(" mut")!=std::string::npos || (!p.children.empty() && p.children[0]->kind==N::MutableType); }
bool definitely_returns(const Node& n) {
    if(n.kind==N::Return)return true;
    if(n.kind==N::If)return n.children.size()==3 && definitely_returns(*n.children[1]) && definitely_returns(*n.children[2]);
    if(n.kind==N::Block) for(const auto& c:n.children)if(definitely_returns(*c))return true;
    return false;
}
const Node& root_target(const Node& n) {
    if((n.kind==N::Member || n.kind==N::Index || n.kind==N::Slice) && !n.children.empty())return root_target(*n.children[0]);return n;
}
std::string element_type(std::string type) {
    if(type.starts_with('[')) {auto end=type.find(']'); if(end!=std::string::npos)return type.substr(end+1);}return "";
}
std::string unref(std::string type) {
    if(type.starts_with("ref<") && type.ends_with('>'))return type.substr(4,type.size()-5);return type;
}
const std::unordered_set<std::string> builtin_names={"print","str","int","float","len","clone","sqrt","min","max","abs","clamp","type"};
}
SemanticAnalyzer::Symbol& SemanticAnalyzer::lookup(const std::string& name,const SourceSpan& span) {
    for(auto i=scopes_.rbegin();i!=scopes_.rend();++i)if(auto found=i->find(name);found!=i->end())return found->second;
    throw Diagnostic("E3001",span,"undefined name `"+name+"`");
}
void SemanticAnalyzer::define(const std::string& name,Symbol symbol,const SourceSpan& span) {
    if(!scopes_.back().emplace(name,std::move(symbol)).second)throw Diagnostic("E3002",span,"duplicate declaration `"+name+"` in this scope");
}
void SemanticAnalyzer::validate_type(const Node& n) {
    static const std::unordered_set<std::string> types={"int","float","bool","string","byte","i8","i16","i32","i64","u8","u16","u32","u64","f32","f64","usize","isize","void","Result"};
    if(n.kind==N::TypeName && !types.contains(n.text) && !model_.structures.contains(n.text)) {
        if(n.text.find('.')==std::string::npos)error(n,"unknown type `"+n.text+"`","E3004");
        auto prefix=n.text.substr(0,n.text.find('.'));lookup(prefix,n.span);
    }
    for(const auto& c:n.children)validate_type(*c);
}
void SemanticAnalyzer::require(const std::string& expected,const Info& actual,const Node& n) {
    if(expected=="float" && actual.type=="int" && numeric_literal(n))return;
    if(!compatible(expected,actual.type))error(n,"expected "+expected+", got "+actual.type,"E3004");
}
void SemanticAnalyzer::infer_mutating_methods() {
    std::function<bool(const Node&,const std::string&)> direct=[&](const Node& n,const std::string& owner) {
        if((n.kind==N::Assignment || n.kind==N::Update) && root_target(*n.children[0]).kind==N::Name && root_target(*n.children[0]).text=="self")return true;
        if(n.kind==N::Call && n.children[0]->kind==N::Member) {
            const auto& member=*n.children[0]; const auto& root=root_target(*member.children[0]);
            if(root.kind==N::Name && root.text=="self") {
                std::function<std::string(const Node&)> receiver=[&](const Node& x)->std::string {
                    if(x.kind==N::Name && x.text=="self")return owner;
                    if(x.kind==N::Member) {
                        auto t=receiver(*x.children[0]);auto found=model_.structures.find(t);
                        if(found!=model_.structures.end())for(const auto& f:found->second->children)if(f->text==x.text)return unref(type_name(*f->children[0]));
                    }
                    return "";
                };
                auto key=receiver(*member.children[0])+"."+member.text;
                if(model_.mutating.contains(key) && model_.mutating.at(key))return true;
            }
        }
        for(const auto& c:n.children)if(direct(*c,owner))return true;return false;
    };
    bool changed=true;
    while(changed) {
        changed=false;
        for(const auto& [name,fn]:model_.functions)if(auto dot=name.find('.');dot!=std::string::npos)
            if(!model_.mutating[name] && direct(*fn->children.back(),name.substr(0,dot))){model_.mutating[name]=true;changed=true;}
    }
}
SemanticModel SemanticAnalyzer::analyze(const Node& program,std::shared_ptr<ExternalRegistry> external) {
    model_={};model_.external=std::move(external);scopes_.clear();scopes_.emplace_back();current_function_=nullptr;
    for(const auto& name:builtin_names){define(name,{"builtin:"+name,false,false,{}},program.span);define("$core$"+name,{"builtin:"+name,false,false,{}},program.span);}
    scopes_.emplace_back();
    for(const auto& n:program.children) {
        if(n->kind==N::Function) {
            auto name=declared_name(*n);define(name,{"fn:"+name,false,false,{}},n->span);model_.functions.emplace(name,n.get());
        } else if(n->kind==N::Struct) {
            auto name=declared_name(*n);define(name,{"struct:"+name,false,false,{}},n->span);model_.structures.emplace(name,n.get());
        }
    }
    for(const auto& [name,structure]:model_.structures) {
        std::unordered_set<std::string> fields;
        for(const auto& f:structure->children){if(!fields.insert(f->text).second)error(*f,"duplicate struct field `"+f->text+"`","E3002");validate_type(*f->children[0]);}
    }
    for(const auto& [name,fn]:model_.functions) {
        if(auto dot=name.find('.');dot!=std::string::npos && !model_.structures.contains(name.substr(0,dot)))error(*fn,"method owner is not a declared struct","E3004");
        for(const auto& p:parameters(*fn))if(!p->children.empty())validate_type(*p->children[0]);
        for(const auto& c:fn->children)if(c->kind==N::ReturnTypes)for(const auto& t:c->children)validate_type(*t);
    }
    infer_mutating_methods();
    for(const auto& n:program.children)if(n->kind!=N::Function && n->kind!=N::Struct)statement(*n);
    // Function bodies see all module-level bindings; local bindings remain declaration-ordered.
    for(const auto& [name,fn]:model_.functions) {
        scopes_.emplace_back();current_function_=fn;
        if(auto dot=name.find('.');dot!=std::string::npos)define("self",{name.substr(0,dot),false,model_.mutating[name],{}},fn->span);
        auto params=parameters(*fn);
        if(name=="main" && !params.empty())error(*fn,"main must take no parameters","E3005");
        for(const auto* p:params)define(declared_name(*p),{p->children.empty()?"":type_name(*p->children[0]),false,mutable_parameter(*p),{}},p->span);
        if(fn->text.find(" external")!=std::string::npos) {
            if(!model_.external || !model_.external->contains(name))error(*fn,"unregistered external declaration","E7003");
            scopes_.pop_back();current_function_=nullptr;continue;
        }
        statements(*fn->children.back(),false);
        auto result=result_type(*fn);
        if(!result.empty() && result!="void" && !result.ends_with('?') && !definitely_returns(*fn->children.back()))
            error(*fn,"typed function may reach its end without returning a value","E3006");
        scopes_.pop_back();current_function_=nullptr;
    }
    return model_;
}
void SemanticAnalyzer::statements(const Node& block,bool scope) {
    if(scope)scopes_.emplace_back();for(const auto& n:block.children)statement(*n);if(scope)scopes_.pop_back();
}
Value SemanticAnalyzer::constant(const Node& n) {
    switch(n.kind) {
    case N::Integer:case N::Float:case N::String:case N::Boolean:case N::Nil:return literal(n);
    case N::Name:{auto& symbol=lookup(n.text,n.span);if(!symbol.constant)error(n,"const initializer requires only compile-time constants","E3007");return *symbol.constant;}
    case N::Unary:return numeric_literal(n)?literal(n):unary_value(n.text,constant(*n.children[0]),n.span);
    case N::Binary: {
        auto left=constant(*n.children[0]);
        if(n.text=="&&" && !as_bool(left,n.span))return Value(false);
        if(n.text=="||" && as_bool(left,n.span))return Value(true);
        return binary_value(n.text,left,constant(*n.children[1]),n.span);
    }
    default:error(n,"const initializer is not a supported compile-time expression","E3007");
    }
}
void SemanticAnalyzer::statement(const Node& n) {
    switch(n.kind) {
    case N::Block:statements(n);break;
    case N::Let:case N::Var:case N::Const: {
        auto info=expression(*n.children.back());auto declared=n.children.size()==2 ? type_name(*n.children[0]):info.type;
        if(n.children.size()==2){validate_type(*n.children[0]);require(declared,info,*n.children.back());}
        std::optional<Value> folded;
        if(n.kind==N::Const){folded=enforce_type(constant(*n.children.back()),declared,n.span,numeric_literal(*n.children.back()));model_.constants[&n]=*folded;}
        bool writable=n.kind==N::Var;
        if(declared.starts_with('['))writable=writable && info.writable;
        define(n.text,{declared,n.kind==N::Var,writable,folded},n.span);model_.types[&n]=declared;break;
    }
    case N::ExpressionStatement:expression(*n.children[0]);break;
    case N::If:require("bool",expression(*n.children[0]),*n.children[0]);statement(*n.children[1]);if(n.children.size()==3)statement(*n.children[2]);break;
    case N::While:require("bool",expression(*n.children[0]),*n.children[0]);statement(*n.children[1]);break;
    case N::For: {
        auto count=n.children.size()-2;const auto& iterable=*n.children[count];std::string item;
        if(iterable.kind==N::Range){for(const auto& c:iterable.children)require("int",expression(*c),*c);item="int";}
        else {auto info=expression(iterable);item=element_type(info.type);if(!info.type.empty() && !info.type.starts_with('['))error(iterable,"for requires a range or slice","E3004");}
        scopes_.emplace_back();
        for(std::size_t i=0;i<count;++i)define(n.children[i]->text,{count==2 && i==0 ? "int":item,false,false,{}},n.children[i]->span);
        statements(*n.children.back(),false);scopes_.pop_back();break;
    }
    case N::Return: {
        const Node* returns=nullptr;for(const auto& c:current_function_->children)if(c->kind==N::ReturnTypes)returns=c.get();
        bool void_return=returns && returns->children.size()==1 && type_name(*returns->children[0])=="void";
        if(returns && (void_return ? 0:returns->children.size())!=n.children.size())error(n,"return value count does not match function signature","E3006");
        for(std::size_t i=0;i<n.children.size();++i){auto info=expression(*n.children[i]);if(returns)require(type_name(*returns->children[i]),info,*n.children[i]);}break;
    }
    case N::ExternalInit:if(!model_.external || !model_.external->module_exists(n.text))error(n,"unregistered external initializer","E7003");break;
    case N::Break:case N::Continue:break;
    case N::Unsafe:statement(*n.children[0]);break;
    case N::Import:{auto name=n.children.empty()?n.text.substr(0,n.text.find('.')):n.children[0]->text;define(name,{"module",false,false,{}},n.span);break;}
    case N::Function:case N::Struct:error(n,"nested declarations are not implemented in Phase 2","E3008");
    default:error(n,"unexpected statement in semantic analysis","E3008");
    }
}
SemanticAnalyzer::Info SemanticAnalyzer::target(const Node& n) {
    if(n.kind==N::Name) {auto& s=lookup(n.text,n.span);if(!s.mutable_binding)error(n,"cannot assign to immutable binding `"+n.text+"`","E3003");return {s.type,true};}
    auto info=expression(n);
    if(!info.writable)error(n,"cannot mutate a read-only value/view","E3003");return info;
}
SemanticAnalyzer::Info SemanticAnalyzer::expression(const Node& n) {
    Info out;
    switch(n.kind) {
    case N::Integer:out={"int",false};break;case N::Float:out={"float",false};break;
    case N::String:out={"string",false};break;case N::Boolean:out={"bool",false};break;case N::Nil:out={"nil",false};break;
    case N::Name:{auto& s=lookup(n.text,n.span);out={s.type,s.writable};break;}
    case N::Array: {
        std::string element;
        for(const auto& c:n.children){auto x=expression(*c);if(element.empty())element=x.type;else require(element,x,*c);}
        out={"[]"+element,true};break;
    }
    case N::Unary: {
        auto x=expression(*n.children[0]);
        if(n.text=="&"){out={"ref<"+x.type+">",false};break;}
        if(n.text=="!"){require("bool",x,n);out={"bool",false};break;}
        if(n.text=="~")require("int",x,n);
        else if(!x.type.empty() && x.type!="int" && x.type!="float")error(n,"unary operator requires numeric operand","E3004");
        out={x.type,false};break;
    }
    case N::Binary: {
        auto a=expression(*n.children[0]),b=expression(*n.children[1]);auto op=n.text;
        if(op=="&&" || op=="||"){require("bool",a,n);require("bool",b,n);out={"bool",false};break;}
        if(op=="==" || op=="!=") {
            if(a.type!="nil" && b.type!="nil")require(a.type,b,n);
            out={"bool",false};break;
        }
        if(op=="**" && a.type=="float" && b.type=="int") {out={"float",false};break;}
        require(a.type,b,n);
        if(op=="<" || op=="<=" || op==">" || op==">=") {
            if(!a.type.empty() && a.type!="int" && a.type!="float" && a.type!="string")error(n,"comparison requires scalar operands","E3004");out={"bool",false};break;
        }
        if(op=="&" || op=="|" || op=="^" || op=="<<" || op==">>" || op=="%" || op=="//")require("int",a,n);
        else if(!(op=="+" && a.type=="string") && !a.type.empty() && a.type!="int" && a.type!="float")error(n,"arithmetic requires numeric operands","E3004");
        out={op=="/" ? "float":a.type,false};break;
    }
    case N::Assignment:case N::Update: {
        auto lhs=target(*n.children[0]);
        if(n.kind==N::Assignment){auto rhs=expression(*n.children[1]);require(lhs.type,rhs,*n.children[1]);
            if(n.text!="=" && !lhs.type.empty() && lhs.type!="int" && lhs.type!="float" && !(n.text=="+=" && lhs.type=="string"))error(n,"compound operator is not defined for this type","E3004");
            if((n.text=="%=" || n.text=="//=") && !lhs.type.empty() && lhs.type!="int")error(n,"integer compound operator requires int","E3004");
            if(n.text=="/=" && lhs.type=="int")error(n,"/= produces float; use a float binding or explicit conversion","E3004");
            if(n.children[0]->kind==N::Name && lhs.type.starts_with('['))lookup(n.children[0]->text,n.span).writable=rhs.writable;
        } else require("int",lhs,n);
        out={lhs.type,false};break;
    }
    case N::Index:case N::Slice: {
        auto base=expression(*n.children[0]);auto element=element_type(base.type);
        if(!base.type.empty() && !base.type.starts_with('['))error(n,"index/slice requires a slice or array","E3004");
        for(std::size_t i=1;i<n.children.size();++i)if(n.children[i]->kind!=N::Omitted)require("int",expression(*n.children[i]),*n.children[i]);
        out={n.kind==N::Slice ? "[]"+element:element,base.writable};break;
    }
    case N::StructLiteral: {
        auto found=model_.structures.find(n.text);if(found==model_.structures.end())error(n,"unknown struct `"+n.text+"`","E3004");
        std::unordered_set<std::string> seen;
        for(const auto& init:n.children) {
            if(!seen.insert(init->text).second)error(*init,"duplicate field initializer","E3002");
            const Node* field=nullptr;for(const auto& f:found->second->children)if(f->text==init->text)field=f.get();
            if(!field)error(*init,"unknown field `"+init->text+"`","E3004");
            require(type_name(*field->children[0]),expression(*init->children[0]),*init->children[0]);
        }
        if(seen.size()!=found->second->children.size())error(n,"all struct fields require explicit initialization","E3004");out={n.text,true};break;
    }
    case N::Member: {
        auto base=expression(*n.children[0]);auto type=unref(base.type);
        if(n.text=="len" && (type.starts_with('[') || type=="string")){out={"int",false};break;}
        if(type.empty() || type=="module" || type.starts_with("ptr<")){out={"",false};break;}
        auto structure=model_.structures.find(type);
        if(structure==model_.structures.end())error(n,"member access requires a struct","E3004");
        bool found=false;
        for(const auto& field:structure->second->children)if(field->text==n.text){out={type_name(*field->children[0]),base.writable};found=true;break;}
        if(!found){auto method=type+"."+n.text;if(!model_.functions.contains(method))error(n,"unknown field/method `"+n.text+"`","E3004");
            auto definition=model_.functions.at(method);
            if(definition->span.file_id!=n.span.file_id && definition->text.find(" pub")==std::string::npos)
                error(n,"method is private to its defining module","E5003");
            if(model_.mutating[method] && !base.writable)error(n,"mutating method requires a mutable receiver","E3003");out={"fn:"+method,false};}
        break;
    }
    case N::Call: {
        auto callee=expression(*n.children[0]);std::vector<Info> args;
        for(std::size_t i=1;i<n.children.size();++i)args.push_back(expression(*n.children[i]));
        if(callee.type.starts_with("fn:")) {
            auto key=callee.type.substr(3);auto fn=model_.functions.at(key);auto ps=parameters(*fn);
            if(ps.size()!=args.size())error(n,"function argument count mismatch","E3005");
            for(std::size_t i=0;i<args.size();++i) {
                if(!ps[i]->children.empty())require(type_name(*ps[i]->children[0]),args[i],*n.children[i+1]);
                if(mutable_parameter(*ps[i]) && !args[i].writable)error(*n.children[i+1],"mut parameter requires mutable argument","E3003");
            }
            out={result_type(*fn),false};
        } else if(callee.type.starts_with("builtin:")) {
            auto name=callee.type.substr(8);
            const std::size_t arity=(name=="min"||name=="max")?2:name=="clamp"?3:1;
            if(name!="print" && args.size()!=arity)error(n,"builtin argument count mismatch","E3005");
            if(!args.empty() && !args[0].type.empty()) {
                const auto& t=args[0].type;
                if((name=="sqrt" || name=="abs") && t!="int" && t!="float")error(n,"numeric builtin requires int or float","E3004");
                if(name=="len" && t!="string" && !t.starts_with('['))error(n,"len requires a slice or string","E3004");
                if((name=="int" || name=="float") && t!="int" && t!="float" && t!="string")error(n,"conversion requires int, float or string","E3004");
                if((name=="min" || name=="max" || name=="clamp") && t!="int" && t!="float" && t!="string")error(n,"comparison builtin requires matching scalar types","E3004");
            }
            if(name=="print")out={"void",false};else if(name=="str" || name=="type")out={"string",false};
            else if(name=="int" || name=="len")out={"int",false};else if(name=="float" || name=="sqrt")out={"float",false};
            else if(name=="clone")out={args[0].type,true};else out={args[0].type,false};
            if(name=="min" || name=="max" || name=="clamp")for(std::size_t i=1;i<args.size();++i)require(args[0].type,args[i],*n.children[i+1]);
        } else if(!callee.type.empty())error(n,"value is not callable","E3004");
        break;
    }
    default:error(n,"expression is not supported by semantic analysis","E3008");
    }
    model_.types[&n]=out.type;return out;
}
}
