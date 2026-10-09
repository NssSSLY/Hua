#include "hua/sema.hpp"
#include "hua/runtime.hpp"
#include <algorithm>
#include <functional>
#include <unordered_set>
namespace hua {
namespace {
using N=NodeKind;
[[noreturn]] void error(const Node& n,std::string message,std::string code="E3001") { throw Diagnostic(std::move(code),n.span,std::move(message)); }
std::string result_type(const Node& fn) {
    for(const auto& c:fn.children) if(c->kind==N::ReturnTypes){if(c->children.size()==1)return type_name(*c->children[0]);std::string out="multi<";for(std::size_t i=0;i<c->children.size();++i){if(i)out+=',';out+=type_name(*c->children[i]);}return out+">";}
    return "";
}
std::vector<const Node*> parameters(const Node& fn) {
    std::vector<const Node*> out; for(const auto& c:fn.children) if(c->kind==N::Parameter)out.push_back(c.get());return out;
}
bool mutable_parameter(const Node& p) { return p.text.find(" mut")!=std::string::npos || (!p.children.empty() && p.children[0]->kind==N::MutableType); }
bool definitely_returns(const Node& n) {
    if(n.kind==N::Return)return true;
    if(n.kind==N::Match){for(std::size_t i=1;i<n.children.size();++i)if(!definitely_returns(*n.children[i]->children[2]))return false;return n.children.size()>1;}
    if(n.kind==N::If)return n.children.size()==3 && definitely_returns(*n.children[1]) && definitely_returns(*n.children[2]);
    if(n.kind==N::Block) for(const auto& c:n.children)if(definitely_returns(*c))return true;
    return false;
}
bool flow_stops(const Node& n){if(n.kind==N::Return||n.kind==N::Break||n.kind==N::Continue)return true;if(n.kind==N::If)return n.children.size()==3&&flow_stops(*n.children[1])&&flow_stops(*n.children[2]);if(n.kind==N::Match){for(std::size_t i=1;i<n.children.size();++i)if(!flow_stops(*n.children[i]->children[2]))return false;return n.children.size()>1;}if(n.kind==N::Block)for(const auto& c:n.children)if(flow_stops(*c))return true;return false;}
const Node& root_target(const Node& n) {
    if((n.kind==N::Member || n.kind==N::Index || n.kind==N::Slice) && !n.children.empty())return root_target(*n.children[0]);return n;
}
std::string element_type(std::string type) {
    if(type.starts_with('[')) {auto end=type.find(']'); if(end!=std::string::npos)return type.substr(end+1);}return "";
}
std::string unref(std::string type) {
    if(type.starts_with("ref<") && type.ends_with('>'))return type.substr(4,type.size()-5);return type;
}

}
TypeRelations interface_relations(const SemanticModel& model){
    TypeRelations result;
    auto signature=[](const Node& fn){std::string out;for(const auto& c:fn.children)if(c->kind==N::Parameter)out+=(mutable_parameter(*c)?"mut:":"")+ (c->children.empty()?"":type_name(*c->children[0]))+";";auto r=result_type(fn);return out+"->"+(r.empty()?"void":r);};
    for(const auto& [name,interface]:model.structures)if(interface->text.find(" interface")!=std::string::npos){auto& conforms=result[name];
        for(const auto& [actual,structure]:model.structures){if(actual==name)continue;bool valid=true;
            for(const auto& [method,prototype]:model.functions)if(method.starts_with(name+".")){auto concrete=actual+method.substr(name.size());auto found=model.functions.find(concrete);bool expected_mut=model.mutating.contains(method)&&model.mutating.at(method),actual_mut=model.mutating.contains(concrete)&&model.mutating.at(concrete);if(found==model.functions.end()||signature(*prototype)!=signature(*found->second)||(!expected_mut&&actual_mut)||(prototype->span.file_id!=found->second->span.file_id&&found->second->text.find(" pub")==std::string::npos)){valid=false;break;}}
            if(valid)conforms.insert(actual);
        }
    }
    return result;
}
SemanticAnalyzer::Symbol& SemanticAnalyzer::lookup(const std::string& name,const SourceSpan& span) {
    for(auto i=scopes_.rbegin();i!=scopes_.rend();++i)if(auto found=i->find(name);found!=i->end())return found->second;
    throw Diagnostic("E3001",span,"undefined name `"+name+"`");
}
void SemanticAnalyzer::define(const std::string& name,Symbol symbol,const SourceSpan& span) {
    if(symbol.declared.empty())symbol.declared=symbol.type;
    if(!scopes_.back().emplace(name,std::move(symbol)).second)throw Diagnostic("E3002",span,"duplicate declaration `"+name+"` in this scope");
}
void SemanticAnalyzer::validate_type(const Node& n) {
    static const std::unordered_set<std::string> types={"int","float","bool","string","byte","i8","i16","i32","i64","u8","u16","u32","u64","f32","f64","usize","isize","void","Result","std.error.Value","Json","Bytes","Buffer","Task"};
    if(n.kind==N::TypeName&&n.text.empty())return;
    if(n.kind==N::TypeName && !types.contains(n.text) && !model_.structures.contains(n.text)) {
        if(n.text.find('.')==std::string::npos)error(n,"unknown type `"+n.text+"`","E3004");
        auto prefix=n.text.substr(0,n.text.find('.'));lookup(prefix,n.span);
    }
    if(n.kind==N::TypeName&&(n.text=="List"||n.text=="Task"))error(n,"List requires an element type","E3004");
    if(n.kind==N::GenericType&&n.text=="List"&&type_name(*n.children[0])=="void")error(n,"List elements cannot be void","E3004");
    if(n.kind==N::GenericType && n.text=="map") {
        auto key=type_name(*n.children[0]);if(key!="bool"&&key!="int"&&key!="string")error(n,"map keys require bool/int/string","E3004");
        if(type_name(*n.children[1])=="void")error(n,"map values cannot be void","E3004");
        if(type_name(*n.children[1]).starts_with("Result<"))error(n,"map Result values require unsupported Optional<Result> lookup","E3004");
    }
    if(n.kind==N::GenericType && n.text=="Result"&&n.children.size()>1&&type_name(*n.children[1])=="void")error(n,"Result error type cannot be void","E3004");
    if(n.kind==N::OptionalType && type_name(*n.children[0]).starts_with("Result<"))error(n,"Optional<Result> is not supported; use Result<T?>","E3004");
    for(const auto& c:n.children)validate_type(*c);
}
void SemanticAnalyzer::require(const std::string& expected,const Info& actual,const Node& n) {
    if(numeric_spec(expected)&&numeric_spec(actual.type)&&numeric_literal(n)&&(numeric_spec(expected)->category=='f'||numeric_spec(actual.type)->category!='f'))return;
    if(expected.starts_with('[')&&n.kind==N::Array){auto close=expected.find(']');if(close==std::string::npos)error(n,"invalid array type","E3004");auto element=expected.substr(close+1);for(const auto& c:n.children)require(element,{model_.types.at(c.get()),model_.writable.at(c.get())},*c);return;}
    if(expected=="float" && actual.type=="int" && numeric_literal(n))return;
    auto es=type_arguments(expected,"Result"),as=type_arguments(actual.type,"Result");
    if(as.size()==2&&as[0]=="void"&&as[1].empty()&&n.kind==N::Call&&n.children.size()==1){if(es.size()!=2||es[0]!="void"||es[1].empty())error(n,"ok() requires an explicit Result<void,E> context","E3004");model_.types[&n]=expected;return;}
    if(actual.type=="void"&&expected!="void")error(n,"void has no usable value","E3004");
    if(es.size()==2&&as.size()==2&&n.kind==N::Call&&n.children.size()==2&&contextual_literal(*n.children[1])) {
        auto ct=model_.types.find(n.children[0].get());if(ct!=model_.types.end()&&(ct->second=="builtin:ok"||ct->second=="builtin:err")){
            auto side=ct->second=="builtin:ok"?0u:1u;if(compatible(es[1-side],as[1-side])){require(es[side],{as[side],actual.writable},*n.children[1]);return;}
        }
    }
    if(!compatible(expected,actual.type))error(n,"expected "+expected+", got "+actual.type,"E3004");
}
void SemanticAnalyzer::infer_mutating_methods() {
    std::function<bool(const Node&,const std::string&)> direct=[&](const Node& n,const std::string& owner) {
        if((n.kind==N::Assignment || n.kind==N::Update) && root_target(*n.children[0]).kind==N::Name && root_target(*n.children[0]).text=="self")return true;
        if(n.kind==N::MultiAssignment)for(const auto& t:n.children[0]->children)if(root_target(*t).kind==N::Name&&root_target(*t).text=="self")return true;
        if(n.kind==N::Call&&n.children.size()==3&&n.children[0]->kind==N::Name&&(n.children[0]->text=="delete"||n.children[0]->text=="$core$delete")&&root_target(*n.children[1]).kind==N::Name&&root_target(*n.children[1]).text=="self")return true;
        if(n.kind==N::Call&&n.children.size()>1&&n.children[0]->kind==N::Name){auto name=n.children[0]->text;if(name.starts_with("$core$"))name=name.substr(6);if(auto f=standard_function(name);f&&f->mutates_first&&root_target(*n.children[1]).kind==N::Name&&root_target(*n.children[1]).text=="self")return true;}
        if(n.kind==N::Call&&n.children[0]->kind==N::Name){auto found=model_.functions.find(n.children[0]->text);if(found!=model_.functions.end()){auto ps=parameters(*found->second);for(std::size_t i=0;i<ps.size()&&i+1<n.children.size();++i)if(mutable_parameter(*ps[i])&&root_target(*n.children[i+1]).kind==N::Name&&root_target(*n.children[i+1]).text=="self")return true;}}
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
    for(const auto& [name,fn]:model_.functions)if(fn->text.find(" mutreceiver")!=std::string::npos)model_.mutating[name]=true;
    bool changed=true;
    while(changed) {
        changed=false;
        for(const auto& [name,fn]:model_.functions)if(auto dot=name.find('.');dot!=std::string::npos)
            if(!model_.mutating[name] && direct(*fn->children.back(),name.substr(0,dot))){model_.mutating[name]=true;changed=true;}
    }
}
void SemanticAnalyzer::infer_return_capabilities(){
    for(const auto& [name,fn]:model_.functions)model_.return_writable[name]=fn->text.find(" abstract")==std::string::npos&&fn->text.find(" external")==std::string::npos;
    bool changed=true;std::size_t passes=0;
    while(changed&&passes++<=model_.functions.size()+1){changed=false;for(const auto& [name,fn]:model_.functions){
        std::map<std::string,bool> values;std::map<std::string,std::string> value_types;for(const auto* p:parameters(*fn)){values[declared_name(*p)]=mutable_parameter(*p);value_types[declared_name(*p)]=p->children.empty()?"":type_name(*p->children[0]);}values["self"]=model_.mutating[name];if(auto dot=name.find('.');dot!=std::string::npos)value_types["self"]=name.substr(0,dot);
        bool has_return=false,all=true;
        std::function<bool(const Node&)> capability=[&](const Node& n)->bool{
            if(n.kind==N::Array||n.kind==N::MapLiteral||n.kind==N::StructLiteral)return true;
            if(n.kind==N::Name){auto found=values.find(n.text);return found!=values.end()&&found->second;}
            if(n.kind==N::Member||n.kind==N::Slice||n.kind==N::Index||n.kind==N::Propagate)return capability(*n.children[0]);
            if(n.kind==N::Pack){for(const auto& c:n.children)if(!capability(*c))return false;return true;}
            if(n.kind==N::Call){auto key=n.children[0]->kind==N::Name?n.children[0]->text:"";if(n.children[0]->kind==N::Member&&n.children[0]->children[0]->kind==N::Name){auto receiver=value_types.find(n.children[0]->children[0]->text);if(receiver!=value_types.end())key=receiver->second+"."+n.children[0]->text;}if(key.starts_with("$core$"))key=key.substr(6);if(key=="clone")return true;if((key=="ok"||key=="err"||key=="unwrap"||key=="unwrap_err")&&n.children.size()>1)return capability(*n.children[1]);if(key=="unwrap_or"&&n.children.size()==3)return capability(*n.children[1])&&capability(*n.children[2]);if(auto f=standard_function(key))return f->writable_result;auto found=model_.return_writable.find(key);return found!=model_.return_writable.end()&&found->second;}
            return false;
        };
        std::function<void(const Node&)> scan=[&](const Node& n){if(n.kind==N::Function)return;
            if(n.kind==N::Let||n.kind==N::Var||n.kind==N::Const){values[n.text]=n.kind==N::Var&&capability(*n.children.back());if(n.children.size()==2)value_types[n.text]=type_name(*n.children[0]);else if(n.children.back()->kind==N::StructLiteral)value_types[n.text]=n.children.back()->text;return;}
            if(n.kind==N::Assignment&&n.children[0]->kind==N::Name){bool cap=capability(*n.children[1]);auto& old=values[n.children[0]->text];old=old&&cap;return;}
            if(n.kind==N::Return){has_return=true;for(const auto& c:n.children)all=all&&capability(*c);return;}
            // Restore lexical names; merge outer effects conservatively across branches and loops.
            if(n.kind==N::Block){auto before=values;std::set<std::string> shadowed;for(const auto& c:n.children){if((c->kind==N::Var||c->kind==N::Let||c->kind==N::Const)&&before.contains(c->text))shadowed.insert(c->text);scan(*c);}for(auto i=values.begin();i!=values.end();)if(!before.contains(i->first))i=values.erase(i);else{if(shadowed.contains(i->first))i->second=before.at(i->first);++i;}return;}
            if(n.kind==N::If){auto before=values;scan(*n.children[1]);auto yes=values;values=before;if(n.children.size()==3)scan(*n.children[2]);for(auto& [key,cap]:values)cap=cap&&yes[key];return;}
            if(n.kind==N::Match){auto before=values,merged=values;for(std::size_t i=1;i<n.children.size();++i){values=before;scan(*n.children[i]->children[2]);for(auto& [key,cap]:merged)cap=cap&&values[key];}values=merged;return;}
            for(const auto& c:n.children)scan(*c);
        };scan(*fn->children.back());bool cap=has_return&&all&&fn->text.find(" abstract")==std::string::npos;if(cap!=model_.return_writable[name]){model_.return_writable[name]=cap;changed=true;}
    }}
}
SemanticModel SemanticAnalyzer::analyze(const Node& program,std::shared_ptr<ExternalRegistry> external) {
    model_={};model_.external=std::move(external);scopes_.clear();scopes_.emplace_back();current_function_=nullptr;
    for(const auto& name:builtin_names()){define(name,{"builtin:"+name,false,false,{}},program.span);define("$core$"+name,{"builtin:"+name,false,false,{}},program.span);}
    scopes_.emplace_back();
    for(const auto& n:program.children) {
        if(n->kind==N::Function) {
            auto name=declared_name(*n);define(name,{"fn:"+name,false,false,{}},n->span);model_.functions.emplace(name,n.get());
        } else if(n->kind==N::Struct) {
            auto name=declared_name(*n);if(name=="Json"||name=="Bytes"||name=="Buffer"||name=="List")error(*n,"reserved standard value type","E3002");define(name,{"struct:"+name,false,false,{}},n->span);model_.structures.emplace(name,n.get());
        }
    }
    std::function<void(const Node&)> collect_nested=[&](const Node& n){for(const auto& c:n.children){if(c->kind==N::Function){model_.functions.emplace(declared_name(*c),c.get());model_.nested[c.get()]=true;}collect_nested(*c);}};for(const auto& n:program.children)if(n->kind==N::Function)collect_nested(*n);
    for(const auto& [name,structure]:model_.structures) {
        std::unordered_set<std::string> fields;
        for(const auto& f:structure->children){if(!fields.insert(f->text).second)error(*f,"duplicate struct field `"+f->text+"`","E3002");validate_type(*f->children[0]);}
    }
    for(const auto& [name,fn]:model_.functions) {
        if(auto dot=name.find('.');dot!=std::string::npos && !model_.structures.contains(name.substr(0,dot)))error(*fn,"method owner is not a declared struct","E3004");
        for(const auto& p:parameters(*fn))if(!p->children.empty())validate_type(*p->children[0]);
        for(const auto& c:fn->children)if(c->kind==N::ReturnTypes)for(const auto& t:c->children)validate_type(*t);
    }
    infer_mutating_methods();infer_return_capabilities();model_.relations=interface_relations(model_);TypeRelationScope relation_scope(model_.relations);
    for(const auto& n:program.children)if(n->kind!=N::Function && n->kind!=N::Struct)statement(*n);
    // Function bodies see all module-level bindings; local bindings remain declaration-ordered.
    auto functions=model_.functions;
    for(const auto& [name,fn]:functions) {
        if(model_.nested.contains(fn))continue;
        scopes_.emplace_back();current_function_=fn;
        if(auto dot=name.find('.');dot!=std::string::npos)define("self",{name.substr(0,dot),false,model_.mutating[name],{}},fn->span);
        auto params=parameters(*fn);
        if(name=="main" && !params.empty())error(*fn,"main must take no parameters","E3005");
        for(const auto* p:params)define(declared_name(*p),{p->children.empty()?"":type_name(*p->children[0]),false,mutable_parameter(*p),{}},p->span);
        if(fn->text.find(" external")!=std::string::npos) {
            if(!model_.external || !model_.external->contains(name))error(*fn,"unregistered external declaration","E7003");
            scopes_.pop_back();current_function_=nullptr;continue;
        }
        if(fn->text.find(" abstract")!=std::string::npos){scopes_.pop_back();current_function_=nullptr;continue;}
        statements(*fn->children.back(),false);
        auto result=result_type(*fn);
        if(!result.empty() && result!="void" && !result.ends_with('?') && !definitely_returns(*fn->children.back()))
            error(*fn,"typed function may reach its end without returning a value","E3006");
        scopes_.pop_back();current_function_=nullptr;
    }
    for(const auto& [node,type]:model_.types)if(node->kind==N::Call&&node->children.size()==1&&type=="Result<void,>")error(*node,"ok() requires an explicit Result<void,E> context","E3004");
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
        return binary_value(n.text,left,constant(*n.children[1]),n.span,numeric_literal(*n.children[1]));
    }
    default:error(n,"const initializer is not a supported compile-time expression","E3007");
    }
}
void SemanticAnalyzer::statement(const Node& n) {
    switch(n.kind) {
    case N::Block:statements(n);break;
    case N::Let:case N::Var:case N::Const: {
        auto info=expression(*n.children.back());if(info.type=="void")error(n,"void cannot be bound","E3004");if(!type_arguments(info.type,"multi").empty())error(n,"multiple values require positional binding","E3006");auto declared=n.children.size()==2 ? type_name(*n.children[0]):info.type;
        if(n.children.size()==2){validate_type(*n.children[0]);require(declared,info,*n.children.back());}
        std::optional<Value> folded;
        if(n.kind==N::Const){folded=enforce_type(constant(*n.children.back()),declared,n.span,numeric_literal(*n.children.back()));model_.constants[&n]=*folded;}
        bool writable=n.kind==N::Var;
        if(declared.starts_with('[')||declared.starts_with("map<")||declared.starts_with("Result<")||declared.starts_with("List<")||declared=="Buffer")writable=writable && info.writable;
        define(n.text,{declared,n.kind==N::Var,writable,folded},n.span);model_.types[&n]=declared;break;
    }
    case N::MultiBinding:case N::MultiAssignment: {
        const auto& list=*n.children[0];std::vector<Info> targets;
        if(n.kind==N::MultiAssignment)for(const auto& p:list.children)targets.push_back(p->kind==N::Name&&p->text=="_"?Info{}:target(*p));
        auto info=expression(*n.children[1]);auto types=type_arguments(info.type,"multi");
        if(!info.type.empty()&&types.size()!=list.children.size())error(n,"multiple value count mismatch","E3006");if(types.empty())types.resize(list.children.size());
        std::unordered_set<std::string> names;
        for(std::size_t i=0;i<list.children.size();++i) {
            const auto& p=*list.children[i];if(p.kind==N::Name&&p.text=="_")continue;if(n.kind==N::MultiBinding&&p.text=="_")continue;
            const Node& source=n.children[1]->kind==N::Pack?*n.children[1]->children[i]:*n.children[1];
            Info item={types[i],model_.writable.at(&source)};
            if(n.kind==N::MultiBinding){if(!names.insert(p.text).second)error(p,"duplicate declaration `"+p.text+"`","E3002");auto declared=p.children.empty()?types[i]:type_name(*p.children[0]);if(!p.children.empty()){validate_type(*p.children[0]);require(declared,item,source);}model_.types[&p]=declared;}
            else {require(targets[i].type,item,source);if(p.kind==N::Name){auto& symbol=lookup(p.text,p.span);symbol.type=symbol.declared.ends_with('?')?(item.type=="nil"?"nil":item.type.ends_with('?')?symbol.declared:symbol.declared.substr(0,symbol.declared.size()-1)):symbol.declared;}}
        }
        if(n.kind==N::MultiBinding)for(std::size_t i=0;i<list.children.size();++i){const auto& p=*list.children[i];if(p.text=="_")continue;auto t=model_.types.at(&p);bool writable=n.text=="var";
            if(t.starts_with('[')||t.starts_with("map<")||t.starts_with("Result<")||t.starts_with("List<")||t=="Buffer"){const Node& source=n.children[1]->kind==N::Pack?*n.children[1]->children[i]:*n.children[1];writable=writable&&model_.writable.at(&source);}
            define(p.text,{t,n.text=="var",writable,{}},p.span);}

        if(n.kind==N::MultiAssignment)for(std::size_t i=0;i<list.children.size();++i){const auto& p=*list.children[i];const auto& t=targets[i].type;if(p.kind==N::Name&&p.text!="_"&&(t.starts_with('[')||t.starts_with("map<")||t.starts_with("Result<")||t.starts_with("List<")||t=="Buffer")){const Node& source=n.children[1]->kind==N::Pack?*n.children[1]->children[i]:*n.children[1];lookup(p.text,p.span).writable=model_.writable.at(&source);}}
        break;
    }
    case N::Defer:case N::ExpressionStatement:expression(*n.children[0]);break;
    case N::Match:{
        auto info=expression(*n.children[0]);auto structure=model_.structures.find(info.type);bool enumeration=structure!=model_.structures.end()&&structure->second->text.find(" enum")!=std::string::npos;
        auto match_before=scopes_;std::vector<std::vector<std::unordered_map<std::string,Symbol>>> match_exits;
        auto cases=enumeration?enum_cases(*structure->second):std::vector<std::pair<std::string,std::vector<std::string>>>{};std::set<std::string> seen;bool wildcard=false;
        if(n.children.size()<2)error(n,"match requires at least one arm","E3011");
        for(std::size_t i=1;i<n.children.size();++i){const auto& arm=*n.children[i];const auto& pattern=*arm.children[0];auto name=pattern.text;bool any=pattern.kind==N::Name&&display_type(name)=="_";if(wildcard)error(arm,"unreachable match arm after _","E3011");scopes_=match_before;scopes_.emplace_back();
            if(any){wildcard=true;if(!arm.children[1]->children.empty())error(arm,"wildcard cannot bind payloads","E3011");}
            else if(enumeration){auto dot=name.rfind('.');if(dot!=std::string::npos){if(name.substr(0,dot)!=info.type)error(pattern,"variant belongs to another enum","E3011");name=name.substr(dot+1);}name=display_type(name);auto found=std::find_if(cases.begin(),cases.end(),[&](const auto& c){return c.first==name;});if(found==cases.end())error(pattern,"unknown enum variant","E3011");if(!seen.insert(name).second)error(arm,"duplicate match variant","E3011");if(found->second.size()!=arm.children[1]->children.size())error(arm,"enum payload binding count mismatch","E3011");model_.constants[&pattern]=Value(name);for(std::size_t j=0;j<found->second.size();++j){const auto& binding=*arm.children[1]->children[j];model_.types[&binding]=found->second[j];if(binding.text!="_")define(binding.text,{found->second[j],false,false,{}},binding.span);}}
            else {if(pattern.kind==N::Name||!arm.children[1]->children.empty())error(pattern,"scalar match requires literal patterns","E3011");auto value=constant(pattern);require(info.type,{value_type(value),false},pattern);auto key=value_type(value)+":"+show(value);if(!seen.insert(key).second)error(arm,"duplicate match literal","E3011");model_.constants[&pattern]=value;}
            if(info.type.ends_with('?')&&n.children[0]->kind==N::Name&&(pattern.kind==N::Nil||(any&&seen.contains("nil:nil")))){auto& symbol=lookup(n.children[0]->text,n.span);bool local=false;for(std::size_t scope=2;scope<scopes_.size();++scope)for(auto& [_,candidate]:scopes_[scope])if(&candidate==&symbol)local=true;if(local&&symbol.declared.ends_with('?'))symbol.type=pattern.kind==N::Nil?"nil":symbol.declared.substr(0,symbol.declared.size()-1);}
            statement(*arm.children[2]);scopes_.pop_back();if(!flow_stops(*arm.children[2]))match_exits.push_back(scopes_);
        }
        if(!wildcard&&!(enumeration&&seen.size()==cases.size())&&!(info.type=="bool"&&seen.size()==2))error(n,"non-exhaustive match requires missing variants or _","E3011");if(match_exits.empty())scopes_=match_before;else{scopes_=match_exits.front();for(std::size_t i=1;i<match_exits.size();++i)join_flow(match_exits[i]);}model_.types[&n]=info.type;break;
    }
    case N::If:{require("bool",expression(*n.children[0]),*n.children[0]);auto before=scopes_;narrow(*n.children[0],true);statement(*n.children[1]);auto yes=scopes_;scopes_=before;narrow(*n.children[0],false);if(n.children.size()==3)statement(*n.children[2]);bool yes_stops=flow_stops(*n.children[1]),no_stops=n.children.size()==3&&flow_stops(*n.children[2]);if(no_stops&&!yes_stops)scopes_=std::move(yes);else if(!yes_stops&&!no_stops)join_flow(yes);break;}
    case N::While:{invalidate_loop(*n.children[1]);require("bool",expression(*n.children[0]),*n.children[0]);auto before=scopes_;std::size_t flow_budget=1;for(const auto& scope:scopes_)flow_budget+=scope.size();for(std::size_t pass=0;pass<flow_budget;++pass){scopes_=before;narrow(*n.children[0],true);statement(*n.children[1]);auto after=scopes_;scopes_=before;join_flow(after);bool changed=false;for(std::size_t i=0;i<scopes_.size();++i)for(const auto& [name,symbol]:scopes_[i])changed=changed||symbol.type!=before[i].at(name).type||symbol.writable!=before[i].at(name).writable;before=scopes_;if(!changed)break;}invalidate_loop(*n.children[1]);bool has_break=false;std::function<void(const Node&)> find=[&](const Node& node){if(node.kind==N::Break)has_break=true;if(node.kind==N::Function||node.kind==N::While||node.kind==N::For)return;for(const auto& c:node.children)find(*c);};find(*n.children[1]);if(!has_break)narrow(*n.children[0],false);break;}
    case N::For: {
        invalidate_loop(*n.children.back());auto loop_before=scopes_;
        auto count=n.children.size()-2;const auto& iterable=*n.children[count];std::string item,key="int";
        if(iterable.kind==N::Range){for(const auto& c:iterable.children)require("int",expression(*c),*c);item="int";}
        else {auto info=expression(iterable);auto map=type_arguments(info.type,"map");if(map.size()==2){key=map[0];item=count==1?key:map[1];}else {item=element_type(info.type);if(!info.type.empty() && !info.type.starts_with('['))error(iterable,"for requires a range, slice or map","E3004");}}
        scopes_.emplace_back();
        for(std::size_t i=0;i<count;++i)define(n.children[i]->text,{count==2 && i==0 ? key:item,false,false,{}},n.children[i]->span);
        statements(*n.children.back(),false);scopes_.pop_back();join_flow(loop_before);auto loop_entry=scopes_;std::size_t budget=1;for(const auto& scope:scopes_)budget+=scope.size();for(std::size_t pass=0;pass<budget;++pass){scopes_=loop_entry;scopes_.emplace_back();for(std::size_t i=0;i<count;++i)define(n.children[i]->text,{count==2&&i==0?key:item,false,false,{}},n.children[i]->span);statements(*n.children.back(),false);scopes_.pop_back();join_flow(loop_entry);bool changed=false;for(std::size_t i=0;i<scopes_.size();++i)for(const auto& [name,symbol]:scopes_[i])changed=changed||symbol.type!=loop_entry[i].at(name).type||symbol.writable!=loop_entry[i].at(name).writable;loop_entry=scopes_;if(!changed)break;}invalidate_loop(*n.children.back());break;
    }
    case N::Return: {
        const Node* returns=nullptr;for(const auto& c:current_function_->children)if(c->kind==N::ReturnTypes)returns=c.get();
        bool void_return=returns && returns->children.size()==1 && type_name(*returns->children[0])=="void";
        if(returns&&returns->children.size()>1&&n.children.size()==1){auto info=expression(*n.children[0]);auto ts=type_arguments(info.type,"multi");if(!info.type.empty()&&ts.size()!=returns->children.size())error(n,"return value count does not match function signature","E3006");for(std::size_t i=0;i<ts.size();++i)require(type_name(*returns->children[i]),{ts[i],false},*n.children[0]);break;}
        if(returns && (void_return ? 0:returns->children.size())!=n.children.size())error(n,"return value count does not match function signature","E3006");
        for(std::size_t i=0;i<n.children.size();++i){auto info=expression(*n.children[i]);if(returns)require(type_name(*returns->children[i]),info,*n.children[i]);}break;
    }
    case N::ExternalInit:if(!model_.external || !model_.external->module_exists(n.text))error(n,"unregistered external initializer","E7003");break;
    case N::Break:case N::Continue:break;
    case N::Unsafe:statement(*n.children[0]);break;
    case N::Import:{auto name=n.children.empty()?n.text.substr(0,n.text.find('.')):n.children[0]->text;define(name,{"module",false,false,{}},n.span);break;}
    case N::Function: {
        auto name=declared_name(n);model_.functions.emplace(name,&n);model_.nested[&n]=true;
        define(name,{"fn:"+name,false,false,{}},n.span);
        auto outer=current_function_;auto saved=scopes_;current_function_=&n;
        // Closures capture readonly snapshots at declaration, rather than dangling stack environments.
        for(std::size_t i=2;i<scopes_.size();++i)for(auto& [_,symbol]:scopes_[i]){symbol.mutable_binding=false;symbol.writable=false;}
        scopes_.emplace_back();
        for(const auto* p:parameters(n)){if(!p->children.empty())validate_type(*p->children[0]);define(declared_name(*p),{p->children.empty()?"":type_name(*p->children[0]),false,mutable_parameter(*p),{}},p->span);}
        for(const auto& c:n.children)if(c->kind==N::ReturnTypes)for(const auto& t:c->children)validate_type(*t);
        statements(*n.children.back(),false);
        auto result=result_type(n);if(!result.empty()&&result!="void"&&!result.ends_with('?')&&!definitely_returns(*n.children.back()))error(n,"typed function may reach its end without returning a value","E3006");
        scopes_=std::move(saved);current_function_=outer;break;
    }
    case N::Struct:error(n,"local struct requires lexical type lowering","E3008");
    default:error(n,"unexpected statement in semantic analysis","E3008");
    }
}
SemanticAnalyzer::Info SemanticAnalyzer::target(const Node& n) {
    if(n.kind==N::Name) {auto& s=lookup(n.text,n.span);if(!s.mutable_binding)error(n,"cannot assign to immutable binding `"+n.text+"`","E3003");return {s.declared,true};}
    auto info=expression(n);
    if(!info.writable)error(n,"cannot mutate a read-only value/view","E3003");
    if(n.kind==N::Index){auto t=type_arguments(model_.types.at(n.children[0].get()),"map");if(t.size()==2)info.type=t[1];}
    return info;
}
SemanticAnalyzer::Info SemanticAnalyzer::expression(const Node& n) {
    Info out;
    switch(n.kind) {
    case N::Integer:out={"int",false};break;case N::Float:out={"float",false};break;
    case N::String:out={"string",false};break;case N::Boolean:out={"bool",false};break;case N::Nil:out={"nil",false};break;
    case N::Name:{auto& s=lookup(n.text,n.span);out={s.type,s.writable};break;}
    case N::Pack: {
        std::string type="multi<";bool writable=true;for(std::size_t i=0;i<n.children.size();++i){auto x=expression(*n.children[i]);if(x.type=="void")error(n,"void cannot be packed as a value","E3004");if(!type_arguments(x.type,"multi").empty())error(n,"nested multiple values are not supported","E3006");if(i)type+=',';type+=x.type;if(x.type.starts_with('[')||x.type.starts_with("map<")||x.type.starts_with("Result<")||x.type.starts_with("List<")||x.type=="Buffer")writable=writable&&x.writable;}out={type+">",writable};break;
    }
    case N::MapLiteral: {
        validate_type(*n.children[0]);auto type=type_name(*n.children[0]);auto ts=type_arguments(type,"map");
        for(std::size_t i=1;i<n.children.size();++i){const auto& e=*n.children[i];require(ts[0],expression(*e.children[0]),*e.children[0]);require(ts[1],expression(*e.children[1]),*e.children[1]);}out={type,true};break;
    }
    case N::Propagate: {
        auto value=expression(*n.children[0]);auto ts=type_arguments(value.type,"Result"),returns=current_function_?type_arguments(result_type(*current_function_),"Result"):std::vector<std::string>{};
        if(ts.size()!=2||returns.size()!=2)error(n,"Result propagation requires a Result value and Result-returning function","E3006");
        if(ts[0].empty())error(n,"Result success type is unknown; add a type annotation","E3004");
        require(returns[1],{ts[1],false},n);out={ts[0],value.writable};break;
    }
    case N::Array: {
        std::string element;
        for(const auto& c:n.children){auto x=expression(*c);if(x.type=="void")error(*c,"void cannot be an array element","E3004");if(element.empty())element=x.type;else require(element,x,*c);}
        out={"[]"+element,true};break;
    }
    case N::Unary: {
        auto x=expression(*n.children[0]);
        if(x.type=="void")error(n,"void is not an operand","E3004");
        if(auto numeric=numeric_spec(x.type);numeric&&x.type!="int"&&x.type!="float"&&n.text!="&"&&n.text!="!"){if(n.text=="~"&&numeric->category=='f')error(n,"bitwise unary requires an integer","E3004");if(n.text=="-"&&numeric->category=='u')error(n,"unsigned negation requires explicit signed conversion","E3004");out={x.type,false};break;}
        if(n.text=="&"){out={"ref<"+x.type+">",false};break;}
        if(n.text=="!"){require("bool",x,n);out={"bool",false};break;}
        if(n.text=="~")require("int",x,n);
        else if(!x.type.empty() && x.type!="int" && x.type!="float")error(n,"unary operator requires numeric operand","E3004");
        out={x.type,false};break;
    }
    case N::Binary: {
        auto a=expression(*n.children[0]);auto op=n.text;auto before=scopes_;if(op=="&&"||op=="||")narrow(*n.children[0],op=="&&");auto b=expression(*n.children[1]);if(op=="&&"||op=="||")join_flow(before);
        if(a.type=="void"||b.type=="void")error(n,"void is not an operand","E3004");
        if(op=="&&" || op=="||"){require("bool",a,n);require("bool",b,n);out={"bool",false};break;}
        if(op=="==" || op=="!=") {
            if(a.type!="nil" && b.type!="nil")require(a.type,b,n);
            out={"bool",false};break;
        }
        if(numeric_spec(a.type)&&numeric_spec(b.type)&&((a.type!="int"&&a.type!="float")||(b.type!="int"&&b.type!="float"))){
            auto left=*numeric_spec(a.type),right=*numeric_spec(b.type);std::string common=a.type;
            if(!numeric_literal(*n.children[1])&&!numeric_widening(common,b.type)){if(numeric_widening(b.type,common))common=b.type;else if(!(op=="**"&&left.category=='f'&&right.category!='f')&&op!="<<"&&op!=">>")error(n,"mixed numeric categories require explicit conversion","E3004");}
            auto spec=*numeric_spec(common);if((op=="&"||op=="|"||op=="^"||op=="<<"||op==">>"||op=="//"||op=="%")&&(spec.category=='f'||right.category=='f'))error(n,"integer operator requires integer operands","E3004");
            bool compare=op=="=="||op=="!="||op=="<"||op=="<="||op==">"||op==">=";out={compare?"bool":op=="/"&&spec.category!='f'?"float":common,false};break;
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
            if(n.text!="=" && !lhs.type.empty() && !numeric_spec(lhs.type) && !(n.text=="+=" && lhs.type=="string"))error(n,"compound operator is not defined for this type","E3004");
            if((n.text=="%=" || n.text=="//=") && !lhs.type.empty() && (!numeric_spec(lhs.type)||numeric_spec(lhs.type)->category=='f'))error(n,"integer compound operator requires int","E3004");
            if(n.text=="/=" && numeric_spec(lhs.type)&&numeric_spec(lhs.type)->category!='f')error(n,"/= produces float; use a float binding or explicit conversion","E3004");
            if(n.children[0]->kind==N::Name){auto& symbol=lookup(n.children[0]->text,n.span);symbol.type=symbol.declared.ends_with('?')?(rhs.type=="nil"?"nil":rhs.type.ends_with('?')?symbol.declared:symbol.declared.substr(0,symbol.declared.size()-1)):symbol.declared;}
            if(n.children[0]->kind==N::Name && (lhs.type.starts_with('[')||lhs.type.starts_with("map<")||lhs.type.starts_with("Result<")||lhs.type.starts_with("List<")||lhs.type=="Buffer"))lookup(n.children[0]->text,n.span).writable=rhs.writable;
        } else if(!numeric_spec(lhs.type)||numeric_spec(lhs.type)->category=='f')require("int",lhs,n);
        out={lhs.type,false};break;
    }
    case N::Index:case N::Slice: {
        auto base=expression(*n.children[0]);auto element=element_type(base.type);auto map=type_arguments(base.type,"map");
        if(n.kind==N::Index&&map.size()==2){require(map[0],expression(*n.children[1]),*n.children[1]);out={map[1].ends_with('?')?map[1]:map[1]+"?",base.writable};break;}
        if(n.kind==N::Index&&base.type.empty()){expression(*n.children[1]);out={"",base.writable};break;}
        if(!base.type.empty() && !base.type.starts_with('['))error(n,"index/slice requires a slice or array","E3004");
        for(std::size_t i=1;i<n.children.size();++i)if(n.children[i]->kind!=N::Omitted)require("int",expression(*n.children[i]),*n.children[i]);
        out={n.kind==N::Slice ? "[]"+element:element,base.writable};break;
    }
    case N::StructLiteral: {
        if(n.text=="std.error.Value")error(n,"standard Error must be constructed through std.error","E3004");
        auto found=model_.structures.find(n.text);if(found==model_.structures.end())error(n,"unknown struct `"+n.text+"`","E3004");
        if(found->second->text.find(" interface")!=std::string::npos)error(n,"interface cannot be constructed directly","E3004");
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
        if(n.text=="len" && (type.starts_with('[') || type=="string" || type.starts_with("map<"))){out={"int",false};break;}
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
        for(std::size_t i=1;i<n.children.size();++i){auto a=expression(*n.children[i]);if(!type_arguments(a.type,"multi").empty())error(n,"multiple values require positional binding","E3006");if(a.type=="void")error(*n.children[i],"void cannot be passed as a value","E3004");args.push_back(a);}
        if(callee.type.starts_with("fn:")) {
            auto key=callee.type.substr(3);auto fn=model_.functions.at(key);if(fn->text.find(" abstract")!=std::string::npos&&n.children[0]->kind!=N::Member)error(n,"interface prototype requires a concrete receiver","E3004");auto ps=parameters(*fn);
            if(ps.size()!=args.size())error(n,"function argument count mismatch","E3005");
            for(std::size_t i=0;i<args.size();++i) {
                if(!ps[i]->children.empty())require(type_name(*ps[i]->children[0]),args[i],*n.children[i+1]);
                if(mutable_parameter(*ps[i])&&!ps[i]->children.empty()&&!args[i].type.empty()){auto annotation=type_name(*ps[i]->children[0]);if(annotation.starts_with('[')&&(element_type(annotation)!=element_type(args[i].type)||(annotation.find(']')>1&&args[i].type.starts_with("[]"))))error(*n.children[i+1],"mut borrow requires identical element type and existing fixed array","E3004");}
                if(mutable_parameter(*ps[i]) && !args[i].writable)error(*n.children[i+1],"mut parameter requires mutable argument","E3003");
            }
            out={result_type(*fn),model_.return_writable.contains(key)&&model_.return_writable.at(key)};
        } else if(callee.type.starts_with("builtin:")) {
            auto name=callee.type.substr(8);
            if(name=="task_start"){
                if(args.empty()||!args[0].type.starts_with("fn:"))error(n,"spawn requires a Hua function","E3004");
                auto fn=model_.functions.at(args[0].type.substr(3));auto ps=parameters(*fn);if(ps.size()+1!=args.size())error(n,"spawn argument count mismatch","E3005");
                if(fn->text.find(" external")!=std::string::npos||fn->text.find(" unsafe")!=std::string::npos)error(n,"Native/WASM functions cannot be spawned","E3008");
                for(std::size_t i=0;i<ps.size();++i){if(mutable_parameter(*ps[i]))error(n,"spawn cannot pass mutable borrows","E3003");if(!ps[i]->children.empty())require(type_name(*ps[i]->children[0]),args[i+1],*n.children[i+2]);}
                auto r=result_type(*fn);if(r.empty()){bool value_return=false;std::function<void(const Node&)> scan=[&](const Node& c){if(c.kind==N::Function)return;if(c.kind==N::Return&&!c.children.empty())value_return=true;for(const auto& item:c.children)scan(*item);};scan(*fn->children.back());if(value_return)error(n,"spawned value functions require an explicit return type","E3004");}out={r.starts_with("Task<")&&!declared_name(*fn).starts_with("$async$")?r:"Task<"+(r.empty()?"void":r)+">",false};break;
            }
            if(name=="task_await"){if(args.size()!=1)error(n,"await requires one task","E3005");auto types=type_arguments(args[0].type,"Task");if(types.size()!=1)error(n,"await requires Task<T>","E3004");out={types[0],true};break;}
            if(name=="task_group_begin"){if(!args.empty())error(n,"invalid taskgroup start","E3005");out={"int",false};break;}
            if(name=="task_group_end"){if(args.size()!=1)error(n,"invalid taskgroup end","E3005");require("int",args[0],*n.children[1]);out={"void",false};break;}
            if(name=="simd_check"||name=="parallel_map"){
                if(args.size()!=(name=="simd_check"?1u:7u))error(n,"invalid parallel/SIMD metadata","E3005");auto type=args[name=="simd_check"?0:4].type;auto element=element_type(type);
                if(!type.empty()&&(!type.starts_with('[')||!numeric_spec(element)))error(n,"parallel/SIMD requires a numeric array","E3004");
                if(name=="parallel_map"){if(!args[0].type.starts_with("fn:"))error(n,"parallel requires a Hua kernel","E3004");for(std::size_t i=1;i<=3;++i)require("int",args[i],*n.children[i+1]);require("bool",args[5],*n.children[6]);require("bool",args[6],*n.children[7]);auto fn=model_.functions.at(args[0].type.substr(3));if(declared_name(*fn).find("$parallel$")==std::string::npos)error(n,"parallel_map is a compiler intrinsic","E3008");const auto& expr=*fn->children.back()->children.back()->children[0];require(element,{model_.types.at(&expr),model_.writable.at(&expr)},expr);}
                out={name=="parallel_map"?"[]"+element:"void",true};break;
            }
            if(auto f=standard_function(name)){
                if(args.size()!=f->parameters.size())error(n,"standard function argument count mismatch","E3005");
                std::vector<std::string> types;for(const auto& a:args)types.push_back(a.type);
                if(f->module=="math")for(std::size_t i=0;i<types.size();++i)if(!types[i].empty()&&types[i]!="float")error(*n.children[i+1],"math requires explicit default float arguments","E3004");
                if(f->module=="alg"&&!types[0].empty()){
                    auto element=element_type(types[0]);
                    if(!types[0].starts_with('[')||(element!="int"&&element!="float"&&element!="string"))error(n,"algorithm requires int/float/string array or slice","E3004");
                    if(f->name=="sum"&&element=="string")error(n,"sum requires int/float array or slice","E3004");
                    if(types.size()==2&&!types[1].empty()&&types[1]!=element)error(*n.children[2],"algorithm target requires identical element type","E3004");
                }
                if(f->module=="list"&&!types.empty()&&!types[0].empty()){
                    if(f->name=="from_slice"){if(!types[0].starts_with('[')||element_type(types[0]).empty())error(n,"from_slice requires a typed slice/array","E3004");}
                    else if(type_arguments(types[0],"List").size()!=1)error(n,"expected List<T>","E3004");
                }
                if(f->module=="simd"&&f->name!="backend"){auto a=element_type(types[0]),b=element_type(types[1]);if(a!=b||(a!="f32"&&a!="f64"&&a!="float"))error(n,"SIMD requires matching f32/f64/float arrays","E3004");}
                if(f->module=="task"&&f->name!="sleep"&&f->name!="yield"&&f->name!="worker_id"){
                    auto task=type_arguments(types[0],"Task");if(task.size()!=1)error(n,"expected Task<T>","E3004");
                    if((f->name=="timeout"||f->name=="all")&&task[0]=="void")error(n,"this operation requires a value task","E3004");
                    if((f->name=="all"||f->name=="race")&&types[0]!=types[1])error(n,"task combination requires identical Task<T> types","E3004");
                }
                auto signature=standard_signature(*f,types);
                for(std::size_t i=0;i<args.size();++i){
                    if(f->module=="list"&&((f->name=="append"&&i==1)||(f->name=="set"&&i==2))&&!compatible(signature.parameters[i],args[i].type))error(*n.children[i+1],"list element type mismatch","E3004");
                    require(signature.parameters[i],args[i],*n.children[i+1]);
                }
                if(f->mutates_first&&!args[0].writable)error(n,"standard mutation requires a writable argument","E3003");
                out={signature.result,f->writable_result};break;
            }
            const std::size_t arity=(name=="min"||name=="max"||name=="has"||name=="delete"||name=="unwrap_or")?2:name=="clamp"?3:1;
            if(name!="print" && !(name=="ok"&&args.empty()) && args.size()!=arity)error(n,"builtin argument count mismatch","E3005");
            if(numeric_spec(name)&&name!="int"&&name!="float"){if(!args[0].type.empty()&&!numeric_spec(args[0].type)&&args[0].type!="string")error(n,"numeric conversion requires a number or string","E3004");out={name,false};break;}
            if(name=="ok"&&args.empty()){out={"Result<void,>",false};break;}
            if(name=="ok"||name=="err"){out={name=="ok"?"Result<"+args[0].type+",>":"Result<,"+args[0].type+">",args[0].writable};if(args[0].type!="void"&&args[0].type!="nil"&&!args[0].type.starts_with('[')&&!args[0].type.starts_with("map<")&&!args[0].type.starts_with("List<")&&args[0].type!="Buffer")out.writable=true;if(args[0].type=="void")error(n,"Result payload cannot be void","E3004");break;}
            if(name=="is_some"||name=="is_none"){out={"bool",false};break;}
            if(name=="is_ok"||name=="is_err"||name=="unwrap_err"){auto ts=type_arguments(args[0].type,"Result");if(!args[0].type.empty()&&ts.size()!=2)error(n,"expected Result","E3004");out={name=="unwrap_err"?(ts.empty()?"":ts[1]):"bool",args[0].writable};break;}
            if(name=="unwrap"||name=="unwrap_or"){auto ts=type_arguments(args[0].type,"Result");std::string type=ts.size()==2?ts[0]:args[0].type;if(ts.empty()&&type.ends_with('?'))type.pop_back();if(type=="nil")type="";if(name=="unwrap_or"){if(type=="void")error(n,"Result<void> has no fallback value","E3004");require(type,args[1],*n.children[2]);if(type.empty())type=args[1].type;}out={type,args[0].writable&&(name=="unwrap"||args[1].writable)};break;}
            if(name=="has"||name=="delete"){auto ts=type_arguments(args[0].type,"map");if(!args[0].type.empty()&&ts.size()!=2)error(n,"expected map","E3004");if(!ts.empty())require(ts[0],args[1],*n.children[2]);if(name=="delete"&&!args[0].writable)error(n,"cannot mutate a read-only map","E3003");out={"bool",false};break;}
            if(!args.empty() && !args[0].type.empty()) {
                const auto& t=args[0].type;
                if((name=="sqrt" || name=="abs") && !numeric_spec(t))error(n,"numeric builtin requires int or float","E3004");
                if(name=="len" && t!="string" && !t.starts_with('[') && !t.starts_with("map<"))error(n,"len requires a slice or string","E3004");
                if((name=="int" || name=="float") && !numeric_spec(t) && t!="string")error(n,"conversion requires int, float or string","E3004");
                if((name=="min" || name=="max" || name=="clamp") && !numeric_spec(t) && t!="string")error(n,"comparison builtin requires matching scalar types","E3004");
            }
            if(name=="print")out={"void",false};else if(name=="str" || name=="type")out={"string",false};
            else if(name=="sizeof"||name=="alignof"){if(!numeric_spec(args[0].type)&&args[0].type!="bool"&&(!args[0].type.starts_with('[')||args[0].type.starts_with("[]")))error(n,"layout query requires scalar or fixed array","E3004");out={"int",false};}
            else if(name=="int" || name=="len")out={"int",false};else if(name=="float" || name=="sqrt")out={"float",false};
            else if(name=="clone")out={args[0].type,true};else out={args[0].type,false};
            if(name=="min" || name=="max" || name=="clamp")for(std::size_t i=1;i<args.size();++i)require(args[0].type,args[i],*n.children[i+1]);
        } else if(!callee.type.empty())error(n,"value is not callable","E3004");
        break;
    }
    default:error(n,"expression is not supported by semantic analysis","E3008");
    }
    model_.types[&n]=out.type;model_.writable[&n]=out.writable;return out;
}
void SemanticAnalyzer::join_flow(const std::vector<std::unordered_map<std::string,Symbol>>& other){for(std::size_t i=0;i<scopes_.size()&&i<other.size();++i)for(auto& [name,symbol]:scopes_[i])if(auto found=other[i].find(name);found!=other[i].end()){if(symbol.type!=found->second.type)symbol.type=symbol.declared;symbol.writable=symbol.writable&&found->second.writable;}}
void SemanticAnalyzer::invalidate_loop(const Node& body){std::set<std::string> writes;std::function<void(const Node&)> visit=[&](const Node& n){if(n.kind==N::Function)return;if((n.kind==N::Assignment||n.kind==N::Update)&&n.children[0]->kind==N::Name)writes.insert(n.children[0]->text);if(n.kind==N::MultiAssignment)for(const auto& c:n.children[0]->children)if(c->kind==N::Name)writes.insert(c->text);for(const auto& c:n.children)visit(*c);};visit(body);for(const auto& name:writes)for(auto i=scopes_.rbegin();i!=scopes_.rend();++i)if(auto found=i->find(name);found!=i->end()){found->second.type=found->second.declared;break;}}
std::vector<std::pair<SemanticAnalyzer::Symbol*,std::string>> SemanticAnalyzer::narrow(const Node& n,bool truth) {
    std::vector<std::pair<Symbol*,std::string>> previous;
    if(n.kind==N::Unary&&n.text=="!")return narrow(*n.children[0],!truth);
    if(n.kind==N::Binary&&((n.text=="&&"&&truth)||(n.text=="||"&&!truth))){previous=narrow(*n.children[0],truth);auto next=narrow(*n.children[1],truth);previous.insert(previous.end(),next.begin(),next.end());return previous;}
    const Node* name=nullptr;bool nonnil=truth;
    if(n.kind==N::Binary&&(n.text=="!="||n.text=="==")){if(n.children[0]->kind==N::Name&&n.children[1]->kind==N::Nil)name=n.children[0].get();if(n.children[1]->kind==N::Name&&n.children[0]->kind==N::Nil)name=n.children[1].get();nonnil=truth==(n.text=="!=");}
    if(n.kind==N::Call&&n.children.size()==2&&n.children[0]->kind==N::Name){auto op=n.children[0]->text;if(op.starts_with("$core$"))op=op.substr(6);if((op=="is_some"||op=="is_none")&&n.children[1]->kind==N::Name){name=n.children[1].get();nonnil=truth==(op=="is_some");}}
    if(name){for(std::size_t i=scopes_.size();i-->0;){auto it=scopes_[i].find(name->text);if(it==scopes_[i].end())continue;auto& symbol=it->second;if(i>=2&&symbol.declared.ends_with('?')){previous.emplace_back(&symbol,symbol.type);symbol.type=nonnil?symbol.declared.substr(0,symbol.declared.size()-1):"nil";}break;}}
    return previous;
}
void SemanticAnalyzer::restore_types(const std::vector<std::pair<Symbol*,std::string>>& old){for(auto it=old.rbegin();it!=old.rend();++it)it->first->type=it->second;}

}
