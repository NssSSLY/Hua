#include "hua/ir.hpp"
#include "hua/runtime.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
namespace hua::ir {
namespace {
using N=NodeKind; using H=HirKind;
constexpr std::size_t limit=250000;
[[noreturn]] void unsupported(const Node& n,const std::string& why) {
    throw Diagnostic("E8001",n.span,"E1 IR unsupported: "+why);
}
TypeId scalar(const std::string& name,const Node& n) {
    if(name=="int")return int_type;
    if(name=="float")return float_type;
    if(name=="bool")return bool_type;
    if(name=="void"||name.empty())return void_type;
    unsupported(n,"type "+name);
}
std::string returned(const Node& fn) {
    for(const auto& c:fn.children)if(c->kind==N::ReturnTypes) {
        if(c->children.size()!=1)unsupported(fn,"multiple returns");
        return type_name(*c->children.front());
    }
    return "void";
}
std::vector<const Node*> parameters(const Node& fn){std::vector<const Node*> out;for(const auto& c:fn.children)if(c->kind==N::Parameter)out.push_back(c.get());return out;}
bool mutable_parameter(const Node& p){return p.text.find(" mut")!=std::string::npos||(!p.children.empty()&&p.children[0]->kind==N::MutableType);}
bool numeric(TypeId t){return t==int_type||t==float_type;}
class Builder {
    const Node& root; const SemanticModel& model;
    HirModule out;
    std::vector<std::map<std::string,SymbolId>> scopes;
    std::map<const Node*,SymbolId> declarations;
    std::vector<const Node*> functions;
    FunctionId owner{1};
    SymbolId add_symbol(std::string name,TypeId t,SymbolKind k,const SourceSpan& span,
                        bool writable=false,FunctionId function={}) {
        if(out.tables.symbols.size()>=limit)invalid(span,"symbol limit");
        SymbolId id{static_cast<std::uint32_t>(out.tables.symbols.size()+1)};
        out.tables.symbols.push_back({id,std::move(name),t,k,owner,function,writable,span});
        return id;
    }
    HirNodeId add(HirNode n) {
        if(out.nodes.size()>=limit)invalid(n.span,"HIR node limit");
        n.id={static_cast<std::uint32_t>(out.nodes.size()+1)};
        n.source={1};
        for(auto c:n.children)n.effects|=out.nodes.at(c.value-1).effects;
        out.nodes.push_back(std::move(n));return out.nodes.back().id;
    }
    SymbolId lookup(const Node& n) {
        for(auto i=scopes.rbegin();i!=scopes.rend();++i)
            if(auto f=i->find(n.text);f!=i->end())return f->second;
        unsupported(n,"unresolved/dynamic name "+n.text);
    }
    TypeId checked_type(const Node& n) {
        auto f=model.types.find(&n);
        if(f==model.types.end())unsupported(n,"missing static type");
        if(f->second.empty())unsupported(n,"dynamic value needs the existing engine");
        return scalar(f->second,n);
    }
    HirNodeId expression(const Node& n) {
        HirNode h{};h.span=n.span;h.contextual=contextual_literal(n);
        if(numeric_literal(n)||n.kind==N::Boolean) {
            h.kind=H::Constant;h.type=checked_type(n);
            try{h.constant=unbox_constant(literal(n),n.span);}catch(const Diagnostic& d){
                h.failure_code=d.code();h.text=d.what();h.failure_message=d.what();h.effects=MayFail;
            }
            return add(std::move(h));
        }
        switch(n.kind) {
        case N::Name:
            h.kind=H::Load;h.symbol=lookup(n);h.type=symbol(out.tables,h.symbol).type;
            h.effects=Read|MayFail;break;
        case N::Unary:
            if(n.text=="&")unsupported(n,"references");
            h.kind=H::Unary;h.text=n.text;h.type=checked_type(n);h.effects=MayFail;
            h.children={expression(*n.children[0])};break;
        case N::Binary:
            h.kind=H::Binary;h.text=n.text;h.type=checked_type(n);h.effects=MayFail;
            for(const auto& c:n.children)h.children.push_back(expression(*c));break;
        case N::Assignment:case N::Update:
            if(n.children[0]->kind!=N::Name)unsupported(n,"non-name assignment");
            h.kind=H::Store;h.control_span=n.children[0]->span;h.symbol=lookup(*n.children[0]);h.type=symbol(out.tables,h.symbol).type;
            h.text=n.kind==N::Update?(n.text=="++"?"+=":"-="):n.text;h.effects=Read|Write|MayFail;
            if(n.kind==N::Update){HirNode one{};one.kind=H::Constant;one.span=n.span;
                one.type=int_type;one.constant=ScalarConstant(std::int64_t{1});one.contextual=true;
                h.children={add(std::move(one))};h.flag=true;
            }else {h.children={expression(*n.children[1])};h.flag=contextual_literal(*n.children[1]);}
            break;
        case N::Call: {
            if(n.children[0]->kind!=N::Name)unsupported(n,"indirect call");
            h.kind=H::Call;h.symbol=lookup(*n.children[0]);const auto& sig=type(out.tables,symbol(out.tables,h.symbol).type);
            if(sig.kind!=TypeKind::Callable)unsupported(n,"dynamic call");
            h.type=sig.result;h.text=model.types.at(&n);h.effects=MayFail|Allocate;
            if(sig.helper==Helper::Print)h.effects|=Io;
            for(const auto& c:n.children)h.children.push_back(expression(*c));
            for(std::size_t i=1;i<h.children.size();++i) {
                auto t=out.nodes.at(h.children[i].value-1).type;
                if(sig.helper==Helper::Print ? !numeric(t)&&t!=bool_type : sig.helper!=Helper::None&&!numeric(t))
                    unsupported(n,"helper argument outside scalar subset");
            }
            break;
        }
        default:unsupported(n,std::string(node_name(n.kind)));
        }
        return add(std::move(h));
    }
    HirNodeId block(const Node& n,bool scope) {
        if(scope)scopes.emplace_back();
        HirNode h{};h.kind=H::Block;h.span=n.span;h.flag=scope;if(scope)h.effects=Allocate|MayFail;
        for(const auto& c:n.children)if(c->kind!=N::Function)h.children.push_back(statement(*c));
        if(scope)scopes.pop_back();
        return add(std::move(h));
    }
    HirNodeId statement(const Node& n) {
        HirNode h{};h.span=n.span;
        switch(n.kind) {
        case N::Block:return block(n,true);
        case N::Let:case N::Var:case N::Const: {
            h.kind=H::Bind;h.type=checked_type(n);h.flag=n.kind==N::Var;h.effects=Write|MayFail;
            if(h.type==void_type)unsupported(n,"void binding");
            if(n.kind==N::Const) {
                HirNode c{};c.kind=H::Constant;c.span=n.span;c.type=h.type;
                c.constant=unbox_constant(model.constants.at(&n),n.span);c.contextual=contextual_literal(*n.children.back());h.children={add(std::move(c))};
            }else h.children={expression(*n.children.back())};
            if(owner.value==1&&scopes.size()==1)h.symbol=declarations.at(&n);
            else {h.symbol=add_symbol(n.text,h.type,SymbolKind::Local,n.span,h.flag);scopes.back()[n.text]=h.symbol;}
            return add(std::move(h));
        }
        case N::ExpressionStatement:h.kind=H::Expression;h.children={expression(*n.children[0])};break;
        case N::If:case N::While:
            h.kind=n.kind==N::If?H::If:H::While;h.children.push_back(expression(*n.children[0]));
            for(std::size_t i=1;i<n.children.size();++i)h.children.push_back(statement(*n.children[i]));
            break;
        case N::For: {
            auto count=n.children.size()-2;const auto& range=*n.children[count];
            if(range.kind!=N::Range)unsupported(n,"non-range iteration");
            h.kind=H::For;h.flag=range.text=="..=";h.effects=Read|Write|Allocate|MayFail;
            for(const auto& c:range.children)h.children.push_back(expression(*c));
            if(range.children.size()==2){HirNode one{};one.kind=H::Constant;one.span=n.span;
                one.type=int_type;one.constant=ScalarConstant(std::int64_t{1});h.children.push_back(add(std::move(one)));}
            scopes.emplace_back();
            for(std::size_t i=0;i<count;++i){auto id=add_symbol(n.children[i]->text,int_type,SymbolKind::Local,n.children[i]->span);
                h.names.push_back(id);scopes.back()[n.children[i]->text]=id;}
            h.children.push_back(block(*n.children.back(),false));scopes.pop_back();
            h.control_span=range.span;
            break;
        }
        case N::Return:
            h.kind=H::Return;
            if(!n.children.empty()&&out.functions.at(owner.value-1).info.result==void_type)unsupported(n,"value return requires scalar annotation");
            for(const auto& c:n.children)h.children.push_back(expression(*c));break;
        case N::Break:h.kind=H::Break;break;
        case N::Continue:h.kind=H::Continue;break;
        default:unsupported(n,std::string(node_name(n.kind)));
        }
        return add(std::move(h));
    }
public:
    Builder(const Node& r,const SemanticModel& m,const std::vector<const Source*>& sources):root(r),model(m) {
        if(sources.size()>1)unsupported(r,"multiple modules");
        if(!m.structures.empty())unsupported(r,"struct/interface/enum");
        std::size_t bytes=r.span.end_offset;
        std::function<void(const Node&,std::size_t)> scan=[&](const Node& n,std::size_t depth){
            if(depth>384)unsupported(n,"nesting exceeds E1 limit (384)");
            if(n.span.file_id!=r.span.file_id)unsupported(n,"multiple source identities");
            bytes=std::max(bytes,n.span.end_offset);for(const auto& c:n.children)scan(*c,depth+1);
        };scan(r,0);if(!sources.empty())bytes=sources[0]->text().size();
        out.tables.sources.push_back({{1},r.span.file_id,bytes});
        out.tables.modules.push_back({{1},{1},r.span.file_id});
        out.tables.types={{{1},TypeKind::Void,"void",{},{}},{{2},TypeKind::Int,"int",{},{}},
            {{3},TypeKind::Float,"float",{},{}},{{4},TypeKind::Bool,"bool",{},{}},
            {{5},TypeKind::Callable,"$core$print",{},void_type,Helper::Print,true},
            {{6},TypeKind::Callable,"$core$int",{},int_type,Helper::Int},
            {{7},TypeKind::Callable,"$core$float",{},float_type,Helper::Float}};
        scopes.emplace_back();
        for(auto [name,t]:std::vector<std::pair<std::string,TypeId>>{{"print",{5}},{"int",{6}},{"float",{7}}}) {
            auto id=add_symbol("$core$"+name,t,SymbolKind::Helper,r.span);
            scopes[0][name]=id;scopes[0]["$core$"+name]=id;
        }
        out.functions.push_back({{{1},{},"<initialize>",r.span,{},void_type,0,true},{}});
        for(const auto& c:r.children)if(c->kind==N::Function) {
            auto name=declared_name(*c);
            if(c->text.find(" unsafe")!=std::string::npos||c->text.find(" external")!=std::string::npos||
               c->text.find(" abstract")!=std::string::npos||name.find('.')!=std::string::npos||name.starts_with("$generic"))
                unsupported(*c,"nonordinary function");
            std::vector<TypeId> args;
            for(const auto* p:parameters(*c)){if(p->children.empty()||mutable_parameter(*p))unsupported(*p,"untyped/mut parameter");
                args.push_back(scalar(type_name(*p->children[0]),*p));if(args.back()==void_type)unsupported(*p,"void parameter");}
            auto result=scalar(returned(*c),*c);TypeId t{static_cast<std::uint32_t>(out.tables.types.size()+1)};
            out.tables.types.push_back({t,TypeKind::Callable,name,args,result});
            FunctionId f{static_cast<std::uint32_t>(out.functions.size()+1)};
            auto id=add_symbol(name,t,SymbolKind::Function,c->span,false,f);
            scopes[0][name]=id;out.functions.push_back({{f,id,name,c->span,{},result},{}});
            functions.push_back(c.get());
        }
        if(functions.size()!=m.functions.size())unsupported(r,"nested/generated unsupported function metadata");
        for(const auto& c:r.children)if(c->kind==N::Let||c->kind==N::Var||c->kind==N::Const) {
            auto t=checked_type(*c);if(t==void_type)unsupported(*c,"void binding");
            auto id=add_symbol(c->text,t,SymbolKind::Global,c->span,c->kind==N::Var);
            scopes[0][c->text]=id;declarations[c.get()]=id;
        }
    }
    HirModule build() {
        out.functions[0].body=block(root,false);
        for(std::size_t i=0;i<functions.size();++i) {
            owner={static_cast<std::uint32_t>(i+2)};scopes.emplace_back();
            auto ps=parameters(*functions[i]);const auto& signature=type(out.tables,symbol(out.tables,out.functions[i+1].info.symbol).type);
            for(std::size_t j=0;j<ps.size();++j) {
                auto id=add_symbol(declared_name(*ps[j]),signature.parameters[j],SymbolKind::Parameter,ps[j]->span);
                scopes.back()[declared_name(*ps[j])]=id;out.functions[i+1].info.parameters.push_back(id);
            }
            out.functions[i+1].body=block(*functions[i]->children.back(),false);scopes.pop_back();
        }
        // Monotone fixed point: recursive calls preserve their transitive effects.
        bool changed=true;while(changed){changed=false;
            for(auto& n:out.nodes){auto before=n.effects;
                for(auto c:n.children)n.effects|=out.nodes[c.value-1].effects;
                if(n.kind==H::Call){const auto& s=symbol(out.tables,n.symbol);
                    if(s.function)n.effects|=out.functions.at(s.function.value-1).info.effects;}
                changed|=before!=n.effects;
            }
            for(auto& f:out.functions){auto before=f.info.effects;f.info.effects|=MayFail|out.nodes[f.body.value-1].effects;changed|=before!=f.info.effects;}
        }
        verify_hir(out);return std::move(out);
    }
};
}

ScalarConstant unbox_constant(const Value& value,const SourceSpan& s){
    return std::visit([&](const auto& x)->ScalarConstant{
        using V=std::decay_t<decltype(x)>;
        if constexpr(std::same_as<V,std::monostate>)return {};
        else if constexpr(std::same_as<V,bool>||std::same_as<V,std::int64_t>||std::same_as<V,double>)return ScalarConstant(x);
        else invalid(s,"non-scalar IR constant");
    },value.data);
}
Value box_constant(const ScalarConstant& value){
    return std::visit([](const auto& x)->Value{
        if constexpr(std::same_as<std::decay_t<decltype(x)>,std::monostate>)return {};
        else return Value(x);
    },value.data);
}

[[noreturn]] void invalid(const SourceSpan& s,const std::string& why){throw Diagnostic("E8002",s,"invalid E1 IR: "+why);}
const Type& type(const Tables& t,TypeId id){if(!id||id.value>t.types.size())invalid({},"type ID");return t.types[id.value-1];}
const Symbol& symbol(const Tables& t,SymbolId id){if(!id||id.value>t.symbols.size())invalid({},"symbol ID");return t.symbols[id.value-1];}
HirModule build_hir(const Node& root,const SemanticModel& model,const std::vector<const Source*>& sources){return Builder(root,model,sources).build();}
std::string dump_hir(const HirModule& m) {
    verify_hir(m);std::ostringstream o;o<<"hua.hir 1\n";
    for(const auto& s:m.tables.sources)o<<"source s"<<s.id.value<<' '<<std::quoted(s.filename)<<'\n';
    for(const auto& x:m.tables.modules)o<<"module m"<<x.id.value<<" source s"<<x.source.value<<'\n';
    for(const auto& t:m.tables.types)o<<"type t"<<t.id.value<<' '<<std::quoted(t.name)<<'\n';
    for(const auto& s:m.tables.symbols)o<<"symbol y"<<s.id.value<<" t"<<s.type.value<<' '<<std::quoted(s.name)<<" owner f"<<s.owner.value<<" mutable="<<s.mutable_binding<<'\n';
    for(const auto& f:m.functions)o<<"function f"<<f.info.id.value<<' '<<std::quoted(f.info.name)<<" -> t"<<f.info.result.value<<" body h"<<f.body.value<<" effects="<<f.info.effects<<'\n';
    static const char* names[]={"block","constant","load","unary","binary","store","call","bind","expression","if","while","for","return","break","continue"};
    for(const auto& n:m.nodes){o<<"h"<<n.id.value<<' '<<names[static_cast<unsigned>(n.kind)]<<" t"<<n.type.value<<" s"<<n.source.value<<" y"<<n.symbol.value<<' '<<std::quoted(n.text);
        if(n.kind==H::Constant)o<<" value="<<std::quoted(show(box_constant(n.constant)))<<" failure="<<n.failure_code;
        for(auto c:n.children)o<<" h"<<c.value;o<<" effects="<<n.effects<<" @"<<n.span.line<<':'<<n.span.column<<'\n';}
    return o.str();
}
}
