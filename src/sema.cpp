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
bool file_type(const std::string& type,const SemanticModel& model,std::set<std::string> seen={}){
    if(type=="std.file.File")return true;
    if(!seen.insert(type).second)return false;
    if(type.ends_with('?'))return file_type(type.substr(0,type.size()-1),model,seen);
    if(type.starts_with('['))return file_type(type.substr(type.find(']')+1),model,seen);
    for(auto c:{"map","List","Result","multi","Task"})for(const auto& t:type_arguments(type,c))if(file_type(t,model,seen))return true;
    if(auto found=model.structures.find(type);found!=model.structures.end())
        for(const auto& field:found->second->children)if(field->kind==N::Field&&!field->children.empty()&&file_type(type_name(*field->children[0]),model,seen))return true;
    return false;
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
void SemanticAnalyzer::validate_type(const Node& n,bool allow_void) {
    if(n.kind==N::TypeName&&n.text=="void"&&!allow_void)error(n,"void is only a no-value function/Result/Task success type","E3004");
    if(n.kind==N::ArrayType){auto count=fixed_extent(n.text);if(!count||*count>1000000)error(n,"fixed array size must be an integer literal in 0..1000000","E3004");}
    static const std::unordered_set<std::string> types={"int","float","bool","string","byte","i8","i16","i32","i64","u8","u16","u32","u64","f32","f64","usize","isize","void","Result","std.error.Value","std.file.File","std.file.IOFailure","Json","Bytes","Buffer","Task"};
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
    for(std::size_t i=0;i<n.children.size();++i)validate_type(*n.children[i],n.kind==N::GenericType&&(n.text=="Result"||n.text=="Task")&&i==0);
}
void SemanticAnalyzer::require(const std::string& expected,const Info& actual,const Node& n) {
    if(expected.ends_with('?')&&actual.type!="nil"&&!actual.type.ends_with('?')){require(expected.substr(0,expected.size()-1),actual,n);return;}
    if(numeric_spec(expected)&&numeric_spec(actual.type)&&numeric_literal(n)&&(numeric_spec(expected)->category=='f'||numeric_spec(actual.type)->category!='f'))return;
    if(expected.starts_with('[')&&n.kind==N::Array){auto close=expected.find(']');if(close==std::string::npos)error(n,"invalid array type","E3004");auto count=fixed_extent(expected.substr(1,close-1));if(close>1&&(!count||*count!=n.children.size()))error(n,"fixed array length mismatch","E3004");auto element=expected.substr(close+1);for(const auto& c:n.children)require(element,{model_.types.at(c.get()),model_.writable.at(c.get())},*c);return;}
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
        HUA_SEMA_COUNT(mutating_passes);
        changed=false;
        for(const auto& [name,fn]:model_.functions)if(auto dot=name.find('.');dot!=std::string::npos)
            if(!model_.mutating[name] && direct(*fn->children.back(),name.substr(0,dot))){model_.mutating[name]=true;changed=true;}
    }
}
void SemanticAnalyzer::infer_return_capabilities(){
    for(const auto& [name,fn]:model_.functions)model_.return_writable[name]=fn->text.find(" abstract")==std::string::npos&&fn->text.find(" external")==std::string::npos;
    bool changed=true;std::size_t passes=0;
    while(changed&&passes++<=model_.functions.size()+1){HUA_SEMA_COUNT(return_passes);changed=false;for(const auto& [name,fn]:model_.functions){
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
    probing_=false;defer_site_=nullptr;field_shapes_.clear();node_views_.clear();node_positions_.clear();node_position_views_.clear();return_facts_.clear();effects_.clear();actual_origins_.clear();actual_views_.clear();pending_return_={};pending_effects_={};effect_roots_.clear();exit_views_.clear();saw_return_=false;
    // Parsing reserved reference/unsafe syntax is useful, but no executor can
    // implement it yet. Scan even unreachable code before any execution/build.
    std::function<void(const Node&)> executable=[&](const Node& n) {
        if(n.kind==N::Unsafe || (n.kind==N::Function&&n.text.find(" unsafe")!=std::string::npos))
            error(n,"unsafe execution/FFI is not implemented","E3008");
        if((n.kind==N::GenericType&&(n.text=="ref"||n.text=="ptr")) || (n.kind==N::Unary&&n.text=="&"))
            error(n,"safe reference/raw pointer execution is not implemented","E3008");
        for(const auto& c:n.children)executable(*c);
    };executable(program);
#ifdef HUA_SEMA_PROFILE
    struct OriginLayout {const Node* owner;std::string path;bool readonly,copied;};
    struct InfoLayout {std::string type;bool writable,readonly_children;std::set<Origin> origins;bool deep_owned,record_copy;};
    static_assert(sizeof(Origin)==sizeof(OriginLayout)&&sizeof(Info)==sizeof(InfoLayout));
    static_assert(std::is_nothrow_move_constructible_v<Info>&&std::is_nothrow_move_constructible_v<Symbol>);
    if(sema_profile::active)sema_profile::active->layouts={sizeof(Origin),sizeof(Info),sizeof(Symbol)};
#endif
    infos_.clear();model_={};model_.external=std::move(external);scopes_.clear();scopes_.emplace_back();current_function_=nullptr;
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
        for(const auto& f:structure->children){if(!fields.insert(f->text).second)error(*f,"duplicate struct field `"+f->text+"`","E3002");validate_type(*f->children[0],f->text.starts_with("$")&&f->text.ends_with("#void"));}
    }
    for(const auto& [name,fn]:model_.functions) {
        if(auto dot=name.find('.');dot!=std::string::npos && !model_.structures.contains(name.substr(0,dot)))error(*fn,"method owner is not a declared struct","E3004");
        for(const auto& p:parameters(*fn))if(!p->children.empty())validate_type(*p->children[0]);
        for(const auto& c:fn->children)if(c->kind==N::ReturnTypes)for(const auto& t:c->children)validate_type(*t,c->children.size()==1);
    }
    infer_mutating_methods();infer_return_capabilities();model_.relations=interface_relations(model_);TypeRelationScope relation_scope(model_.relations);
    for(const auto& n:program.children)if(n->kind!=N::Function && n->kind!=N::Struct)statement(*n);
    infer_field_capabilities(program);
    // Function bodies see all module-level bindings; local bindings remain declaration-ordered.
    HUA_SEMA_PHASE(checking);
    auto functions=model_.functions;auto globals=HUA_SEMA_SNAPSHOT(scopes_,global_snapshot);
    for(const auto& [name,fn]:functions) {
        if(model_.nested.contains(fn))continue;
        HUA_SEMA_FUNCTION(name,0,false);
        HUA_SEMA_RESTORE(scopes_,globals,function_reset);scopes_.emplace_back();current_function_=fn;
        if(auto dot=name.find('.');dot!=std::string::npos)define("self",{name.substr(0,dot),false,model_.mutating[name],{}},fn->span);
        auto params=parameters(*fn);
        if(name.find('.')!=std::string::npos)seed("self",fn,!model_.mutating[name]);
        if(name=="main" && !params.empty())error(*fn,"main must take no parameters","E3005");
        for(const auto* p:params){define(declared_name(*p),{p->children.empty()?"":type_name(*p->children[0]),false,mutable_parameter(*p),{}},p->span);seed(declared_name(*p),p,!mutable_parameter(*p));}
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

bool SemanticAnalyzer::record(std::string type) const {
    type=unref(type);if(type.ends_with('?'))type.pop_back();
    return model_.structures.contains(type);
}
SemanticAnalyzer::Info SemanticAnalyzer::copied(Info value) const {
    // Struct assignment copies the outer record; inherited shared children retain their capability.
    if(record(value.type)) {
        value.readonly_children=value.readonly_children||!value.writable;
        value.record_copy=true;if(!value.writable)value.deep_owned=false;value.writable=true;std::set<Origin> origins;for(auto origin:value.origins){origin.copied=true;origins.insert(std::move(origin));}value.origins=std::move(origins);
    }
    return value;
}
std::pair<SemanticAnalyzer::Symbol*,std::string> SemanticAnalyzer::projection(const Node& n) {
    if(n.kind==N::Name)return {&lookup(n.text,n.span),""};
    if(n.kind==N::Member){auto [root,path]=projection(*n.children[0]);return {root,path+"."+n.text};}
    if(n.kind==N::Index){auto [root,path]=projection(*n.children[0]);return {root,path+"[]"};}
    return {nullptr,""};
}
void SemanticAnalyzer::remember(const Node& destination,const Node& source,Info value) {
    auto [root,path]=projection(destination);if(!root)return;
    if(!path.empty()&&!value.type.starts_with('[')&&!value.type.starts_with("map<")&&!value.type.starts_with("Result<")&&!value.type.starts_with("List<")&&value.type!="Buffer"&&!record(value.type))return;
    auto views=node_views_.contains(&source)?node_views_.at(&source):fields(value);
    Views inherited;
    for(auto [field,view]:views){
        if(!value.writable){view.writable=false;view.readonly_children=true;}
        if(record(value.type)&&record(view.type))view=copied(view);
        inherited.emplace(path+field,std::move(view));
    }
    value=copied(value);bound(inherited,value);
    if(path.empty()){root->readonly_children=value.readonly_children;root->origins=value.origins;root->deep_owned=value.deep_owned;root->record_copy=value.record_copy;root->views=std::move(inherited);if(probing_&&effect_roots_.contains(root->declaration)){pending_effects_.writes[root->declaration][""]=value;for(const auto& [field,view]:root->views)pending_effects_.writes[root->declaration][field]=view;}}
    else {
        for(auto it=root->views.begin();it!=root->views.end();)if(it->first==path||it->first.starts_with(path+".")||it->first.starts_with(path+"[]"))it=root->views.erase(it);else ++it;
        root->views[path]=value;
        root->views.insert(inherited.begin(),inherited.end());
        if(probing_&&effect_roots_.contains(root->declaration))pending_effects_.writes[root->declaration][path]=value;
        if(probing_&&effect_roots_.contains(root->declaration))for(const auto& [field,view]:inherited)pending_effects_.writes[root->declaration][field]=view;
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
        define(n.text,{declared,n.kind==N::Var,writable,folded},n.span);lookup(n.text,n.span).declaration=&n;Node destination(N::Name,n.span,n.text);remember(destination,*n.children.back(),info);model_.types[&n]=declared;break;
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
            Info item=node_positions_.contains(&source)&&i<node_positions_.at(&source).size()?node_positions_.at(&source)[i]:Info{types[i],model_.writable.at(&source),infos_.at(&source).readonly_children};
            if(n.kind==N::MultiBinding){if(!names.insert(p.text).second)error(p,"duplicate declaration `"+p.text+"`","E3002");auto declared=p.children.empty()?types[i]:type_name(*p.children[0]);if(!p.children.empty()){validate_type(*p.children[0]);require(declared,item,source);}model_.types[&p]=declared;}
            else {require(targets[i].type,item,source);if(p.kind==N::Name){auto& symbol=lookup(p.text,p.span);symbol.type=symbol.declared.ends_with('?')?(item.type=="nil"?"nil":item.type.ends_with('?')?symbol.declared:symbol.declared.substr(0,symbol.declared.size()-1)):symbol.declared;}}
        }
        if(n.kind==N::MultiBinding)for(std::size_t i=0;i<list.children.size();++i){const auto& p=*list.children[i];if(p.text=="_")continue;auto t=model_.types.at(&p);bool writable=n.text=="var";
            if(t.starts_with('[')||t.starts_with("map<")||t.starts_with("Result<")||t.starts_with("List<")||t=="Buffer"){const Node& source=n.children[1]->kind==N::Pack?*n.children[1]->children[i]:*n.children[1];writable=writable&&(node_positions_.contains(&source)&&i<node_positions_.at(&source).size()?node_positions_.at(&source)[i].writable:model_.writable.at(&source));}
            define(p.text,{t,n.text=="var",writable,{}},p.span);const auto& source=n.children[1]->kind==N::Pack?*n.children[1]->children[i]:*n.children[1];Node destination(N::Name,p.span,p.text);auto value=node_positions_.contains(&source)&&i<node_positions_.at(&source).size()?node_positions_.at(&source)[i]:infos_.at(&source);value.type=t;
            auto original=node_views_[&source];if(node_position_views_.contains(&source)&&i<node_position_views_.at(&source).size())node_views_[&source]=node_position_views_.at(&source)[i];remember(destination,source,value);node_views_[&source]=std::move(original);}

        if(n.kind==N::MultiAssignment)for(std::size_t i=0;i<list.children.size();++i){
            const auto& p=*list.children[i];if(p.text=="_")continue;
            const auto& source=n.children[1]->kind==N::Pack?*n.children[1]->children[i]:*n.children[1];
            auto value=node_positions_.contains(&source)&&i<node_positions_.at(&source).size()?node_positions_.at(&source)[i]:infos_.at(&source);value.type=targets[i].type;
            if(p.kind==N::Name&&(value.type.starts_with('[')||value.type.starts_with("map<")||value.type.starts_with("Result<")||value.type.starts_with("List<")||value.type=="Buffer"))lookup(p.text,p.span).writable=value.writable;
            auto original=node_views_[&source];if(node_position_views_.contains(&source)&&i<node_position_views_.at(&source).size())node_views_[&source]=node_position_views_.at(&source)[i];
            remember(p,source,value);if(p.kind==N::Index)update_aliases(infos_.at(p.children[0].get()),"[]",value,node_views_[&source]);node_views_[&source]=std::move(original);
        }
        break;
    }
    case N::Defer:{auto previous=defer_site_;defer_site_=n.children[0].get();expression(*n.children[0]);defer_site_=previous;break;}
    case N::ExpressionStatement:expression(*n.children[0]);break;
    case N::Match:{
        auto info=expression(*n.children[0]);auto structure=model_.structures.find(info.type);bool enumeration=structure!=model_.structures.end()&&structure->second->text.find(" enum")!=std::string::npos;
        auto match_before=HUA_SEMA_SNAPSHOT(scopes_,branch_snapshot);std::vector<std::vector<std::unordered_map<std::string,Symbol>>> match_exits;
        auto cases=enumeration?enum_cases(*structure->second):std::vector<std::pair<std::string,std::vector<std::string>>>{};std::set<std::string> seen;bool wildcard=false;
        if(n.children.size()<2)error(n,"match requires at least one arm","E3011");
        for(std::size_t i=1;i<n.children.size();++i){const auto& arm=*n.children[i];const auto& pattern=*arm.children[0];auto name=pattern.text;bool any=pattern.kind==N::Name&&display_type(name)=="_";if(wildcard)error(arm,"unreachable match arm after _","E3011");HUA_SEMA_RESTORE(scopes_,match_before,scope_restore);scopes_.emplace_back();
            if(any){wildcard=true;if(!arm.children[1]->children.empty())error(arm,"wildcard cannot bind payloads","E3011");}
            else if(enumeration){auto dot=name.rfind('.');if(dot!=std::string::npos){if(name.substr(0,dot)!=info.type)error(pattern,"variant belongs to another enum","E3011");name=name.substr(dot+1);}name=display_type(name);auto found=std::find_if(cases.begin(),cases.end(),[&](const auto& c){return c.first==name;});if(found==cases.end())error(pattern,"unknown enum variant","E3011");if(!seen.insert(name).second)error(arm,"duplicate match variant","E3011");if(found->second.size()!=arm.children[1]->children.size())error(arm,"enum payload binding count mismatch","E3011");model_.constants[&pattern]=Value(name);for(std::size_t j=0;j<found->second.size();++j){const auto& binding=*arm.children[1]->children[j];model_.types[&binding]=found->second[j];if(binding.text!="_")define(binding.text,{found->second[j],false,false,{}},binding.span);}}
            else {if(pattern.kind==N::Name||!arm.children[1]->children.empty())error(pattern,"scalar match requires literal patterns","E3011");auto value=constant(pattern);require(info.type,{value_type(value),false},pattern);auto key=value_type(value)+":"+show(value);if(!seen.insert(key).second)error(arm,"duplicate match literal","E3011");model_.constants[&pattern]=value;}
            if(info.type.ends_with('?')&&n.children[0]->kind==N::Name&&(pattern.kind==N::Nil||(any&&seen.contains("nil:nil")))){auto& symbol=lookup(n.children[0]->text,n.span);bool local=false;for(std::size_t scope=2;scope<scopes_.size();++scope)for(auto& [_,candidate]:scopes_[scope])if(&candidate==&symbol)local=true;if(local&&symbol.declared.ends_with('?'))symbol.type=pattern.kind==N::Nil?"nil":symbol.declared.substr(0,symbol.declared.size()-1);}
            statement(*arm.children[2]);scopes_.pop_back();if(!flow_stops(*arm.children[2]))match_exits.push_back(HUA_SEMA_SNAPSHOT(scopes_,branch_snapshot));
        }
        if(!wildcard&&!(enumeration&&seen.size()==cases.size())&&!(info.type=="bool"&&seen.size()==2))error(n,"non-exhaustive match requires missing variants or _","E3011");if(match_exits.empty())HUA_SEMA_RESTORE(scopes_,match_before,scope_restore);else{HUA_SEMA_RESTORE(scopes_,match_exits.front(),scope_restore);for(std::size_t i=1;i<match_exits.size();++i)join_flow(match_exits[i]);}model_.types[&n]=info.type;break;
    }
    case N::If:{require("bool",expression(*n.children[0]),*n.children[0]);auto before=HUA_SEMA_SNAPSHOT(scopes_,branch_snapshot);narrow(*n.children[0],true);statement(*n.children[1]);auto yes=HUA_SEMA_SNAPSHOT(scopes_,branch_snapshot);HUA_SEMA_RESTORE(scopes_,before,scope_restore);narrow(*n.children[0],false);if(n.children.size()==3)statement(*n.children[2]);bool yes_stops=flow_stops(*n.children[1]),no_stops=n.children.size()==3&&flow_stops(*n.children[2]);if(no_stops&&!yes_stops)scopes_=std::move(yes);else if(!yes_stops&&!no_stops)join_flow(yes);break;}
    case N::While:{invalidate_loop(*n.children[1]);require("bool",expression(*n.children[0]),*n.children[0]);auto before=HUA_SEMA_SNAPSHOT(scopes_,branch_snapshot);std::size_t flow_budget=1;for(const auto& scope:scopes_)flow_budget+=scope.size();for(std::size_t pass=0;pass<flow_budget;++pass){HUA_SEMA_COUNT(loop_passes);HUA_SEMA_RESTORE(scopes_,before,scope_restore);narrow(*n.children[0],true);statement(*n.children[1]);auto after=HUA_SEMA_SNAPSHOT(scopes_,loop_snapshot);HUA_SEMA_RESTORE(scopes_,before,scope_restore);join_flow(after);bool changed=false;for(std::size_t i=0;i<scopes_.size();++i)for(const auto& [name,symbol]:scopes_[i])changed=changed||symbol.type!=before[i].at(name).type||symbol.writable!=before[i].at(name).writable||symbol.readonly_children!=before[i].at(name).readonly_children||symbol.views!=before[i].at(name).views;HUA_SEMA_RESTORE(before,scopes_,loop_snapshot);if(!changed)break;}invalidate_loop(*n.children[1]);bool has_break=false;std::function<void(const Node&)> find=[&](const Node& node){if(node.kind==N::Break)has_break=true;if(node.kind==N::Function||node.kind==N::While||node.kind==N::For)return;for(const auto& c:node.children)find(*c);};find(*n.children[1]);if(!has_break)narrow(*n.children[0],false);break;}
    case N::For: {
        invalidate_loop(*n.children.back());auto loop_before=HUA_SEMA_SNAPSHOT(scopes_,loop_snapshot);
        auto count=n.children.size()-2;const auto& iterable=*n.children[count];std::string item,key="int";
        if(iterable.kind==N::Range){for(const auto& c:iterable.children)require("int",expression(*c),*c);item="int";}
        else {auto info=expression(iterable);auto map=type_arguments(info.type,"map");if(map.size()==2){key=map[0];item=count==1?key:map[1];}else {item=element_type(info.type);if(!info.type.empty() && !info.type.starts_with('['))error(iterable,"for requires a range, slice or map","E3004");}}
        scopes_.emplace_back();
        for(std::size_t i=0;i<count;++i)define(n.children[i]->text,{count==2 && i==0 ? key:item,false,false,{}},n.children[i]->span);
        statements(*n.children.back(),false);scopes_.pop_back();join_flow(loop_before);auto loop_entry=HUA_SEMA_SNAPSHOT(scopes_,loop_snapshot);std::size_t budget=1;for(const auto& scope:scopes_)budget+=scope.size();for(std::size_t pass=0;pass<budget;++pass){HUA_SEMA_COUNT(loop_passes);HUA_SEMA_RESTORE(scopes_,loop_entry,scope_restore);scopes_.emplace_back();for(std::size_t i=0;i<count;++i)define(n.children[i]->text,{count==2&&i==0?key:item,false,false,{}},n.children[i]->span);statements(*n.children.back(),false);scopes_.pop_back();join_flow(loop_entry);bool changed=false;for(std::size_t i=0;i<scopes_.size();++i)for(const auto& [name,symbol]:scopes_[i])changed=changed||symbol.type!=loop_entry[i].at(name).type||symbol.writable!=loop_entry[i].at(name).writable||symbol.readonly_children!=loop_entry[i].at(name).readonly_children||symbol.views!=loop_entry[i].at(name).views;HUA_SEMA_RESTORE(loop_entry,scopes_,loop_snapshot);if(!changed)break;}invalidate_loop(*n.children.back());break;
    }
    case N::Return: {
        const Node* returns=nullptr;for(const auto& c:current_function_->children)if(c->kind==N::ReturnTypes)returns=c.get();
        bool void_return=returns && returns->children.size()==1 && type_name(*returns->children[0])=="void";
        if(returns&&returns->children.size()>1&&n.children.size()==1){auto info=expression(*n.children[0]);auto ts=type_arguments(info.type,"multi");if(!info.type.empty()&&ts.size()!=returns->children.size())error(n,"return value count does not match function signature","E3006");for(std::size_t i=0;i<ts.size();++i)require(type_name(*returns->children[i]),{ts[i],false},*n.children[0]);if(probing_){capture_return(n);capture_effect_exit();}break;}
        if(returns && (void_return ? 0:returns->children.size())!=n.children.size())error(n,"return value count does not match function signature","E3006");
        for(std::size_t i=0;i<n.children.size();++i){auto info=expression(*n.children[i]);if(returns)require(type_name(*returns->children[i]),info,*n.children[i]);}if(probing_){capture_return(n);capture_effect_exit();}break;
    }
    case N::ExternalInit:if(!model_.external || !model_.external->module_exists(n.text))error(n,"unregistered external initializer","E7003");break;
    case N::Break:case N::Continue:break;
    case N::Unsafe:statement(*n.children[0]);break;
    case N::Import:{auto name=n.children.empty()?n.text.substr(0,n.text.find('.')):n.children[0]->text;define(name,{"module",false,false,{}},n.span);break;}
    case N::Function: {
        auto name=declared_name(n);model_.functions.emplace(name,&n);model_.nested[&n]=true;
        define(name,{"fn:"+name,false,false,{}},n.span);
        auto outer=current_function_;auto saved=HUA_SEMA_SNAPSHOT(scopes_,closure);auto previous_return=pending_return_;auto previous_effects=pending_effects_;auto previous_exits=std::move(exit_views_);auto previous_roots=std::move(effect_roots_);bool previous_seen=saw_return_;pending_return_={};pending_effects_={};effect_roots_.clear();exit_views_.clear();saw_return_=false;current_function_=&n;
        // Closures capture readonly snapshots at declaration, rather than dangling stack environments.
        for(std::size_t i=2;i<scopes_.size();++i)for(auto& [_,symbol]:scopes_[i]){symbol.mutable_binding=false;symbol.writable=false;}
        scopes_.emplace_back();
        for(const auto* p:parameters(n)){if(!p->children.empty())validate_type(*p->children[0]);define(declared_name(*p),{p->children.empty()?"":type_name(*p->children[0]),false,mutable_parameter(*p),{}},p->span);seed(declared_name(*p),p,!mutable_parameter(*p));}
        for(const auto& c:n.children)if(c->kind==N::ReturnTypes)for(const auto& t:c->children)validate_type(*t,c->children.size()==1);
        if(probing_)select_effect_roots();
        statements(*n.children.back(),false);
        if(probing_){capture_effect_exit();finish_effects();}
        auto result=result_type(n);if(!result.empty()&&result!="void"&&!result.ends_with('?')&&!definitely_returns(*n.children.back()))error(n,"typed function may reach its end without returning a value","E3006");
        if(probing_){return_facts_[name]=pending_return_;effects_[name]=pending_effects_;}
        pending_return_=std::move(previous_return);pending_effects_=std::move(previous_effects);exit_views_=std::move(previous_exits);effect_roots_=std::move(previous_roots);saw_return_=previous_seen;scopes_=std::move(saved);current_function_=outer;break;
    }
    case N::Struct:error(n,"local struct requires lexical type lowering","E3008");
    default:error(n,"unexpected statement in semantic analysis","E3008");
    }
}
SemanticAnalyzer::Info SemanticAnalyzer::target(const Node& n) {
    if(n.kind==N::Name) {auto& s=lookup(n.text,n.span);if(!s.mutable_binding)error(n,"cannot assign to immutable binding `"+n.text+"`","E3003");return {s.declared,true};}
    auto info=expression(n);
    if(n.kind==N::Member||n.kind==N::Index)info.writable=model_.writable.at(n.children[0].get());
    if(!info.writable&&!probing_)error(n,"cannot mutate a read-only value/view","E3003");
    note_write(n);
    if(n.kind==N::Index){auto t=type_arguments(model_.types.at(n.children[0].get()),"map");if(t.size()==2)info.type=t[1];}
    return info;
}
SemanticAnalyzer::Info SemanticAnalyzer::expression(const Node& n) {
    node_views_.erase(&n);node_positions_.erase(&n);node_position_views_.erase(&n);
    Info out;
    switch(n.kind) {
    case N::Integer:out={"int",false};break;case N::Float:out={"float",false};break;
    case N::String:out={"string",false};break;case N::Boolean:out={"bool",false};break;case N::Nil:out={"nil",false};break;
    case N::Name:{auto& symbol=lookup(n.text,n.span);out={symbol.type,symbol.writable,symbol.readonly_children,symbol.origins,symbol.deep_owned,symbol.record_copy};node_views_[&n]=symbol.views;break;}
    case N::Pack: {
        std::string type="multi<";bool writable=true;for(std::size_t i=0;i<n.children.size();++i){auto x=expression(*n.children[i]);if(x.type=="void")error(n,"void cannot be packed as a value","E3004");if(!type_arguments(x.type,"multi").empty())error(n,"nested multiple values are not supported","E3006");if(i)type+=',';type+=x.type;if(x.type.starts_with('[')||x.type.starts_with("map<")||x.type.starts_with("Result<")||x.type.starts_with("List<")||x.type=="Buffer")writable=writable&&x.writable;}out={type+">",writable};break;
    }
    case N::MapLiteral: {
        validate_type(*n.children[0]);auto type=type_name(*n.children[0]);auto ts=type_arguments(type,"map");
        Info item;Views views;bool first=true;
        for(std::size_t i=1;i<n.children.size();++i){const auto& e=*n.children[i];require(ts[0],expression(*e.children[0]),*e.children[0]);auto value=expression(*e.children[1]);require(ts[1],value,*e.children[1]);
            auto children=node_views_.contains(e.children[1].get())?node_views_.at(e.children[1].get()):fields(value);if(!value.writable)for(auto& [path,view]:children){view.writable=false;view.readonly_children=true;if(record(view.type))view=copied(view);}
            for(auto& [_,view]:children){if(!value.writable){view.writable=false;view.readonly_children=true;}if(record(value.type)&&record(view.type))view=copied(view);}
            value=copied(value);value.type=ts[1];if(first){item=value;views=children;first=false;}else{views=meet(std::move(views),children,item,value);item=meet(item,value);}}
        if(!first){node_views_[&n]["[]"]=item;for(auto [path,view]:views)node_views_[&n]["[]"+path]=std::move(view);}out={type,true};break;
    }
    case N::Propagate: {
        auto value=expression(*n.children[0]);auto ts=type_arguments(value.type,"Result"),returns=current_function_?type_arguments(result_type(*current_function_),"Result"):std::vector<std::string>{};
        if(ts.size()!=2||returns.size()!=2)error(n,"Result propagation requires a Result value and Result-returning function","E3006");
        if(ts[0].empty())error(n,"Result success type is unknown; add a type annotation","E3004");
        require(returns[1],{ts[1],false},n);out={ts[0],value.writable,value.readonly_children};break;
    }
    case N::Array: {
        std::string element;
        for(const auto& c:n.children){auto x=expression(*c);if(x.type=="void")error(*c,"void cannot be an array element","E3004");if(element.empty())element=x.type;else require(element,x,*c);}
        out={"[]"+element,true};Info item;Views item_views;bool first=true;
        for(const auto& child:n.children){const auto& source=infos_.at(child.get());auto value=copied(source);auto views=node_views_.contains(child.get())?node_views_.at(child.get()):fields(source);
            // Record copies own their scalar/record fields, but inherit shared
            // descendants from the original element before it becomes writable.
            for(auto& [_,view]:views){if(!source.writable){view.writable=false;view.readonly_children=true;}if(record(source.type)&&record(view.type))view=copied(view);}
            if(first){item=value;item_views=views;first=false;}else{item_views=meet(std::move(item_views),views,item,value);item=meet(std::move(item),value);}}
        if(!first){node_views_[&n]["[]"]=item;for(auto [path,view]:item_views)node_views_[&n]["[]"+path]=std::move(view);}
        out.origins={{&n,"",false,false}};
        break;
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
        auto a=expression(*n.children[0]);auto op=n.text;
        // Only conditional evaluation needs an alternate flow state. Eager operators
        // analyze both operands in order and never read a saved scope.
        decltype(scopes_) before;
        if(op=="&&"||op=="||"){before=HUA_SEMA_BINARY(scopes_,op);narrow(*n.children[0],op=="&&");}
        auto b=expression(*n.children[1]);if(op=="&&"||op=="||")join_flow(before);
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
            if(n.text=="="){remember(*n.children[0],*n.children[1],rhs);if(n.children[0]->kind==N::Index)update_aliases(infos_.at(n.children[0]->children[0].get()),"[]",rhs,node_views_[n.children[1].get()]);}
        } else if(!numeric_spec(lhs.type)||numeric_spec(lhs.type)->category=='f')require("int",lhs,n);
        out={lhs.type,false};break;
    }
    case N::Index:case N::Slice: {
        auto base=expression(*n.children[0]);auto element=element_type(base.type);auto map=type_arguments(base.type,"map");
        if(n.kind==N::Index&&map.size()==2){require(map[0],expression(*n.children[1]),*n.children[1]);auto type=map[1].ends_with('?')?map[1]:map[1]+"?";out={type,base.writable&&!base.readonly_children,base.readonly_children};
            auto views=node_views_.contains(n.children[0].get())?node_views_.at(n.children[0].get()):fields(base);if(base.writable&&views.contains("[]"))out=views.at("[]");out.type=type;out.record_copy=false;
            for(auto [path,view]:views)if(path.starts_with("[].")||path.starts_with("[][]"))node_views_[&n][path.substr(2)]=std::move(view);if(out.origins.empty())out.origins=projected(base,"[]").origins;break;}
        if(n.kind==N::Index&&base.type.empty()){expression(*n.children[1]);out={"",base.writable};break;}
        if(!base.type.empty() && !base.type.starts_with('['))error(n,"index/slice requires a slice or array","E3004");
        for(std::size_t i=1;i<n.children.size();++i)if(n.children[i]->kind!=N::Omitted)require("int",expression(*n.children[i]),*n.children[i]);
        out={n.kind==N::Slice ? "[]"+element:element,base.writable&&!base.readonly_children,base.readonly_children};
        auto views=node_views_.contains(n.children[0].get())?node_views_.at(n.children[0].get()):fields(base);
        // Capability views can retain the literal's pre-conversion type. The container
        // type determines the element layout; reuse only the view's capabilities.
        if(n.kind==N::Index){if(base.writable)if(auto it=views.find("[]");it!=views.end())out=it->second;out.type=element;out.record_copy=false;
            for(auto [path,view]:views)if(path.starts_with("[].")||path.starts_with("[][]"))node_views_[&n][path.substr(2)]=std::move(view);
            if(out.origins.empty())out.origins=projected(base,"[]").origins;
        }else {out.origins=base.origins;node_views_[&n]=views;}break;
    }
    case N::StructLiteral: {
        if(n.text=="std.error.Value")error(n,"standard Error must be constructed through std.error","E3004");
        if(n.text=="std.file.File"||n.text=="std.file.IOFailure")error(n,"opaque File/IOFailure must be constructed through std.file","E3004");
        auto found=model_.structures.find(n.text);if(found==model_.structures.end())error(n,"unknown struct `"+n.text+"`","E3004");
        if(found->second->text.find(" interface")!=std::string::npos)error(n,"interface cannot be constructed directly","E3004");
        std::unordered_set<std::string> seen;
        for(const auto& init:n.children) {
            if(!seen.insert(init->text).second)error(*init,"duplicate field initializer","E3002");
            const Node* field=nullptr;for(const auto& f:found->second->children)if(f->text==init->text)field=f.get();
            if(!field)error(*init,"unknown field `"+init->text+"`","E3004");
            auto item=expression(*init->children[0]);require(type_name(*field->children[0]),item,*init->children[0]);
            node_views_[&n]["."+init->text]=copied(item);node_views_[&n]["."+init->text].type=type_name(*field->children[0]);
            auto nested=node_views_.contains(init->children[0].get())?node_views_.at(init->children[0].get()):fields(item);
            for(auto [path,view]:nested){if(!item.writable){view.writable=false;view.readonly_children=true;if(record(view.type))view=copied(view);}node_views_[&n]["."+init->text+path]=std::move(view);}
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
        for(const auto& field:structure->second->children)if(field->text==n.text){out=projected(base,"."+n.text);out.type=type_name(*field->children[0]);
            auto views=node_views_.contains(n.children[0].get())?node_views_.at(n.children[0].get()):fields(base);
            if(base.writable)if(auto it=views.find("."+n.text);it!=views.end())out=it->second;
            for(auto [path,view]:views)if(path.starts_with("."+n.text+".")||path.starts_with("."+n.text+"[]"))node_views_[&n][path.substr(n.text.size()+1)]=std::move(view);
            found=true;break;}
        if(!found){auto method=type+"."+n.text;if(!model_.functions.contains(method))error(n,"unknown field/method `"+n.text+"`","E3004");
            auto definition=model_.functions.at(method);
            if(definition->span.file_id!=n.span.file_id && definition->text.find(" pub")==std::string::npos)
                error(n,"method is private to its defining module","E5003");
            if(model_.mutating[method] && !base.writable&&!probing_)error(n,"mutating method requires a mutable receiver","E3003");out={"fn:"+method,false};}
        break;
    }
    case N::Call: {
        auto callee=expression(*n.children[0]);std::vector<Info> args;
        for(std::size_t i=1;i<n.children.size();++i){auto a=expression(*n.children[i]);if(!type_arguments(a.type,"multi").empty())error(n,"multiple values require positional binding","E3006");if(a.type=="void")error(*n.children[i],"void cannot be passed as a value","E3004");args.push_back(a);}
        if(defer_site_==&n)for(auto& arg:args)arg=copied(arg);
        if(callee.type.starts_with("fn:")) {
            auto key=callee.type.substr(3);auto fn=model_.functions.at(key);if(fn->text.find(" abstract")!=std::string::npos&&n.children[0]->kind!=N::Member)error(n,"interface prototype requires a concrete receiver","E3004");auto ps=parameters(*fn);
            if(ps.size()!=args.size())error(n,"function argument count mismatch","E3005");
            for(std::size_t i=0;i<args.size();++i) {
                if(!ps[i]->children.empty())require(type_name(*ps[i]->children[0]),args[i],*n.children[i+1]);
                if(mutable_parameter(*ps[i])&&!ps[i]->children.empty()&&!args[i].type.empty()){auto annotation=type_name(*ps[i]->children[0]);if(annotation.starts_with('[')&&(element_type(annotation)!=element_type(args[i].type)||(annotation.find(']')>1&&args[i].type.starts_with("[]"))))error(*n.children[i+1],"mut borrow requires identical element type and existing fixed array","E3004");}
                if(mutable_parameter(*ps[i]) && !args[i].writable&&!probing_)error(*n.children[i+1],"mut parameter requires mutable argument","E3003");
            }
            out={result_type(*fn),model_.return_writable.contains(key)&&model_.return_writable.at(key)};
            auto previous_origins=HUA_SEMA_SNAPSHOT(actual_origins_,call_actuals);auto previous_views=HUA_SEMA_SNAPSHOT(actual_views_,call_actuals);
            for(std::size_t layer=0;layer<std::min<std::size_t>(2,scopes_.size());++layer)for(const auto& [name,symbol]:scopes_[layer])if(symbol.declaration){actual_origins_[symbol.declaration]={symbol.type,symbol.writable,symbol.readonly_children,symbol.origins,symbol.deep_owned};actual_views_[symbol.declaration]=symbol.views;}
            for(std::size_t i=0;i<ps.size();++i){actual_origins_[ps[i]]=args[i];actual_views_[ps[i]]=node_views_[n.children[i+1].get()];}
            if(n.children[0]->kind==N::Member){const auto& receiver=*n.children[0]->children[0];actual_origins_[fn]=infos_.at(&receiver);actual_views_[fn]=node_views_[&receiver];}
#ifdef HUA_SEMA_PROFILE
            sema_profile::dependency(key);
#endif
            if(auto facts=return_facts_.find(key);facts!=return_facts_.end()){
                auto value=instantiate(facts->second.value,n);value.type=out.type;out=std::move(value);node_views_[&n]=instantiate(facts->second.views,n);
                node_positions_[&n].clear();for(auto position:facts->second.positions)node_positions_[&n].push_back(instantiate(std::move(position),n));
                for(auto views:facts->second.position_views)node_position_views_[&n].push_back(instantiate(std::move(views),n));
            }
            if(auto effect=effects_.find(key);effect!=effects_.end())apply_effects(effect->second,n);
            actual_origins_=std::move(previous_origins);actual_views_=std::move(previous_views);
        } else if(callee.type.starts_with("builtin:")) {
            auto name=callee.type.substr(8);
            if(name=="task_start"){
                if(args.empty()||!args[0].type.starts_with("fn:"))error(n,"spawn requires a Hua function","E3004");
                auto fn=model_.functions.at(args[0].type.substr(3));auto ps=parameters(*fn);if(ps.size()+1!=args.size())error(n,"spawn argument count mismatch","E3005");
                if(fn->text.find(" external")!=std::string::npos||fn->text.find(" unsafe")!=std::string::npos)error(n,"Native/WASM functions cannot be spawned","E3008");
                for(std::size_t i=0;i<ps.size();++i){if(mutable_parameter(*ps[i]))error(n,"spawn cannot pass mutable borrows","E3003");if(!ps[i]->children.empty())require(type_name(*ps[i]->children[0]),args[i+1],*n.children[i+2]);}
                for(std::size_t i=1;i<args.size();++i)if(file_type(args[i].type,model_))error(n,"spawn arguments cannot contain File","E3008");
                auto r=result_type(*fn);if(file_type(r,model_))error(n,"spawn results cannot contain File","E3008");if(r.empty()){bool value_return=false;std::function<void(const Node&)> scan=[&](const Node& c){if(c.kind==N::Function)return;if(c.kind==N::Return&&!c.children.empty())value_return=true;for(const auto& item:c.children)scan(*item);};scan(*fn->children.back());if(value_return)error(n,"spawned value functions require an explicit return type","E3004");}out={r.starts_with("Task<")&&!declared_name(*fn).starts_with("$async$")?r:"Task<"+(r.empty()?"void":r)+">",false};break;
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
                if(f->mutates_first&&!args[0].writable&&!probing_)error(n,"standard mutation requires a writable argument","E3003");
                if(f->mutates_first&&probing_)for(const auto& origin:args[0].origins)pending_effects_.required.insert(origin);
                out={signature.result,f->writable_result};break;
            }
            const std::size_t arity=(name=="min"||name=="max"||name=="has"||name=="delete"||name=="unwrap_or")?2:name=="clamp"?3:1;
            if(name!="print" && !(name=="ok"&&args.empty()) && args.size()!=arity)error(n,"builtin argument count mismatch","E3005");
            if(numeric_spec(name)&&name!="int"&&name!="float"){if(!args[0].type.empty()&&!numeric_spec(args[0].type)&&args[0].type!="string")error(n,"numeric conversion requires a number or string","E3004");out={name,false};break;}
            if(name=="ok"&&args.empty()){out={"Result<void,>",false};break;}
            if(name=="ok"||name=="err"){out={name=="ok"?"Result<"+args[0].type+",>":"Result<,"+args[0].type+">",args[0].writable};if(args[0].type!="void"&&args[0].type!="nil"&&!args[0].type.starts_with('[')&&!args[0].type.starts_with("map<")&&!args[0].type.starts_with("List<")&&args[0].type!="Buffer"&&!args[0].type.starts_with("Result<")&&!record(args[0].type))out.writable=true;out.readonly_children=args[0].readonly_children;if(args[0].type=="void")error(n,"Result payload cannot be void","E3004");break;}
            if(name=="is_some"||name=="is_none"){out={"bool",false};break;}
            if(name=="is_ok"||name=="is_err"||name=="unwrap_err"){auto ts=type_arguments(args[0].type,"Result");if(!args[0].type.empty()&&ts.size()!=2)error(n,"expected Result","E3004");out={name=="unwrap_err"?(ts.empty()?"":ts[1]):"bool",args[0].writable,args[0].readonly_children};break;}
            if(name=="unwrap"||name=="unwrap_or"){auto ts=type_arguments(args[0].type,"Result");std::string type=ts.size()==2?ts[0]:args[0].type;if(ts.empty()&&type.ends_with('?'))type.pop_back();if(type=="nil")type="";if(name=="unwrap_or"){if(type=="void")error(n,"Result<void> has no fallback value","E3004");require(type,args[1],*n.children[2]);if(type.empty())type=args[1].type;}out={type,args[0].writable&&(name=="unwrap"||args[1].writable),args[0].readonly_children||(name=="unwrap_or"&&args[1].readonly_children)};break;}
            if(name=="has"||name=="delete"){auto ts=type_arguments(args[0].type,"map");if(!args[0].type.empty()&&ts.size()!=2)error(n,"expected map","E3004");if(!ts.empty())require(ts[0],args[1],*n.children[2]);if(name=="delete"&&!args[0].writable&&!probing_)error(n,"cannot mutate a read-only map","E3003");if(name=="delete"&&probing_)for(const auto& origin:args[0].origins)pending_effects_.required.insert(origin);out={"bool",false};break;}
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
            else if(name=="clone"){if(file_type(args[0].type,model_))error(n,"File cannot be cloned, including inside containers","E3008");out={args[0].type,true};}else out={args[0].type,false};
            if(name=="min" || name=="max" || name=="clamp")for(std::size_t i=1;i<args.size();++i)require(args[0].type,args[i],*n.children[i+1]);
        } else if(!callee.type.empty())error(n,"value is not callable","E3004"); else {if(probing_)pending_effects_.unknown=true;forget_unknown(n);out={"",false};}
        break;
    }
    default:error(n,"expression is not supported by semantic analysis","E3008");
    }
    if(n.kind==N::Call&&n.children.size()>1&&n.children[0]->kind==N::Name){auto key=n.children[0]->text;if(key.starts_with("$core$"))key=key.substr(6);
        if(key=="clone"){out.deep_owned=true;out.origins={{&n,"",false,false}};node_views_[&n]=fields(out);}
        else if(key=="ok"||key=="err"||key=="unwrap"||key=="unwrap_err"){
            const auto& input=*n.children[1];out.origins=infos_.at(&input).origins;out.deep_owned=infos_.at(&input).deep_owned;
            if(node_views_.contains(&input))node_views_[&n]=node_views_.at(&input);
        }
    }
    if(out.origins.empty()&&(n.kind==N::MapLiteral||n.kind==N::Array))out.origins={{&n,"",false,false}};
    if(!node_views_.contains(&n)&&record(out.type))node_views_[&n]=fields(out);
    if(node_views_.contains(&n))bound(node_views_[&n],out);
    infos_[&n]=out;model_.types[&n]=out.type;model_.writable[&n]=out.writable;return out;
}
void SemanticAnalyzer::join_flow(const std::vector<std::unordered_map<std::string,Symbol>>& other) {
    HUA_SEMA_AREA(join);
    for(std::size_t i=0;i<scopes_.size()&&i<other.size();++i)for(auto& [name,symbol]:scopes_[i]) {
        auto found=other[i].find(name);if(found==other[i].end())continue;
        if(symbol.type!=found->second.type)symbol.type=symbol.declared;
        symbol.writable=symbol.writable&&found->second.writable;
        symbol.readonly_children=symbol.readonly_children||found->second.readonly_children;symbol.origins.insert(found->second.origins.begin(),found->second.origins.end());symbol.deep_owned=symbol.deep_owned&&found->second.deep_owned;
        auto previous=HUA_SEMA_SNAPSHOT(symbol.views,join);
        for(const auto& [path,view]:found->second.views) {
            auto& merged=symbol.views[path];
            if(!previous.contains(path)){merged=projected(Info{found->second.type,found->second.writable,found->second.readonly_children,found->second.origins,found->second.deep_owned,found->second.record_copy},path);merged.type=view.type;}
            merged=meet(merged,view);
        }
        for(const auto& [path,view]:previous)if(!found->second.views.contains(path)) {
            auto& merged=symbol.views[path];
            auto fallback=projected(Info{found->second.type,found->second.writable,found->second.readonly_children,found->second.origins,found->second.deep_owned,found->second.record_copy},path);fallback.type=view.type;merged=meet(merged,fallback);
        }
        Info value{symbol.type,symbol.writable,symbol.readonly_children,symbol.origins,symbol.deep_owned};bound(symbol.views,value);symbol.readonly_children=value.readonly_children;
    }
}
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


void SemanticAnalyzer::bound(Views& views,Info& root) const{
    auto depth=[](const std::string& path){return std::count(path.begin(),path.end(),'.')+std::count(path.begin(),path.end(),'[');};
    std::vector<std::string> removed;
    for(auto it=views.begin();it!=views.end();)if(depth(it->first)>8){removed.push_back(it->first);it=views.erase(it);}else ++it;
    for(const auto& path:removed){bool marked=false;
        for(auto& [prefix,value]:views)if(depth(prefix)==8&&path.starts_with(prefix)&&!value.deep_owned){value.readonly_children=true;marked=true;}
        if(!marked&&!root.deep_owned)root.readonly_children=true;
    }
}
SemanticAnalyzer::Info SemanticAnalyzer::meet(Info a,const Info& b){
    a.writable=a.writable&&b.writable;a.readonly_children=a.readonly_children||b.readonly_children;
    a.origins.insert(b.origins.begin(),b.origins.end());a.deep_owned=a.deep_owned&&b.deep_owned;a.record_copy=a.record_copy&&b.record_copy;return a;
}
SemanticAnalyzer::Views SemanticAnalyzer::meet(Views a,const Views& b,const Info& left,const Info& right){
    for(const auto& [path,value]:b){auto it=a.find(path);if(it==a.end())a[path]=meet(Info{value.type,left.writable&&!left.readonly_children,left.readonly_children},value);else it->second=meet(it->second,value);}
    for(auto& [path,value]:a)if(!b.contains(path))value=meet(value,Info{value.type,right.writable&&!right.readonly_children,right.readonly_children});return a;
}
SemanticAnalyzer::Info SemanticAnalyzer::projected(Info base,const std::string& path) const{
    Info out{"",base.writable&&!base.readonly_children,base.readonly_children||!base.writable};out.deep_owned=base.deep_owned;
    for(auto origin:base.origins){origin.path+=path;if(origin.path.size()>512){origin.readonly=true;origin.path.clear();}origin.copied=false;out.origins.insert(std::move(origin));}return out;
}
SemanticAnalyzer::Views SemanticAnalyzer::fields(Info base,unsigned depth) const{
    HUA_SEMA_AREA(fields);
    // Cache only type/capability shapes. Actual source owners stay per analysis
    // and per value, never in the cache or across analyzed trees.
    HUA_SEMA_COUNT(field_requests);
    auto key=base.type+"|"+std::to_string(depth)+":"+std::to_string(base.writable)+std::to_string(base.readonly_children)+std::to_string(base.deep_owned)+std::to_string(base.record_copy);
    auto prototype=field_shapes_.find(key);
    if(prototype!=field_shapes_.end())HUA_SEMA_COUNT(field_hits);
    if(prototype==field_shapes_.end()){HUA_SEMA_COUNT(field_builds);auto shape=base;shape.origins.clear();prototype=field_shapes_.emplace(std::move(key),field_shape(std::move(shape),depth)).first;}
    auto out=prototype->second;
#ifdef HUA_SEMA_PROFILE
    sema_profile::dimensions(out.size(),base.origins.size());
#endif
    for(auto& [path,value]:out){std::set<Origin> origins;
        for(auto origin:base.origins){
            // Match projected() overflow handling at every member/index step.
            for(std::size_t begin=0;begin<path.size();){auto end=begin+1;while(end<path.size()&&path[end]!='.'&&path[end]!='[')++end;origin.path+=path.substr(begin,end-begin);if(origin.path.size()>512){origin.readonly=true;origin.path.clear();}begin=end;}
            origin.copied=value.record_copy;origins.insert(std::move(origin));
        }
        value.origins=std::move(origins);
    }
    return out;
}
SemanticAnalyzer::Views SemanticAnalyzer::field_shape(Info base,unsigned depth) const{
    Views out;if(depth>=8)return out;
    auto type=unref(base.type);if(type.ends_with('?'))type.pop_back();
    for(auto wrapper:{"Result","Task"}){auto args=type_arguments(type,wrapper);if(!args.empty())type=args[0];}
    if(auto record=model_.structures.find(type);record!=model_.structures.end()){
        for(const auto& field:record->second->children){auto path="."+field->text;auto value=projected(base,path);value.type=type_name(*field->children[0]);
            if(this->record(value.type)&&base.record_copy)value=copied(value);
            out[path]=value;for(auto [nested,view]:field_shape(value,depth+1))out[path+nested]=std::move(view);
        }
    }else{
        auto element=element_type(type);auto map=type_arguments(type,"map"),list=type_arguments(type,"List");if(!map.empty())element=map[1];if(!list.empty())element=list[0];
        if(!element.empty()){auto value=projected(base,"[]");value.type=element;out["[]"]=value;for(auto [path,view]:field_shape(value,depth+1))out["[]"+path]=std::move(view);}
    }
    return out;
}
void SemanticAnalyzer::seed(const std::string& name,const Node* owner,bool readonly){
    auto& symbol=lookup(name,owner->span);symbol.declaration=owner;symbol.origins={{owner,"",readonly,false}};
    Info value{symbol.type,symbol.writable,symbol.readonly_children,symbol.origins};symbol.views=fields(value);
}
void SemanticAnalyzer::capture_return(const Node& n){
    HUA_SEMA_AREA(returns);
    ReturnFacts facts;auto result=result_type(*current_function_);auto types=type_arguments(result,"multi");
    for(std::size_t i=0;i<n.children.size();++i){auto value=infos_.at(n.children[i].get());
        if(n.children.size()==1&&!types.empty()&&node_positions_.contains(n.children[i].get())){
            const auto& positions=node_positions_.at(n.children[i].get());auto views=node_position_views_.find(n.children[i].get());
            for(std::size_t p=0;p<positions.size();++p){facts.positions.push_back(copied(positions[p]));facts.position_views.push_back(views!=node_position_views_.end()&&p<views->second.size()?views->second[p]:fields(positions[p]));}break;
        }
        if(value.type.empty())value.type=types.empty()?result:types[i];
        auto views=node_views_.contains(n.children[i].get())?node_views_.at(n.children[i].get()):fields(value);
        auto original=value;value=copied(value);
        if(record(value.type))for(auto& [path,view]:views){if(!original.writable){view.writable=false;view.readonly_children=true;}if(record(view.type))view=copied(view);}
        facts.positions.push_back(value);facts.position_views.push_back(views);
    }
    if(facts.positions.size()==1){facts.value=facts.positions[0];facts.views=facts.position_views[0];}
    else facts.value={result,false};
    if(!saw_return_){pending_return_=std::move(facts);saw_return_=true;return;}
    pending_return_.views=meet(std::move(pending_return_.views),facts.views,pending_return_.value,facts.value);pending_return_.value=meet(pending_return_.value,facts.value);
    for(std::size_t i=0;i<facts.positions.size()&&i<pending_return_.positions.size();++i){pending_return_.position_views[i]=meet(std::move(pending_return_.position_views[i]),facts.position_views[i],pending_return_.positions[i],facts.positions[i]);pending_return_.positions[i]=meet(pending_return_.positions[i],facts.positions[i]);}
}
SemanticAnalyzer::Info SemanticAnalyzer::instantiate(Info value,const Node& site) const{
    std::set<Origin> origins;
    for(const auto& source:value.origins){auto found=actual_origins_.find(source.owner);
        if(found==actual_origins_.end()){auto origin=source;auto type=infos_.at(site.children[0].get()).type;
            if(type.starts_with("fn:")&&source.owner&&(source.owner->kind==N::Array||source.owner->kind==N::MapLiteral||source.owner->kind==N::StructLiteral||source.owner->kind==N::Call)&&source.owner!=&site){origin.owner=&site;if(!source.path.starts_with('@'))origin.path="@"+std::to_string(reinterpret_cast<std::uintptr_t>(source.owner))+source.path;}
            origins.insert(std::move(origin));continue;}
        auto actual=found->second;auto views=actual_views_.find(source.owner);
        if(!source.path.empty()){
            if(views!=actual_views_.end()&&views->second.contains(source.path))actual=views->second.at(source.path);
            else {actual=projected(actual,source.path);if(!found->second.deep_owned){actual.writable=false;actual.readonly_children=true;}}
            if(!found->second.writable){actual.writable=false;actual.readonly_children=true;}
        }
        if(!value.record_copy){value.writable=value.writable&&actual.writable&&!source.readonly;value.readonly_children=value.readonly_children||actual.readonly_children||source.readonly;}
        for(auto origin:actual.origins){origin.readonly=origin.readonly||source.readonly;origin.copied=origin.copied||source.copied;origins.insert(std::move(origin));}
    }
    value.origins=std::move(origins);return value;
}
SemanticAnalyzer::Views SemanticAnalyzer::instantiate(Views views,const Node& site) const{for(auto& [path,value]:views)value=instantiate(std::move(value),site);return views;}
void SemanticAnalyzer::note_write(const Node& n){
    if(!probing_)return;
    if(n.kind==N::Index||n.kind==N::Member){const auto& base=infos_.at(n.children[0].get());if(n.kind==N::Index||!base.record_copy)for(const auto& origin:base.origins)pending_effects_.required.insert(origin);}
}
void SemanticAnalyzer::update_aliases(const Info& container,const std::string& path,Info value,const Views& descendants){
    HUA_SEMA_AREA(alias);
    HUA_SEMA_COUNT(alias_scans);
    for(auto& scope:scopes_)for(auto& [name,symbol]:scope){
        HUA_SEMA_COUNT(alias_symbols);HUA_SEMA_ADD(alias_views,symbol.views.size());
        auto shares=[&](const std::set<Origin>& origins){for(const auto& a:origins)for(const auto& b:container.origins)if(a.owner==b.owner&&a.path==b.path)return true;return false;};
        std::vector<std::string> prefixes;if(shares(symbol.origins))prefixes.push_back("");
        for(const auto& [prefix,view]:symbol.views)if(shares(view.origins)&&view.type==container.type)prefixes.push_back(prefix);
        for(const auto& prefix:prefixes){auto key=prefix+path;auto updated=copied(value);if(auto old=symbol.views.find(key);old!=symbol.views.end())updated=meet(old->second,updated);
            symbol.views[key]=updated;for(auto [field,view]:descendants){if(!value.writable){view.writable=false;view.readonly_children=true;if(record(view.type))view=copied(view);}auto name=key+field;if(auto old=symbol.views.find(name);old!=symbol.views.end())view=meet(old->second,view);symbol.views[name]=std::move(view);}
            if(probing_&&effect_roots_.contains(symbol.declaration))pending_effects_.writes[symbol.declaration][key]=updated;
        }
        Info root{symbol.type,symbol.writable,symbol.readonly_children,symbol.origins,symbol.deep_owned,symbol.record_copy};bound(symbol.views,root);symbol.readonly_children=root.readonly_children;
    }
}
void SemanticAnalyzer::apply_effects(const Effects& effects,const Node& site){
    HUA_SEMA_AREA(apply);
    if(effects.unknown){if(probing_)pending_effects_.unknown=true;forget_unknown(site);}
    for(const auto& required:effects.required){auto found=actual_origins_.find(required.owner);if(found==actual_origins_.end())continue;
        Info fact=found->second;if(!required.path.empty()){auto views=actual_views_.find(required.owner);if(views!=actual_views_.end()&&views->second.contains(required.path))fact=views->second.at(required.path);else fact=projected(fact,required.path);}
        if((required.readonly||!fact.writable)&&!probing_)error(site,"callee writes through a read-only member/view","E3003");
        if(probing_&&!fact.record_copy)for(const auto& origin:fact.origins)pending_effects_.required.insert(origin);
    }
    const Node* function=nullptr;auto type=infos_.at(site.children[0].get()).type;if(type.starts_with("fn:"))function=model_.functions.at(type.substr(3));if(!function)return;auto params=parameters(*function);
    for(const auto& [owner,writes]:effects.writes){std::size_t index=0;while(index<params.size()&&params[index]!=owner)++index;
        const Node* argument=nullptr;if(index<params.size()&&mutable_parameter(*params[index]))argument=site.children[index+1].get();
        else if(owner==function&&site.children[0]->kind==N::Member)argument=site.children[0]->children[0].get();
        Symbol* root=nullptr;std::string prefix;
        if(argument){auto location=projection(*argument);root=location.first;prefix=location.second;}
        else for(std::size_t layer=0;layer<std::min<std::size_t>(2,scopes_.size());++layer)for(auto& [name,symbol]:scopes_[layer])if(symbol.declaration==owner)root=&symbol;
        if(!root&&!argument)continue;
        std::vector<std::pair<std::string,Info>> ordered(writes.begin(),writes.end());std::sort(ordered.begin(),ordered.end(),[](const auto& a,const auto& b){return a.first.size()!=b.first.size()?a.first.size()<b.first.size():a.first<b.first;});
        for(auto [path,value]:ordered){if(defer_site_==&site&&index<params.size()&&record(actual_origins_.at(params[index]).type)&&path.find("[]")==std::string::npos)continue;auto key=prefix+path;value=instantiate(std::move(value),site);
            if(root){for(auto it=root->views.begin();it!=root->views.end();)if(key.empty()||it->first==key||it->first.starts_with(key+".")||it->first.starts_with(key+"[]"))it=root->views.erase(it);else ++it;
            if(key.empty()){root->readonly_children=value.readonly_children;root->origins=value.origins;root->deep_owned=value.deep_owned;root->record_copy=value.record_copy;if(root->type.starts_with('[')||root->type.starts_with("map<")||root->type.starts_with("List<")||root->type.starts_with("Result<"))root->writable=value.writable;}
            else root->views[key]=value;
            if(probing_&&effect_roots_.contains(root->declaration))pending_effects_.writes[root->declaration][key]=value;}
            if(auto index=path.find("[]");index!=std::string::npos&&argument){auto container=infos_.at(argument);auto ancestor=path.substr(0,index);
                if(root){if(auto view=root->views.find(prefix+ancestor);view!=root->views.end())container=view->second;}
                else if(auto views=node_views_.find(argument);views!=node_views_.end()){if(auto view=views->second.find(ancestor);view!=views->second.end())container=view->second;}
                update_aliases(container,path.substr(index),value,{});
            }
        }
    }
}
void SemanticAnalyzer::forget_unknown(const Node& site){
    HUA_SEMA_AREA(unknown);
    auto shared=[](const std::string& type){return type.starts_with('[')||type.starts_with("map<")||type.starts_with("List<")||type.starts_with("Result<")||type=="Buffer";};
    auto reset=[&](Symbol& symbol,bool global){
        if(!shared(symbol.type)&&!record(symbol.type))return;
        if(global&&shared(symbol.type))symbol.writable=false;
        symbol.readonly_children=true;symbol.deep_owned=false;
        symbol.views=fields(Info{symbol.type,symbol.writable,true,symbol.origins,false,symbol.record_copy});
    };
    for(std::size_t layer=0;layer<std::min<std::size_t>(2,scopes_.size());++layer)for(auto& [name,symbol]:scopes_[layer])reset(symbol,true);
    for(std::size_t i=1;i<site.children.size();++i){auto [symbol,path]=projection(*site.children[i]);if(!symbol)continue;
        if(path.empty())reset(*symbol,false);else if(auto view=symbol->views.find(path);view!=symbol->views.end()){view->second.readonly_children=true;view->second.deep_owned=false;}
    }
}
void SemanticAnalyzer::select_effect_roots(){
    effect_roots_.clear();
    for(std::size_t layer=0;layer<std::min<std::size_t>(2,scopes_.size());++layer)for(const auto& [name,symbol]:scopes_[layer])if(symbol.declaration)effect_roots_.insert(symbol.declaration);
    if(current_function_){effect_roots_.insert(current_function_);for(auto parameter:parameters(*current_function_))if(mutable_parameter(*parameter))effect_roots_.insert(parameter);}
}
void SemanticAnalyzer::capture_effect_exit(){
    HUA_SEMA_AREA(exit);
    HUA_SEMA_COUNT(exits);
    std::unordered_map<const Node*,Views> snapshot;
    for(const auto& scope:scopes_)for(const auto& [name,symbol]:scope)if(effect_roots_.contains(symbol.declaration)){auto& facts=snapshot[symbol.declaration];facts=symbol.views;facts[""]={symbol.type,symbol.writable,symbol.readonly_children,symbol.origins,symbol.deep_owned,symbol.record_copy};}
    exit_views_.push_back(std::move(snapshot));
}
void SemanticAnalyzer::finish_effects(){
    for(auto& [owner,writes]:pending_effects_.writes){
        for(auto& [path,value]:writes){bool first=true;
            for(const auto& exit:exit_views_){auto root=exit.find(owner);if(root==exit.end())continue;auto found=root->second.find(path);auto next=found!=root->second.end()?found->second:projected(root->second.at(""),path);next.type=value.type;
                value=first?next:meet(value,next);first=false;
            }
        }
        // Effects use the same finite path domain as local views. A write below
        // that domain invalidates ownership of its nearest retained ancestor.
        std::vector<std::pair<std::string,Info>> ancestors;
        for(auto it=writes.begin();it!=writes.end();){unsigned depth=0;std::size_t cut=std::string::npos;
            for(std::size_t i=0;i<it->first.size();++i)if(it->first[i]=='.'||it->first[i]=='[')if(++depth==9){cut=i;break;}
            if(cut==std::string::npos){++it;continue;}
            auto prefix=it->first.substr(0,cut);Info ancestor;
            for(const auto& exit:exit_views_)if(auto root=exit.find(owner);root!=exit.end()){if(auto view=root->second.find(prefix);view!=root->second.end())ancestor=view->second;else ancestor=projected(root->second.at(""),prefix);break;}
            ancestor.readonly_children=true;ancestor.deep_owned=false;ancestors.emplace_back(std::move(prefix),std::move(ancestor));it=writes.erase(it);
        }
        for(auto& [path,value]:ancestors){if(auto previous=writes.find(path);previous!=writes.end()){previous->second.readonly_children=true;previous->second.deep_owned=false;}else writes.emplace(std::move(path),std::move(value));}
    }
}
void SemanticAnalyzer::infer_field_capabilities(const Node&){
    HUA_SEMA_PHASE(summaries);
    auto globals=HUA_SEMA_SNAPSHOT(scopes_,global_snapshot);auto functions=model_.functions;
    for(auto& scope:globals)for(auto& [name,symbol]:scope)if(symbol.declaration){symbol.origins={{symbol.declaration,"",!symbol.writable,false}};for(auto& [path,view]:symbol.views)view.origins={{symbol.declaration,path,!symbol.writable,false}};}
    probing_=true;bool changed=true;unsigned pass=0;
    while(changed&&pass++<32){auto previous_returns=HUA_SEMA_SNAPSHOT(return_facts_,summary_snapshot);auto previous_effects=HUA_SEMA_SNAPSHOT(effects_,summary_snapshot);
        for(const auto& [name,fn]:functions){if(model_.nested.contains(fn)||fn->text.find(" external")!=std::string::npos||fn->text.find(" abstract")!=std::string::npos)continue;
            HUA_SEMA_FUNCTION(name,pass,true);
            HUA_SEMA_RESTORE(scopes_,globals,function_reset);scopes_.emplace_back();current_function_=fn;pending_return_={};pending_effects_={};effect_roots_.clear();exit_views_.clear();saw_return_=false;
            if(auto dot=name.find('.');dot!=std::string::npos){define("self",{name.substr(0,dot),false,model_.mutating[name],{}},fn->span);seed("self",fn,!model_.mutating[name]);}
            for(const auto* p:parameters(*fn)){define(declared_name(*p),{p->children.empty()?"":type_name(*p->children[0]),false,mutable_parameter(*p),{}},p->span);seed(declared_name(*p),p,!mutable_parameter(*p));}
            select_effect_roots();statements(*fn->children.back(),false);capture_effect_exit();finish_effects();
#ifdef HUA_SEMA_PROFILE
            if(sema_profile::active)sema_profile::summary_change(name,!return_facts_.contains(name)||return_facts_.at(name)!=pending_return_,!effects_.contains(name)||effects_.at(name)!=pending_effects_);
#endif
            {HUA_SEMA_AREA(summary_store);return_facts_[name]=pending_return_;effects_[name]=pending_effects_;}
        }
        changed=previous_returns!=return_facts_||previous_effects!=effects_;
#ifdef HUA_SEMA_PROFILE
        sema_profile::finish_pass(pass,changed);
#endif
    }
    if(changed)HUA_SEMA_COUNT(summary_fallbacks);
    if(changed)for(auto& [name,facts]:return_facts_){facts.value.writable=false;facts.value.readonly_children=true;facts.views=fields(facts.value);for(auto& value:facts.positions){value.writable=false;value.readonly_children=true;}effects_[name].unknown=true;effects_[name].writes.clear();}
    probing_=false;scopes_=std::move(globals);current_function_=nullptr;pending_effects_={};pending_return_={};saw_return_=false;
}

}
