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
    std::ostringstream body,protos,closure_code,closure_layouts;
    std::vector<SourceSpan> spans;std::vector<std::string> literals;
    struct Local { std::string cpp,type; bool stack_functions{}, immutable_callable{}; };
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
    struct Signature { std::string name,result;std::vector<std::string> names,types;std::vector<bool> mut;bool any_mut{};std::string self;bool self_mut{}; };
    std::unordered_map<const Node*,std::string> inferred_returns;
    std::map<const Node*,std::string> inferred_expressions;
    static bool function_value_type(const std::string& type){
        return type=="@function"||type.starts_with("fn:")||type.starts_with("callable:");
    }
    const std::string& ty(const Node& n){
        if(auto inferred=inferred_expressions.find(&n);inferred!=inferred_expressions.end())
            return inferred->second;
        const auto found=m.types.find(&n);
        if(n.kind==N::Array&&found!=m.types.end()&&found->second=="[]"&&!n.children.empty()){
            const auto first=ty(*n.children[0]);
            if(first.starts_with("fn:")&&std::all_of(n.children.begin(),n.children.end(),
                    [&](const auto& child){return ty(*child)==first;}))
                return inferred_expressions.emplace(&n,"[]"+first).first->second;
            if(function_value_type(first)&&std::all_of(n.children.begin(),n.children.end(),
                    [&](const auto& child){return function_value_type(ty(*child));}))
                return inferred_expressions.emplace(&n,"[]@function").first->second;
        }
        if(n.kind==N::Pack&&found!=m.types.end()){
            const auto parts=type_arguments(found->second,"multi");
            if(std::any_of(parts.begin(),parts.end(),[](const auto& t){return t.empty();})){
                std::string packed="multi<";
                for(std::size_t i=0;i<n.children.size();++i){
                    if(i)packed+=",";
                    packed+=ty(*n.children[i]);
                }
                return inferred_expressions.emplace(&n,packed+">").first->second;
            }
        }
        if(n.kind==N::Call&&n.children.size()==2&&(found==m.types.end()||found->second.empty()||found->second=="[]")){
            const auto original=m.types.find(n.children[0].get());
            if(original!=m.types.end()&&original->second=="builtin:clone")return ty(*n.children[1]);
        }
        if(found!=m.types.end()&&!found->second.empty()){
            if(found->second!="[]"||(n.kind!=N::Name&&n.kind!=N::Let&&n.kind!=N::Var))return found->second;
            if(n.kind==N::Name){
                if(const auto* local=find(n.text,n);local&&(local->type.starts_with("[]fn:")||local->type=="[]@function"))return local->type;
                return found->second;
            }
        }
        if(n.kind==N::Let||n.kind==N::Var)return ty(*n.children.back());
        if(n.kind==N::Name)if(const auto local=find(n.text,n))return local->type;
        if(n.kind==N::Index){
            const auto& base=ty(*n.children[0]);
            if(base.starts_with("[]fn:")||base=="[]@function")
                return inferred_expressions.emplace(&n,base.substr(2)).first->second;
        }
        if(n.kind==N::Call){
            const auto& callee=ty(*n.children[0]);
            if(callee.starts_with("callable:"))return indirect_signatures.at(callee).result;
            if(callee=="@function"||callee=="@dynamic")return dynamic_value_type;
            if(callee=="builtin:clone"&&n.children.size()==2)return ty(*n.children[1]);
            if(callee.starts_with("fn:")){
                const auto fn=m.functions.find(callee.substr(3));
                if(fn!=m.functions.end())if(const auto result=inferred_returns.find(fn->second);result!=inferred_returns.end())
                    return result->second;
            }
        }
        if(n.kind==N::Binary||n.kind==N::Unary)return dynamic_value_type;
        if(found!=m.types.end())return found->second;
        no(n,"missing static type for "+std::string(node_name(n.kind))+" "+n.text);
    }
    std::map<std::string,Signature> indirect_signatures;
    std::map<std::string,std::set<std::string>> indirect_candidates;
    std::set<std::string> indirect_identities;
    std::unordered_map<const Node*,bool> self_borrow_cache;
    const std::string dynamic_value_type="@dynamic";
    bool indirect_scalar(const std::string& type){
        return type=="bool"||type=="string"||type=="nil"||hua::numeric_spec(type);
    }
    bool dynamic_signature(const Signature& signature){
        const auto usable=[](const std::string& type){return !type.starts_with("Task<");};
        const bool receiver=signature.self.empty()||is_interface(signature.self)||located_records.contains(signature.self);
        return receiver&&usable(signature.result)&&std::all_of(signature.types.begin(),signature.types.end(),usable);
    }
    static bool returns_a_value(const Node& node){
        if(node.kind==N::Function)return false;
        if(node.kind==N::Return)return !node.children.empty();
        return std::any_of(node.children.begin(),node.children.end(),[](const auto& n){return returns_a_value(*n);});
    }
    static bool has_return_type(const Node& function){
        return std::any_of(function.children.begin(),function.children.end(),[](const auto& child){return child->kind==N::ReturnTypes;});
    }
    static bool same_call_shape(const Signature& first,const Signature& second){
        return first.types==second.types&&first.mut==second.mut&&first.result==second.result;
    }
    static bool always_returns(const Node& n){
        if(n.kind==N::Return)return true;
        if(n.kind==N::Block)return std::any_of(n.children.begin(),n.children.end(),[](const auto& c){return always_returns(*c);});
        return n.kind==N::If&&n.children.size()==3&&always_returns(*n.children[1])&&always_returns(*n.children[2]);
    }
    void collect_inferred_returns(){
        for(const auto& [name,fn]:m.functions){
            if(has_return_type(*fn))continue;
            std::set<std::string> candidates;
            bool consistent=true;
            const auto visit=[&](auto&& self,const Node& node)->void{
                if(node.kind==N::Function)return;
                if(node.kind==N::Return){
                    if(node.children.size()!=1){consistent=false;return;}
                    const auto it=m.types.find(node.children[0].get());
                    if(it==m.types.end()||!it->second.starts_with("fn:")){consistent=false;return;}
                    candidates.insert(it->second.substr(3));
                }
                for(const auto& child:node.children)self(self,*child);
            };
            visit(visit,*fn->children.back());
            if(!consistent||candidates.empty()||!always_returns(*fn->children.back())){
                if(returns_a_value(*fn->children.back()))inferred_returns.emplace(fn,dynamic_value_type);
                continue;
            }
            if(candidates.size()==1){
                inferred_returns.emplace(fn,"fn:"+*candidates.begin());
                continue;
            }
            try{
                const auto first=signature(*m.functions.at(*candidates.begin()));
                if(!first.self.empty())continue;
                bool boxed=dynamic_signature(first);
                for(const auto& candidate:candidates){
                    const auto next=signature(*m.functions.at(candidate));
                    if(!next.self.empty()||!same_call_shape(first,next))consistent=false;
                    if(!dynamic_signature(next))boxed=false;
                }
                if(consistent&&!first.any_mut){
                    const auto type="callable:"+*candidates.begin();
                    indirect_signatures.try_emplace(type,first);
                    indirect_candidates[type].insert(candidates.begin(),candidates.end());
                    inferred_returns.emplace(fn,type);
                }else if(boxed)inferred_returns.emplace(fn,"@function");
            }catch(const NativeUnsupported&){ /* The normal emitter reports unsupported signatures on use. */ }
        }
    }
    bool is_struct(const std::string& t){auto f=m.structures.find(t);return f!=m.structures.end()&&f->second->text.find(" enum")==std::string::npos&&f->second->text.find(" interface")==std::string::npos&&f->second->kind==N::Struct;}
    bool is_enum(const std::string& t){auto f=m.structures.find(t);return f!=m.structures.end()&&f->second->text.find(" enum")!=std::string::npos&&f->second->kind==N::Struct;}
    // Payload fields of an enum, per variant in tag order: (field name, payload type).
    struct EnumInfo { std::vector<std::string> variants;std::map<std::string,std::vector<std::pair<std::string,std::string>>> fields; };
    std::map<std::string,EnumInfo> enums;
    const EnumInfo& enum_info(const std::string& t){
        if(auto f=enums.find(t);f!=enums.end())return f->second;
        EnumInfo info;const auto& decl=*m.structures.at(t);
        for(const auto& f:decl.children)if(f->kind==N::Field&&f->text!="$case"){
            const auto first=f->text.find('#'),second=f->text.find('#',first+1);
            const auto variant=f->text.substr(1,first-1);const auto type=canonical_type(f->text.substr(second+1));
            if(std::find(info.variants.begin(),info.variants.end(),variant)==info.variants.end())info.variants.push_back(variant);
            info.fields[variant].push_back({f->text,type});
        }
        return enums.emplace(t,std::move(info)).first->second;
    }
    std::size_t tag_of(const std::string& t,const std::string& variant,const Node& at){
        const auto& v=enum_info(t).variants;auto f=std::find(v.begin(),v.end(),variant);if(f==v.end())no(at,"unknown variant");return static_cast<std::size_t>(f-v.begin());
    }
    static const char* opaque_tag(const std::string& t){if(t=="Bytes")return "hn::BytesTag";if(t=="Buffer")return "hn::BufferTag";if(t=="Json")return "hn::JsonTag";
        if(t=="std.file.File")return "hn::FileTag";
        if(t=="std.file.IOFailure")return "hn::IOFailureTag";
        if(t=="std.error.Value")return "hn::ErrorTag";
        return nullptr;}
    // map types are spelled `map[K]V` in source and `map<K,V>` by the checker.
    static bool is_map(const std::string& t){return t.rfind("map[",0)==0||t.rfind("map<",0)==0;}
    std::vector<std::string> map_parts(const std::string& t){
        if(t.starts_with("map<"))return type_arguments(t,"map");
        int depth=0;for(std::size_t i=3;i<t.size();++i){if(t[i]=='[')++depth;else if(t[i]==']'&&--depth==0)return {t.substr(4,i-4),t.substr(i+1)};}
        return {};
    }
    bool is_interface(const std::string& t){
        auto it=m.structures.find(t);
        return it!=m.structures.end()&&it->second->text.find(" interface")!=std::string::npos;
    }
    bool is_fixed(const std::string& t){return t.starts_with('[')&&!t.starts_with("[]");}
    bool is_sequence(const std::string& t){return t.starts_with('[');}
    std::string element_type(const std::string& t){return t.substr(t.find(']')+1);}
    std::string exact_type(const std::string& t){
        const auto spec=hua::numeric_spec(t);
        if(!spec||t=="int"||t=="float")return {};
        auto name=t;
        if(t=="@integer")name="Integer";
        else name[0]=static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
        const auto rep=spec->category=='f'?(spec->bits==32?"float":"double"):
            "std::"+std::string(spec->category=='u'?"uint":"int")+std::to_string(spec->bits)+"_t";
        return "hn::Exact<hn::ExactName::"+name+","+rep+">";
    }
    std::map<std::string,std::size_t> interface_tags;
    std::string interface_layouts(){
        std::string code;
        for(const auto& [name,types]:m.relations){
            const auto tag=mangle("IF_",name);
            code+="struct "+tag+";\n";
            code+="template<> struct TypeName<Interface<"+tag+">>{static std::string get(){return "+quoted(name)+";}};\n";
        }
        return code;
    }
    // The C++ type of a Hua type.
    std::string ct(const std::string& t,const Node& at){
        if(t=="@dynamic")return "hn::DynamicValue";
        if(t=="@function")return "hn::DynamicFunction";
        if(t.starts_with("callable:")){
            const auto& sig=indirect_signatures.at(t);
            std::string type="hn::IndirectFunction<"+(sig.result.empty()?"void":ct(sig.result,at))+"(";
            for(std::size_t i=0;i<sig.types.size();++i){
                if(i)type+=",";
                type+=parameter_type(sig,i,at);
            }
            return type+")>";
        }
        if(t.starts_with("fn:")){
            const auto name=t.substr(3);
            const auto fn=m.functions.find(name);
            if(fn==m.functions.end())no(at,"unknown function type");
            const auto sig=signature(*fn->second);
            if(is_struct(sig.self)&&located_records.contains(sig.self))
                return "hn::BoundMethod<hn::"+mangle("SB_",sig.self)+">";
            if(is_interface(sig.self))return "hn::BoundInterface<"+ct(sig.self,at)+">";
            return "hn::FunctionValue<hn::"+mangle("FT_",name)+","+(m.nested.contains(fn->second)?"true":"false")+">";
        }
        if(t=="std.random.Rng")return "hn::Rng";
        if(t=="int")return "hn::I";if(t=="float")return "hn::F";if(t=="bool")return "bool";if(t=="string")return "hn::Str";if(t=="void")return "hn::Unit";
        if(auto exact=exact_type(t);!exact.empty())return exact;
        if(is_interface(t))return "hn::Interface<hn::"+mangle("IF_",t)+">";
        if(is_fixed(t)){
            const auto end=t.find(']');
            if(end==std::string::npos)no(at,"malformed fixed array type");
            return "hn::Fixed<"+ct(element_type(t),at)+","+t.substr(1,end-1)+">";
        }
        if(t.starts_with("[]"))return "hn::Slice<"+ct(t.substr(2),at)+">";
        if(t.starts_with("multi<")){
            std::string result="std::tuple<";
            const auto parts=type_arguments(t,"multi");
            for(std::size_t i=0;i<parts.size();++i){
                if(i)result+=",";
                result+=ct(parts[i],at);
            }
            return result+">";
        }
        if(is_map(t)){const auto a=map_parts(t);if(a.size()==2)return "hn::Map<"+ct(a[0],at)+","+ct(a[1],at)+">";}
        if(t.starts_with("List<")){const auto& a=type_arguments(t,"List");if(a.size()==1)return "hn::List<"+ct(a[0],at)+">";}
        if(t.starts_with("Deque<")){
            const auto a=type_arguments(t,"Deque");
            if(a.size()==1)return "hn::Deque<"+ct(a[0],at)+">";
        }
        if(t.starts_with("Result<")){const auto& a=type_arguments(t,"Result");
            if(a.size()==1)return "hn::Result<"+ct(a[0],at)+",hn::Str>";
            if(a.size()==2&&!a[0].empty()&&!a[1].empty())return "hn::Result<"+ct(a[0],at)+","+ct(a[1],at)+">";}
        if(t.size()>1&&t.back()=='?')return "hn::Opt<"+ct(t.substr(0,t.size()-1),at)+">";
        if(is_struct(t)&&(!structs_ready||usable_structs.contains(t)))return mangle("S_",t);
        if(is_enum(t)&&(!structs_ready||usable_structs.contains(t)))return mangle("E_",t);
        if(auto tag=opaque_tag(t))return std::string("hn::Opaque<")+tag+">";
        if(t.empty())no(at,"empty type at "+std::string(node_name(at.kind))+" "+at.text+" "+at.span.file_id.substr(at.span.file_id.find_last_of("\\\\/")+1)+":"+std::to_string(at.span.line));
        no(at,"type `"+t+"`");
    }
    // Arguments the bridge can box (a copy, except opaque handles which are shared).
    bool boxable(const std::string& t){
        if(t=="@dynamic"||value_like(t)||opaque_tag(t))return true;
        if(is_sequence(t))return boxable(element_type(t));
        if(is_interface(t))return true;
        if(t.starts_with("List<")){const auto& a=type_arguments(t,"List");return a.size()==1&&boxable(a[0]);}
        if(is_map(t)){const auto a=map_parts(t);return a.size()==2&&boxable(a[1]);}
        if(t.starts_with("Result<")){const auto& a=type_arguments(t,"Result");if(a.size()==1)return boxable(a[0]);return a.size()==2&&boxable(a[0])&&boxable(a[1]);}
        if(t.size()>1&&t.back()=='?')return boxable(t.substr(0,t.size()-1));
        return (is_struct(t)||is_enum(t))&&usable_structs.contains(t);
    }
    // Types the boxed bridge can carry in both directions (no handles with identity).
    bool value_like(const std::string& t){
        if(opaque_tag(t)||t=="std.random.Rng")return true;
        if(t=="int"||t=="float"||t=="bool"||t=="string"||t=="void")return true;
        if(t.starts_with("multi<")){
            const auto parts=type_arguments(t,"multi");
            return !parts.empty()&&std::all_of(parts.begin(),parts.end(),[&](const auto& part){return value_like(part);});
        }
        if(hua::numeric_spec(t))return true;
        if(is_sequence(t))return value_like(element_type(t));
        if(t.starts_with("Result<")){const auto& a=type_arguments(t,"Result");if(a.size()==1)return value_like(a[0]);return a.size()==2&&value_like(a[0])&&value_like(a[1]);}
        if(t.size()>1&&t.back()=='?')return value_like(t.substr(0,t.size()-1));
        return false;
    }
    std::unordered_map<std::string,Local> globals; // module-level let/var
    using Scopes=decltype(scopes);
    struct Capture { Local source,access;std::string field; };
    struct ClosureFrame {
        ClosureFrame* parent;
        const Scopes& outer;
        std::map<std::string,Capture> fields;
    };
    ClosureFrame* closure_frame{};
    std::set<std::string> escaping_functions;

    static bool has_typed_callback(const std::string& name){
        static const std::set<std::string> supported{
            "map","fold","filter","any","all","find","find_index","partition","zip_with",
            "sorted_by","sorted_by_key","min_by_key","max_by_key"
        };
        constexpr std::string_view prefix="$std$alg$";
        return (name.starts_with(prefix)&&supported.contains(name.substr(prefix.size())))
            ||name=="$std$list$retain"||name=="$std$list$sort_by"||name=="$std$list$sort_by_key"
            ||name=="$std$maps$update"||name=="$std$heap$push_by_key"||name=="$std$heap$pop_by_key";
    }
    void classify_closure_uses(const Node& n,const Node* parent,const Node* owner){
        if(n.kind==N::Function)owner=&n;
        if(n.kind==N::Name&&parent){
            const auto type=m.types.find(&n);
            if(type!=m.types.end()&&type->second.starts_with("fn:")){
                const auto name=type->second.substr(3);
                const auto declaration=m.functions.find(name);
                if(declaration!=m.functions.end()&&m.nested.contains(declaration->second)){
                    const bool direct=parent->kind==N::Call&&parent->children[0].get()==&n;
                    const auto callee=parent->kind==N::Call?m.types.find(parent->children[0].get()):m.types.end();
                    const bool immediate=callee!=m.types.end()&&callee->second.starts_with("builtin:")&&has_typed_callback(callee->second.substr(8));
                    const auto declaring_owner=closure_owners.find(declaration->second);
                    const bool same_frame=declaration->second==owner||(declaring_owner!=closure_owners.end()&&declaring_owner->second==owner);
                    if(!same_frame||(!direct&&!immediate))escaping_functions.insert(name);
                }
            }
        }
        for(const auto& child:n.children)classify_closure_uses(*child,&n,owner);
    }
    std::unordered_map<const Node*,const Node*> closure_owners;
    void collect_closure_owners(const Node& n,const Node* owner){
        if(n.kind==N::Function){
            if(m.nested.contains(&n))closure_owners.emplace(&n,owner);
            owner=&n;
        }
        for(const auto& child:n.children)collect_closure_owners(*child,owner);
    }
    // Borrowing the caller's environment is safe only if this body cannot
    // publish its own function value. Direct recursion keeps the caller alive.
    bool borrows_self(const Node& fn){
        if(const auto cached=self_borrow_cache.find(&fn);cached!=self_borrow_cache.end())return cached->second;
        const auto name=declared_name(fn);
        const auto identity="fn:"+name;
        const auto safe=[&](auto&& visit,const Node& node,const Node* parent)->bool{
            if(node.kind==N::Function)return false;
            if(node.kind==N::Name){
                const auto actual=m.types.find(&node);
                if(node.text==name||(actual!=m.types.end()&&actual->second==identity))
                    return parent&&parent->kind==N::Call&&parent->children[0].get()==&node;
            }
            return std::all_of(node.children.begin(),node.children.end(),
                [&](const auto& child){return visit(visit,*child,&node);});
        };
        return self_borrow_cache.emplace(&fn,safe(safe,*fn.children.back(),nullptr)).first->second;
    }

    struct StackClosurePlan {
        bool array{};
        std::vector<std::pair<const Node*,const Node*>> calls; // call, factory declaration
    };
    std::unordered_map<const Node*,const Node*> eligible_factories;
    std::unordered_map<const Node*,const Node*> enabled_factories;
    std::unordered_set<const Node*> factory_closures;
    std::unordered_map<const Node*,StackClosurePlan> stack_closure_plans;
    std::unordered_map<const Node*,std::string> stack_call_slots;

    void collect_stack_factories(){
        for(const auto& [name,fn]:m.functions){
            if(m.nested.contains(fn)||fn->children.empty())continue;
            const auto inferred=inferred_returns.find(fn);
            if(inferred==inferred_returns.end()||!inferred->second.starts_with("fn:"))continue;
            const auto& body=*fn->children.back();
            if(body.kind!=N::Block||body.children.size()!=2)continue;
            const auto& closure=*body.children[0];
            if(closure.kind!=N::Function||inferred->second!="fn:"+declared_name(closure))continue;
            if(body.children[1]->kind!=N::Return||body.children[1]->children.size()!=1||
               body.children[1]->children[0]->kind!=N::Name)continue;
            try{
                const auto sig=signature(*fn), nested=signature(closure);
                if(!sig.self.empty()||sig.any_mut||!has_return_type(closure)||!indirect_scalar(nested.result))continue;
                if(!std::all_of(sig.types.begin(),sig.types.end(),
                    [&](const auto& t){return indirect_scalar(t);}))continue;
                if(borrows_self(closure))eligible_factories.emplace(fn,&closure);
            }catch(const NativeUnsupported&){ /* The usual lowering reports unsupported declarations. */ }
        }
    }
    const Node* stack_factory(const Node& call){
        if(call.kind!=N::Call||call.children[0]->kind!=N::Name)return nullptr;
        const auto actual=m.types.find(call.children[0].get());
        if(actual==m.types.end()||!actual->second.starts_with("fn:"))return nullptr;
        const auto fn=m.functions.find(actual->second.substr(3));
        return fn!=m.functions.end()&&eligible_factories.contains(fn->second)?fn->second:nullptr;
    }
    void plan_stack_closures(const Node& function){
        struct Use {const Node* node;const Node* parent;const Node* grandparent;const Node* block;};
        std::unordered_map<std::string,std::vector<Use>> uses;
        std::vector<const Node*> blocks;
        const auto collect=[&](auto&& visit,const Node& n,const Node* parent,const Node* grandparent,
                               const Node* block,bool nested)->void{
            if(n.kind==N::Function)nested=true;
            if(n.kind==N::Block){
                block=&n;
                if(!nested)blocks.push_back(block);
            }
            if(n.kind==N::Name)uses[n.text].push_back({&n,parent,grandparent,block});
            for(const auto& c:n.children)visit(visit,*c,&n,parent,block,nested);
        };
        collect(collect,*function.children.back(),nullptr,nullptr,nullptr,false);
        for(const auto* block:blocks){
            const auto block_binding=[&](const Node* n,const Node* value){
                return n&&n->kind==N::Let&&n->children.back().get()==value&&
                    std::any_of(block->children.begin(),block->children.end(),[&](const auto& child){return child.get()==n;});
            };
            const auto direct_call=[](const Node* parent,const Node* value){
                return parent&&parent->kind==N::Call&&parent->children[0].get()==value;
            };
            for(const auto& binding:block->children){
                if(binding->kind!=N::Let||binding->children.empty())continue;
                const auto& init=*binding->children.back();
                StackClosurePlan plan;
                plan.array=init.kind==N::Array;
                if(plan.array){
                    if(init.children.empty()||init.children.size()>8)continue;
                    for(const auto& child:init.children){
                        const auto* factory=stack_factory(*child);
                        if(!factory)break;
                        plan.calls.emplace_back(child.get(),factory);
                    }
                    if(plan.calls.size()!=init.children.size())continue;
                    const auto* closure=eligible_factories.at(plan.calls.front().second);
                    if(!std::all_of(plan.calls.begin(),plan.calls.end(),[&](const auto& call){
                        return eligible_factories.at(call.second)==closure;
                    }))continue;
                }else{
                    const auto* factory=stack_factory(init);
                    if(!factory)continue;
                    plan.calls.emplace_back(&init,factory);
                }
                // Only immutable aliases in this block may carry the stack value.
                // Any other use (including capture, storage, return, or a
                // shadowed spelling in a different block) keeps the heap path.
                std::vector<std::pair<std::string,bool>> aliases{{binding->text,plan.array}};
                std::set<std::string> seen;
                bool local=true;
                for(std::size_t i=0;i<aliases.size()&&local;++i){
                    const auto [name,array]=aliases[i];
                    if(!seen.insert(name).second){local=false;break;}
                    for(const auto& use:uses[name]){
                        if(use.block!=block){local=false;break;}
                        const Node* value=use.node;
                        const Node* consumer=use.parent;
                        if(array){
                            if(!consumer||consumer->kind!=N::Index||consumer->children[0].get()!=value){local=false;break;}
                            value=consumer;
                            consumer=use.grandparent;
                        }
                        if(direct_call(consumer,value))continue;
                        if(block_binding(consumer,value))aliases.emplace_back(consumer->text,false);
                        else {local=false;break;}
                    }
                }
                if(!local)continue;
                for(const auto& [call,fn]:plan.calls){
                    const auto* closure=eligible_factories.at(fn);
                    enabled_factories.emplace(fn,closure);
                    factory_closures.insert(closure);
                }
                stack_closure_plans.emplace(binding.get(),std::move(plan));
            }
        }
    }

    bool needs_tracing(const std::string& type,std::set<std::string> active={}){
        if(type=="@function"||type=="@dynamic"||type.starts_with("fn:")||type.starts_with("callable:")||is_interface(type))return true;
        if(type.ends_with('?'))return needs_tracing(type.substr(0,type.size()-1),active);
        if(is_sequence(type))return needs_tracing(element_type(type),active);
        const auto parts=is_map(type)?map_parts(type):type.starts_with("List<")?type_arguments(type,"List"):type_arguments(type,"Result");
        for(const auto& part:parts)if(needs_tracing(part,active))return true;
        if(is_struct(type)||is_enum(type)){
            if(!active.insert(type).second)return true;
            if(is_enum(type)){
                for(const auto& [variant,fields]:enum_info(type).fields)
                    for(const auto& [field,t]:fields)
                        if(needs_tracing(t,active))return true;
            }else{
                for(const auto& field:m.structures.at(type)->children)
                    if(field->kind==N::Field&&needs_tracing(type_name(*field->children[0]),active))return true;
            }
        }
        return false;
    }
    std::set<std::string> resource_functions;
    bool might_hold_file(const std::string& type,std::set<std::string> active={},bool unknown_functions=false){
        if(type=="std.file.File"||type=="@function"||type=="@dynamic"||type.starts_with("callable:"))return true;
        if(type.starts_with("fn:"))return unknown_functions||resource_functions.contains(type.substr(3));
        if(!active.insert(type).second)return false;
        if(type.ends_with('?'))return might_hold_file(type.substr(0,type.size()-1),active,unknown_functions);
        if(is_sequence(type))return might_hold_file(element_type(type),active,unknown_functions);
        const auto parts=is_map(type)?map_parts(type):type.starts_with("List<")?type_arguments(type,"List"):type_arguments(type,"Result");
        for(const auto& part:parts)if(might_hold_file(part,active,unknown_functions))return true;
        if(is_interface(type)){
            if(const auto relation=m.relations.find(type);relation!=m.relations.end())
                for(const auto& concrete:relation->second)if(might_hold_file(concrete,active,unknown_functions))return true;
        }
        if(is_enum(type)){
            for(const auto& [variant,fields]:enum_info(type).fields)
                for(const auto& [field,t]:fields)if(might_hold_file(t,active,unknown_functions))return true;
        }
        if(is_struct(type)){
            for(const auto& field:m.structures.at(type)->children)
                if(field->kind==N::Field&&might_hold_file(type_name(*field->children[0]),active,unknown_functions))return true;
        }
        return false;
    }
    bool capture_safe(const std::string& type,std::set<std::string> active={}){
        if(active.contains(type))return true;
        if(hua::numeric_spec(type)||type=="bool"||type=="string"||opaque_tag(type)||type=="@dynamic"||type=="@function"||type.starts_with("fn:")||type.starts_with("callable:"))return true;
        if(type.ends_with('?'))return capture_safe(type.substr(0,type.size()-1),active);
        if(type.starts_with("Result<")){
            const auto parts=type_arguments(type,"Result");
            return std::all_of(parts.begin(),parts.end(),[&](const auto& t){return t=="void"||capture_safe(t,active);});
        }
        if(is_sequence(type))return capture_safe(element_type(type),active);
        auto parts=is_map(type)?map_parts(type):type_arguments(type,"List");
        if(!parts.empty())return std::all_of(parts.begin(),parts.end(),[&](const auto& t){return capture_safe(t,active);});
        if(is_interface(type)&&active.insert(type).second){
            const auto found=m.relations.find(type);
            return found!=m.relations.end()&&std::all_of(found->second.begin(),found->second.end(),
                [&](const auto& concrete){return capture_safe(concrete,active);});
        }
        if(is_enum(type)&&active.insert(type).second){
            for(const auto& [variant,fields]:enum_info(type).fields)
                for(const auto& [field,t]:fields)
                    if(t!="void"&&!capture_safe(t,active))return false;
            return true;
        }
        if(is_struct(type)&&active.insert(type).second){
            const auto& fields=m.structures.at(type)->children;
            return std::all_of(fields.begin(),fields.end(),[&](const auto& f){
                return f->kind!=N::Field||capture_safe(type_name(*f->children[0]),active);
            });
        }
        // Containers require runtime capability projection and may introduce ownership cycles.
        return false;
    }
    const Local* find_local(const Scopes& in,const std::string& name) const {
        for(auto i=in.rbegin();i!=in.rend();++i)
            if(auto f=i->find(name);f!=i->end())return &f->second;
        return nullptr;
    }
    const Local* resolve_capture(ClosureFrame& frame,const std::string& name,const Node& at){
        if(auto found=frame.fields.find(name);found!=frame.fields.end())return &found->second.access;
        const auto* source=find_local(frame.outer,name);
        if(!source&&frame.parent)source=resolve_capture(*frame.parent,name,at);
        if(!source)return nullptr;
        const auto& type=source->type;
        if(!capture_safe(type))no(at,"capture of "+type);
        const auto field=mangle("captured_",name);
        Capture capture{*source,{"native_env."+field,source->type},field};
        return &frame.fields.emplace(name,std::move(capture)).first->second.access;
    }
    const Local* find(const std::string& name,const Node& at){
        if(auto local=find_local(scopes,name))return local;
        if(closure_frame)if(auto capture=resolve_capture(*closure_frame,name,at))return capture;
        if(auto g=globals.find(name);g!=globals.end())return &g->second;
        return nullptr;
    }
    void need(const Node* fn){if(queued.insert(fn).second)queue.push_back(fn);}
    // A generated enum constructor: `fn E.V(arg0..) E { return E{$case: "V", ...} }`.
    bool enum_constructor(const Node& fn,const std::string& owner){
        if(!is_enum(owner))return false;const auto& body=*fn.children.back();
        if(body.children.size()!=1||body.children[0]->kind!=N::Return||body.children[0]->children.size()!=1)return false;
        const auto& literal=*body.children[0]->children[0];
        return literal.kind==N::StructLiteral&&literal.text==owner&&!literal.children.empty()&&literal.children[0]->text=="$case";
    }
    Signature signature(const Node& fn){
        Signature s;s.name=declared_name(fn);
        if(fn.text.find(" external")!=std::string::npos||fn.text.find(" unsafe")!=std::string::npos)no(fn,"external or unsafe function");
        if(const auto dot=s.name.rfind('.');dot!=std::string::npos){
            const auto owner=s.name.substr(0,dot);
            if(!enum_constructor(fn,owner)){
                if(!is_struct(owner)&&!is_enum(owner)&&!is_interface(owner))no(fn,"method of `"+owner+"`");
                s.self=owner;auto f=m.mutating.find(s.name);s.self_mut=f!=m.mutating.end()&&f->second;
            }
        }
        for(const auto& c:fn.children){
            if(c->kind==N::Parameter){
                const bool mut=c->text.find(" mut")!=std::string::npos||(!c->children.empty()&&c->children[0]->kind==N::MutableType);
                s.names.push_back(declared_name(*c));
                s.types.push_back(c->children.empty()?dynamic_value_type:type_name(*c->children[0]));
                s.mut.push_back(mut);
                s.any_mut=s.any_mut||mut;
            }else if(c->kind==N::ReturnTypes){
                if(c->children.size()==1)s.result=type_name(*c->children[0]);
                else if(!c->children.empty()){
                    s.result="multi<";
                    for(std::size_t i=0;i<c->children.size();++i){
                        if(i)s.result+=",";
                        s.result+=type_name(*c->children[i]);
                    }
                    s.result+=">";
                }
            }else if(c->kind==N::GenericParameters)no(*c,"generic function");
        }
        if(s.result.empty()){
            if(auto inferred=inferred_returns.find(&fn);inferred!=inferred_returns.end())s.result=inferred->second;
            else if(!has_return_type(fn)&&returns_a_value(*fn.children.back()))s.result=dynamic_value_type;
        }
        if(s.result=="void")s.result.clear();
        if(s.self_mut)s.any_mut=true;
        return s;
    }
    std::string self_type(const Signature& s,const Node& at){
        const auto c=ct(s.self,at);if(s.self_mut)return c+"&";return s.any_mut?c:"const "+c+"&";
    }
    std::string parameter_type(const Signature& s,std::size_t i,const Node& at){
        const auto c=ct(s.types[i],at);
        const auto& t=s.types[i];
        if(s.mut[i]&&(record_type(t)||is_fixed(t)))return c+"&";
        if(s.mut[i])return c;
        return (record_type(t)&&!s.any_mut&&!is_interface(t))?"const "+c+"&":c;
    }
    // `e` converted to Hua type `to` when the checker's type of the expression differs
    // (contextual numeric literals, ok()/err() whose other side is still open, nil).
    std::string coerce(const std::string& e,const std::string& from,const std::string& to,const Node& at,bool evaluated=false){
        if(from==to)return e;
        if(to.starts_with("callable:")&&from.starts_with("fn:"))return indirect_value(e,from,to,at);
        if(to=="@function"&&function_value_type(from))return dynamic_function(e,from,at);
        if(to=="@dynamic")return dynamic_pack(e,from,at,false);
        if(from=="@dynamic")return dynamic_unpack(e,to,at,conversion_site(at));
        if(to.ends_with('?')&&from!="nil"&&from!="void"&&!from.ends_with('?')&&to.substr(0,to.size()-1)!=from)
            return ct(to,at)+"("+coerce(e,from,to.substr(0,to.size()-1),at,evaluated)+")";
        if(is_interface(to)){
            if(is_interface(from))return "hn::interface_convert<"+ct(to,at)+">("+e+",true)";
            if(!(is_struct(from)||is_enum(from))||!m.relations.at(to).contains(from))no(at,"interface conversion from "+from);
            return "hn::interface_value<"+ct(to,at)+">("+e+","+std::to_string(interface_tags.at(from))+",true)";
        }
        if(hua::numeric_spec(from)&&hua::numeric_spec(to)&&(from!="int"||to!="float"))
            return numeric_conversion(e,from,to,at,conversion_source(at));
        if(is_sequence(from)&&is_sequence(to)){
            const auto element=element_type(to);
            if(!evaluated&&is_fixed(to))if(auto literal=fixed_literal(conversion_source(at),to,at);!literal.empty())return literal;
            if(from=="[]"){
                const auto empty="hn::Slice<"+ct(element,at)+">(hn::EmptySlice{})";
                if(!is_fixed(to))return empty;
                return "hn::make_fixed<"+ct(element,at)+","+to.substr(1,to.find(']')-1)+">("+empty+","+conversion_site(at)+")";
            }
            if(element_type(from)==element){
                if(!is_fixed(to))return "hn::sequence_view("+e+")";
                return "hn::make_fixed<"+ct(element,at)+","+to.substr(1,to.find(']')-1)+">(hn::sequence_view("+e+"),"+conversion_site(at)+")";
            }
            return "hn::sequence_coerce<"+ct(to,at)+">("+e+","+conversion_site(at)+","+array_context(at)+")";
        }
        if(from=="[]"&&to.starts_with("[]"))return ct(to,at)+"("+e+")";
        return "static_cast<"+ct(to,at)+">("+e+")";
    }
    std::string indirect_value(const std::string& value,const std::string& from,const std::string& to,const Node& at){
        const auto* fn=m.functions.at(from.substr(3));
        const auto sig=signature(*fn);
        if(!sig.self.empty()||!same_call_shape(sig,indirect_signatures.at(to)))no(at,"incompatible function value");
        const bool nested=m.nested.contains(fn);
        if(!nested)need(fn);
        const auto runtime_type=ct(to,at);
        const auto target=fresh("function");
        const auto result=sig.result.empty()?"void":ct(sig.result,at);
        std::string params="const void* environment,const std::shared_ptr<const void>& owner",args;
        if(nested)args=ct(from,at)+(borrows_self(*fn)?"{environment,{}}":"{environment,owner}");
        for(std::size_t i=0;i<sig.types.size();++i){
            const auto type=parameter_type(sig,i,at);
            const auto arg="arg"+std::to_string(i);
            params+=","+type+" "+arg;
            if(!args.empty())args+=",";
            args+="std::forward<"+type+">("+arg+")";
        }
        indirect_identities.insert(sig.name);
        return "({auto "+target+"="+value+";"+runtime_type+"{+[]("+params+")->"+result+"{return "+
            mangle("hf_",sig.name)+"("+args+");},"+(nested?target+".data,std::move("+target+".owner)":"nullptr,{}")+
            ",hn::"+mangle("FID_",sig.name)+(nested?",+[](const void* data,hn::FileScan& scan,unsigned depth){return hn::file_in("+
                ct(from,at)+"{data,{}},scan,depth);}":"")+"};})";
    }
    std::string dynamic_pack(const std::string& value,const std::string& type,const Node& at,bool writable){
        if(type=="@dynamic")return value;
        if(type=="void"||type.empty())return "hn::dynamic_scalar(hn::Value(hua::VoidValue{}))";
        if(indirect_scalar(type))return "hn::dynamic_scalar(hn::to_value("+value+"))";
        if(type=="@function")return "hn::dynamic_object("+value+")";
        if(type.starts_with("multi<")){
            const auto items=type_arguments(type,"multi");
            const auto tuple=fresh("tuple");
            std::string code="({auto "+tuple+"="+value+";hn::dynamic_multiple({";
            for(std::size_t i=0;i<items.size();++i){
                if(i)code+=",";
                code+=dynamic_pack("std::get<"+std::to_string(i)+">(std::move("+tuple+"))",items[i],at,writable);
            }
            return code+"});})";
        }
        if(type.starts_with("fn:"))return "hn::dynamic_object("+dynamic_function(value,type,at)+")";
        if(type.starts_with("callable:"))return "hn::dynamic_object("+dynamic_function(value,type,at)+")";
        return "hn::dynamic_object<"+ct(type,at)+">("+value+","+quoted(type)+","+(writable?"true":"false")+")";
    }
    std::string dynamic_unpack(const std::string& value,const std::string& type,const Node& at,const std::string& site,
                               bool mut=false,const std::string& contextual="false"){
        if(type=="@dynamic"){
            if(mut)return "hn::dynamic_mut("+value+","+site+")";
            return "hn::capture_value("+value+")";
        }
        if(type.starts_with("multi<")){
            const auto types=type_arguments(type,"multi");
            const auto packed=fresh("pack"), items=fresh("items");
            std::string code="({auto "+packed+"="+value+";const auto& "+items+"=hn::unpack_dynamic("+
                packed+","+std::to_string(types.size())+","+site+");"+ct(type,at)+"{";
            for(std::size_t i=0;i<types.size();++i){
                if(i)code+=",";
                code+=dynamic_unpack(items+"["+std::to_string(i)+"]",types[i],at,site);
            }
            return code+"};})";
        }
        const auto args=value+","+quoted(type)+","+site;
        if(is_interface(type)){
            const auto target=ct(type,at);
            std::string body="([&]()->"+target+"{";
            body+="if(hn::dynamic_is<"+target+">("+value+"))return "+
                (mut?target+"{hn::dynamic_borrow<"+target+">("+args+").object.alias()}":
                     "hn::dynamic_take<"+target+">("+args+")")+";";
            const auto relation=m.relations.find(type);
            if(relation!=m.relations.end())for(const auto& concrete:relation->second){
                if(!interface_tags.contains(concrete))continue;
                const auto concrete_type=ct(concrete,at);
                const auto concrete_args=value+","+quoted(concrete)+","+site;
                const auto tag=std::to_string(interface_tags.at(concrete));
                body+="if(hn::dynamic_is<"+concrete_type+">("+value+"))return hn::"+
                    (mut?"interface_borrow":"interface_value")+"<"+target+">("+
                    (mut?"hn::dynamic_borrow<":"hn::dynamic_exact<")+concrete_type+">("+concrete_args+"),"+
                    tag+(mut?"":" ,false")+");";
            }
            return body+(mut?"(void)hn::dynamic_mut("+value+","+site+");":"")+
                "return hn::dynamic_take<"+target+">("+args+");})()";
        }
        if(mut){
            const auto helper=record_type(type)||is_fixed(type)?"dynamic_borrow":"dynamic_mut_value";
            return "hn::"+std::string(helper)+"<"+ct(type,at)+">("+args+")";
        }
        if(indirect_scalar(type))return "hn::dynamic_convert<"+ct(type,at)+">("+args+","+contextual+")";
        return "hn::dynamic_take<"+ct(type,at)+">("+args+")";
    }
    std::string dynamic_function(const std::string& value,const std::string& from,const Node& at){
        const bool indirect=from.starts_with("callable:");
        const auto* fn=indirect?nullptr:m.functions.at(from.substr(3));
        const auto sig=indirect?indirect_signatures.at(from):signature(*fn);
        if(!dynamic_signature(sig))no(at,"unsupported dynamic function signature");
        const bool nested=fn&&m.nested.contains(fn);
        if(fn&&!nested)need(fn);
        const auto target=fresh("function");
        const bool bound=!sig.self.empty();
        const bool interface=is_interface(sig.self);
        std::string args,conversions;
        if(nested)args=ct(from,at)+"{function.data,function.owner}";
        if(bound){
            conversions="const auto& method=*static_cast<const "+ct(from,at)+"*>(function.data);";
            if(interface){
                if(sig.self_mut)conversions+="if(!method.receiver.object.writable)hn::fail(site,\"mutating method requires mutable receiver\",\"E4007\");";
                conversions+=ct(sig.self,at)+" receiver{method.receiver.object.alias()};";
            }else{
                if(sig.self_mut)conversions+="if(!method.writable)hn::fail(site,\"mutating method requires mutable receiver\",\"E4007\");";
                conversions+=ct(sig.self,at)+" receiver{hn::RecordStorage<hn::"+mangle("SB_",sig.self)+">::alias(method.receiver)};";
            }
            args="receiver";
        }
        for(std::size_t i=0;i<sig.types.size();++i){
            const auto arg="arg"+std::to_string(i);
            const auto index=std::to_string(i);
            const auto& type=sig.types[i];
            auto operand=dynamic_unpack("values["+index+"]",type,at,"site",sig.mut[i],"contextual["+index+"]");
            if(!sig.mut[i]&&type!="@dynamic"&&!indirect_scalar(type))operand="hn::capture_value("+operand+")";
            const bool borrow=sig.mut[i]&&(record_type(type)||is_fixed(type));
            conversions+=std::string(borrow&&!is_interface(type)?"auto& ":"auto ")+arg+"="+operand+";";
            if(!args.empty())args+=",";
            args+=borrow?arg:"std::move("+arg+")";
        }
        const auto callee=indirect?"static_cast<const "+ct(from,at)+"*>(function.data)->invoke":mangle("hf_",sig.name);
        const auto call=callee+"("+args+(interface?",site":"")+")";
        const std::string enter="const auto depth=hn::enter<NATIVE_COUNT_STEPS>(site);";
        const auto result=enter+(sig.result.empty()?call+";hn::leave(depth);return hn::dynamic_scalar(hn::Value(hua::VoidValue{}));":
            "auto result="+call+";hn::leave(depth);return "+dynamic_pack("std::move(result)",sig.result,at,true)+";");
        std::string code="({auto "+target+"="+value+";";
        std::string data=nested?target+".data,std::move("+target+".owner)":"nullptr,{}";
        std::string identity=quoted(from);
        if(indirect||bound){
            const auto holder=fresh("owner");
            code+="auto "+holder+"=hua::managed<hn::DynamicHolder<"+ct(from,at)+">>(std::move("+target+"));";
            data="&"+holder+"->value,"+holder;
            if(indirect||interface)identity="std::string("+holder+"->value.type)";
        }
        std::string resources;
        if(indirect||bound)resources=",+[](const hn::DynamicFunction& f,hn::FileScan& scan,unsigned depth){return hn::file_in(*static_cast<const "+
            ct(from,at)+"*>(f.data),scan,depth);}";
        else if(nested)resources=",+[](const hn::DynamicFunction& f,hn::FileScan& scan,unsigned depth){return hn::file_in("+
            ct(from,at)+"{f.data,{}},scan,depth);}";
        return code+"hn::DynamicFunction{+[](const hn::DynamicFunction& function,"+
            "std::span<const hn::DynamicValue> values,std::span<const bool> contextual,const SourceSpan& site){"+
            conversions+result+"},"+data+","+identity+","+std::to_string(sig.types.size())+resources+"};})";
    }
    // Ordinary values and container handles are read now. A record obtained through a
    // place is copied at binding time, after evaluation of the entire argument list.
    std::string pending_return;
    std::string conversion_site(const Node& site){
        return !pending_return.empty()&&site.kind==N::Return?"native_return_site":span(site.span);
    }
    bool return_can_fail(const std::string& from,const std::string& to){
        if(from==to)return false;
        if(from=="@dynamic"&&to!="@dynamic")return true;
        if(hua::numeric_spec(to)&&hua::numeric_spec(from))
            return !hua::numeric_widening(to,from);
        return is_sequence(to)&&is_sequence(from);
    }
    std::string numeric_conversion(const std::string& e,const std::string& from,const std::string& to,
                                   const Node& site,const Node& source){
        const auto widening=hua::numeric_widening(to,from)?"true":"false";
        return "hn::numeric_coerce<"+ct(to,site)+","+widening+">("+e+","+conversion_site(site)+","+
            (contextual_literal(source)?"true":"false")+")";
    }
    std::string fixed_literal(const Node& n,const std::string& type,const Node& site){
        if(n.kind!=N::Array)return {};
        const auto element=element_type(type);
        std::string items;
        for(const auto& child:n.children){
            std::string value;
            if(is_fixed(element))value=fixed_literal(*child,element,site);
            else if(numeric_literal(*child)&&hua::numeric_spec(element))
                value=ty(*child)==element?expr(*child):numeric_conversion(expr(*child),ty(*child),element,site,*child);
            else if(child->kind==N::Boolean&&element=="bool")value=scalar_literal(*child);
            if(value.empty())return {};
            if(!items.empty())items+=",";
            items+=value;
        }
        const auto count=type.substr(1,type.find(']')-1);
        return "hn::make_fixed_literal<"+ct(element,site)+","+count+">({"+items+"},"+conversion_site(site)+")";
    }
    const Node& conversion_source(const Node& n){
        if((n.kind==N::Let||n.kind==N::Var||n.kind==N::Return||n.kind==N::Assignment||n.kind==N::FieldInit)&&!n.children.empty())
            return *n.children.back();
        return n;
    }
    std::string array_context(const Node& n){
        const auto* e=&conversion_source(n);
        if(e->kind!=N::Array)return "hn::ArrayContext{}";
        std::string out="hn::ArrayContext{true,{";
        for(std::size_t i=0;i<e->children.size();++i){
            if(i)out+=",";
            const auto& child=*e->children[i];
            out+=child.kind==N::Array?array_context(child):"hn::ArrayContext{"+std::string(numeric_literal(child)?"true":"false")+",{}}";
        }
        return out+"}}";
    }
    bool record_type(const std::string& t){return is_struct(t)||is_enum(t)||is_interface(t);}
    std::string sequence_expr(const Node& n){
        if(is_fixed(ty(n))&&n.kind==N::Index){
            const auto& parent=*n.children[0];
            if(is_sequence(ty(parent))){
                const auto storage=fresh("sequence"),index=fresh("index");
                return "({auto "+storage+"="+sequence_expr(parent)+";auto "+index+"="+expr(*n.children[1])+
                    ";hn::nested_fixed_view("+storage+","+index+","+span(n.span)+");})";
            }
        }
        auto value=expr(n);
        return is_fixed(ty(n))?"hn::sequence_view("+value+")":value;
    }
    std::string binding_value(const Node& binding,const std::string& type,const std::string& value){
        if(type.starts_with("fn:")){
            const auto it=m.types.find(&binding);
            if(it!=m.types.end()&&!it->second.empty()&&is_interface(signature(*m.functions.at(type.substr(3))).self))
                return "hn::check_bound_type("+value+","+quoted(it->second)+","+span(binding.span)+")";
        }
        if(binding.kind!=N::Let)return value;
        if(is_fixed(type))return "hn::capture_value("+value+")";
        if(is_interface(type))return "hn::readonly_interface("+value+")";
        return value;
    }
    std::string fixed_from_view(const std::string& value,const std::string& type,const Node& site){
        return "hn::make_fixed<"+ct(element_type(type),site)+","+type.substr(1,type.find(']')-1)+">("+
            value+","+span(site.span)+")";
    }
    bool place_expression(const Node& n){
        return n.kind==N::Name||(n.kind==N::Member&&place_expression(*n.children[0]));
    }
    std::string evaluated(const Node& n,const std::string& type,const std::string& name,bool defer_record_copy){
        const bool borrow=defer_record_copy&&record_type(type)&&ty(n)==type&&place_expression(n);
        return (borrow?"auto&& ":ct(type,n)+" ")+name+"="+coerce(expr(n),ty(n),type,n)+";";
    }
    bool simple(const Node& n){return n.kind==N::Name||n.kind==N::Integer||n.kind==N::Float||n.kind==N::Boolean||n.kind==N::String||n.kind==N::Nil;}
    std::string value_literal(const Value& v,const Node& n){
        const auto signed_literal=[](std::int64_t value){
            if(value==std::numeric_limits<std::int64_t>::min())return std::string("(-9223372036854775807LL-1)");
            return std::to_string(value)+"LL";
        };
        const auto real_literal=[](double value){
            char buffer[64];
            std::snprintf(buffer,sizeof buffer,"%a",value);
            return std::string(buffer);
        };
        if(const auto p=hua::get_if<std::int64_t>(&v.data))return "hn::I("+signed_literal(*p)+")";
        if(const auto p=hua::get_if<double>(&v.data))return "hn::F("+real_literal(*p)+")";
        if(const auto p=hua::get_if<bool>(&v.data))return *p?"true":"false";
        if(const auto p=hua::string_data(&v.data))return literal(*p);
        if(const auto number=hua::get_if<hua::NumericValue>(&v.data)){
            std::string value;
            if(const auto u=hua::get_if<std::uint64_t>(&number->number))value=std::to_string(*u)+"ULL";
            else if(const auto i=hua::get_if<std::int64_t>(&number->number))value=signed_literal(*i);
            else value=real_literal(hua::get<double>(number->number))+(number->type=="f32"?"f":"");
            return ct(number->type,n)+"{"+value+"}";
        }
        if(v.data.index()==0)return "std::nullopt";
        no(n,"literal kind");
    }
    std::string scalar_literal(const Node& n){return value_literal(hua::literal(n),n);}
    static bool may_rebind(const Node& n){
        if(n.kind==N::Call)return true;
        return std::any_of(n.children.begin(),n.children.end(),[](const auto& child){return may_rebind(*child);});
    }
    std::set<std::string> located_records;
    std::set<std::string> module_bindings;
    std::unordered_set<const Node*> stable_local_fields;
    void plan_locations(const Node& n,const Node* parent=nullptr,const Node* function=nullptr){
        if(n.kind==N::Function)function=&n;
        const auto mark=[&](const Node& target){
            if(target.kind!=N::Member)return;
            auto t=m.types.find(target.children[0].get());
            if(t!=m.types.end()&&is_struct(t->second))located_records.insert(t->second);
        };
        if(n.kind==N::MultiAssignment){
            for(const auto& target:n.children[0]->children)mark(*target);
        }else if(n.kind==N::Assignment||n.kind==N::Update){
            const auto& target=*n.children[0];
            if(target.kind==N::Member){
                const auto& receiver=*target.children[0];
                if(function&&receiver.kind==N::Name&&receiver.text!="self"&&!module_bindings.contains(receiver.text)&&
                   std::none_of(function->children.begin(),function->children.end(),[&](const auto& node){
                       return node->kind==N::Parameter&&declared_name(*node)==receiver.text;
                   })){
                    // Calls cannot replace a private local record. Resolve the C++ field
                    // after the RHS in case a mut borrow promoted that same record.
                    stable_local_fields.insert(&n);
                }else if(!place_expression(target)||(n.kind==N::Assignment&&may_rebind(*n.children[1]))){
                    mark(target);
                }
            }
        }
        if(n.kind==N::Call){
            const auto type=m.types.find(n.children[0].get());
            const Node* known=nullptr;
            if(type!=m.types.end()&&type->second.starts_with("fn:")){
                auto fn=m.functions.find(type->second.substr(3));
                if(fn!=m.functions.end())known=fn->second;
            }
            bool dynamic=type==m.types.end()||type->second.empty();
            if(known)for(const auto& child:known->children)
                if(child->kind==N::Parameter&&child->children.empty())dynamic=true;
            if(dynamic)for(std::size_t i=1;i<n.children.size();++i){
                const auto actual=m.types.find(n.children[i].get());
                if(actual!=m.types.end()&&is_struct(actual->second))located_records.insert(actual->second);
            }
        }
        if(n.kind==N::Member&&(!parent||parent->kind!=N::Call||parent->children[0].get()!=&n)){
            const auto type=m.types.find(&n);
            if(type!=m.types.end()&&type->second.starts_with("fn:")){
                mark(n);
                auto owner=m.types.find(n.children[0].get());
                if(owner!=m.types.end()&&is_interface(owner->second)){
                    auto relation=m.relations.find(owner->second);
                    if(relation!=m.relations.end())
                        for(const auto& concrete:relation->second)
                            if(is_struct(concrete))located_records.insert(concrete);
                }
            }
        }
        for(const auto& child:n.children)plan_locations(*child,&n,function);
    }
    std::string field_access(const std::string& base,const std::string& type,const std::string& field){
        return base+(located_records.contains(type)?".fields.get().":".")+mangle("f_",field);
    }
    // An assignable C++ lvalue for an assignment target or a `mut` argument.
    std::string lvalue(const Node& n){
        if(n.kind==N::Name){auto l=find(n.text,n);if(!l)no(n,"assignment to a non-local name `"+n.text+"` line "+std::to_string(n.span.line));return l->cpp;}
        if(n.kind==N::Member){if(!is_struct(ty(*n.children[0])))no(n,"field of a non-struct");return field_access(lvalue(*n.children[0]),ty(*n.children[0]),n.text);}
        if(n.kind==N::Index&&ty(*n.children[0]).starts_with("[]"))return "hn::slot("+lvalue(*n.children[0])+","+expr(*n.children[1])+","+span(n.span)+")";
        no(n,"assignment target "+std::string(node_name(n.kind))+" of `"+(n.children.empty()?std::string():ty(*n.children[0]))+"` line "+std::to_string(n.span.line));
    }
    std::string local_type(const Node& n){
        if(n.kind==N::Name){auto l=find(n.text,n);if(!l)no(n,"unknown local `"+n.text+"` line "+std::to_string(n.span.line));return l->type;}
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
        if(lt==rt&&lt==out&&!exact_type(lt).empty()&&(op=="+"||op=="-"||op=="*"||(op=="/"&&hua::numeric_spec(lt)->category=='f')))
            return "hn::exact_arithmetic<'"+op+"'>("+l+","+r+","+s+")";
        if(lt==rt&&!exact_type(lt).empty()&&compare.contains(op))return "("+l+".value"+op+r+".value)";
        return "hn::general_binary<"+ct(out,at)+">(\""+op+"\","+l+","+r+","+s+(contextual?",true":"")+")";
    }
    std::string condition(const Node& n){
        const auto value=expr(n);
        if(ty(n)=="@dynamic")return "hua::as_bool(hn::to_value("+value+"),"+span(n.span)+")";
        return value;
    }
    std::string expr(const Node& n){
        if(numeric_literal(n)||n.kind==N::String||n.kind==N::Boolean||n.kind==N::Nil)return scalar_literal(n);
        switch(n.kind){
        case N::Name:{
            if(auto l=find(n.text,n)){
                if(l->type.ends_with('?')&&ty(n)==l->type.substr(0,l->type.size()-1))return "(*"+l->cpp+")";
                return l->cpp;
            }
            if(auto c=constants.find(n.text);c!=constants.end())return value_literal(m.constants.at(c->second),n);
            const auto& type=ty(n);
            if(type.starts_with("fn:")){
                const auto target=m.functions.find(type.substr(3));
                if(target!=m.functions.end()&&!m.nested.contains(target->second)){
                    const auto sig=signature(*target->second);
                    if(sig.self.empty()){need(target->second);return ct(type,n)+"{}";}
                }
            }
            no(n,"unresolved name "+n.text);}
        case N::Unary:{
            const auto& t=ty(*n.children[0]);auto v=expr(*n.children[0]);
            if(n.text=="!"&&t=="bool")return "(!"+v+")";
            if(n.text=="-"&&t=="int")return "hn::neg("+v+","+span(n.span)+")";
            if(n.text=="-"&&t=="float")return "(-"+v+")";
            return "hn::general_unary<"+ct(ty(n),n)+">(\""+n.text+"\","+v+","+span(n.span)+")";}
        case N::Binary:{
            const auto& a=*n.children[0];const auto& b=*n.children[1];
            if(n.text=="&&"||n.text=="||")return "("+condition(a)+n.text+condition(b)+")";
            const auto& lt=ty(a);const auto& rt=ty(b);
            if(simple(a)&&simple(b))return binary(n.text,expr(a),lt,expr(b),rt,ty(n),n,numeric_literal(b));
            auto l=fresh("l"),r=fresh("r");
            const auto left_capture=record_type(lt)&&place_expression(a)?"auto&& ":"auto ";
            return "({"+std::string(left_capture)+l+"="+expr(a)+";auto&& "+r+"="+expr(b)+";"+binary(n.text,l,lt,r,rt,ty(n),n,numeric_literal(b))+";})";}
        case N::Member:{
            const auto& t=ty(*n.children[0]);
            if(is_struct(t)){
                const auto& member_type=ty(n);
                if(member_type.starts_with("fn:")){
                    const auto& receiver=*n.children[0];
                    if(!located_records.contains(t))no(n,"bound method without stable receiver");
                    const auto sig=signature(*m.functions.at(member_type.substr(3)));
                    const auto permission=m.writable.find(&receiver);
                    const bool writable=permission!=m.writable.end()&&permission->second;
                    if(sig.self_mut&&!writable)no(n,"bound mutating method requires writable receiver");
                    const auto method=ct(member_type,n);
                    if(possible_place(receiver)&&writable)
                        return location_expression([&]{
                            const auto alias=locate_receiver(receiver);
                            return method+"{"+alias+".fields.pin(),true}";
                        });
                    if(receiver.kind==N::Index&&is_sequence(ty(*receiver.children[0])))
                        return location_expression([&]{
                            const auto owner=fresh("receiver_sequence");
                            line("auto "+owner+"="+sequence_expr(*receiver.children[0])+";");
                            const auto index=fresh("receiver_index");
                            line("auto "+index+"="+expr(*receiver.children[1])+";");
                            return method+"{hn::sequence_record("+owner+","+index+","+span(receiver.span)+").fields.pin(),false}";
                        });
                    if(place_expression(receiver)&&receiver.kind!=N::Name)
                        no(n,"bound method of a read-only member");
                    const auto holder=fresh("receiver");
                    return "({auto "+holder+"="+expr(receiver)+";"+method+"{"+holder+".fields.pin(),false};})";
                }
                // Read the field in place when the object is itself a place; otherwise copy it out of the temporary.
                const auto& o=*n.children[0];
                if(o.kind==N::Name||o.kind==N::Member)return field_access(expr(o),t,n.text);
                auto t0=fresh("o");return "({auto "+t0+"="+expr(o)+";auto "+t0+"f=std::move("+field_access(t0,t,n.text)+");"+t0+"f;})";
            }
            if(is_interface(t)&&ty(n).starts_with("fn:")){
                const auto receiver=fresh("receiver");
                std::string code="({auto&& "+receiver+"="+expr(*n.children[0])+";std::string identity;";
                const auto relation=m.relations.find(t);
                if(relation==m.relations.end())no(n,"bound interface method without a concrete type");
                code+="switch("+receiver+".object.tag){";
                for(const auto& concrete:relation->second){
                    if(!interface_tags.contains(concrete))continue;
                    code+="case "+std::to_string(interface_tags.at(concrete))+":identity="+quoted("fn:"+concrete+"."+n.text)+";break;";
                }
                code+="default:hn::fail("+span(n.span)+",\"interface prototype requires a concrete receiver\",\"E4003\");}";
                return code+ct(ty(n),n)+"{"+ct(t,n)+"{"+receiver+".object.alias()},std::move(identity)};})";
            }
            if(n.text=="len"&&(t=="string"||is_sequence(t)||is_map(t)))return "hn::len("+(is_fixed(t)?sequence_expr(*n.children[0]):expr(*n.children[0]))+")";
            no(n,"member `"+n.text+"` of `"+t+"`");}
        case N::Index:{
            const auto& base=*n.children[0];
            if(base.kind==N::Name)if(const auto* local=find(base.text,base);local&&local->stack_functions){
                return "hn::stack_function_at("+local->cpp+","+expr(*n.children[1])+","+span(n.span)+")";
            }
            const auto& t=ty(base);
            if(is_sequence(t)){auto b=fresh("b"),i=fresh("i");return "({auto "+b+"="+sequence_expr(*n.children[0])+";auto "+i+"="+expr(*n.children[1])+";auto "+b+"v=hn::read_element("+b+","+i+","+span(n.span)+");"+b+"v;})";}
            if(is_map(t)){const auto a=map_parts(t);auto b=fresh("b"),k=fresh("k");
                return "({auto "+b+"="+expr(*n.children[0])+";"+ct(a[0],n)+" "+k+"="+expr(*n.children[1])+";hn::at("+b+","+k+","+span(n.span)+");})";}
            no(n,"index into `"+t+"`");}
        case N::Slice:{
            const auto& base=*n.children[0];
            if(!is_sequence(ty(base)))no(n,"slice of non-sequence");
            const auto container=fresh("slice"),start=fresh("start"),end=fresh("end");
            return "({auto "+container+"="+sequence_expr(base)+";hn::I "+start+"="+
                (n.children[1]->kind==N::Omitted?"0":expr(*n.children[1]))+";hn::I "+end+"="+
                (n.children[2]->kind==N::Omitted?container+".length":expr(*n.children[2]))+";hn::sub_slice(std::move("+
                container+"),"+start+","+end+","+span(n.span)+");})";
        }
        case N::Pack:return pack(n,type_arguments(ty(n),"multi"));
        case N::Array:{
            const auto& t=ty(n);if(!t.starts_with("[]"))no(n,"fixed array literal");
            if(t=="[]"&&n.children.empty())return "hn::EmptySlice{}";
            if(t=="[]@function"){
                const auto items=fresh("functions");
                std::string code="({std::vector<hn::DynamicFunction> "+items+";"+items+".reserve("+std::to_string(n.children.size())+");";
                for(const auto& child:n.children){
                    const auto value=coerce(expr(*child),ty(*child),"@function",*child);
                    code+="hn::append_function("+items+","+value+","+span(child->span)+");";
                }
                return code+"hn::Slice<hn::DynamicFunction>::of(std::move("+items+"));})";
            }
            const auto element=t.substr(2);std::string s="hn::Slice<"+ct(element,n)+">::of({";
            for(std::size_t i=0;i<n.children.size();++i){if(i)s+=",";s+=coerce(expr(*n.children[i]),ty(*n.children[i]),element,n);}
            return s+"})";}
        case N::StructLiteral:{
            if(is_enum(n.text)){
                auto t=fresh("e");std::string s="({"+ct(n.text,n)+" "+t+"{};";const auto& info=enum_info(n.text);
                for(const auto& c:n.children){
                    const auto& value=*c->children[0];
                    if(c->text=="$case"){if(value.kind!=N::String)no(*c,"computed enum tag");const Value tag=hua::literal(value);const auto* text=hua::string_data(&tag.data);if(!text)no(*c,"enum tag");s+=t+".tag="+std::to_string(tag_of(n.text,*text,*c))+";";continue;}
                    if(value.kind==N::Nil)continue;
                    std::string type;for(const auto& [variant,fields]:info.fields)for(const auto& [name,ft]:fields)if(name==c->text)type=ft;
                    if(type.empty()||type=="void")no(*c,"enum payload field");
                    s+=t+"."+mangle("f_",c->text)+"="+coerce(expr(value),ty(value),type,*c)+";";
                }
                return s+t+";})";
            }
            if(!is_struct(n.text))no(n,"literal of `"+n.text+"`");
            const auto& decl=*m.structures.at(n.text);std::map<std::string,std::string> types;
            for(const auto& f:decl.children)if(f->kind==N::Field)types[f->text]=type_name(*f->children[0]);
            auto t=fresh("s");std::string s="({"+mangle("S_",n.text)+" "+t+"{};";
            for(const auto& c:n.children){auto f=types.find(c->text);if(f==types.end())no(*c,"unknown field");
                s+=field_access(t,n.text,c->text)+"="+coerce(expr(*c->children[0]),ty(*c->children[0]),f->second,*c)+";";}
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
    std::string pack(const Node& n,const std::vector<std::string>& types){
        std::string code="({",values;
        for(std::size_t i=0;i<n.children.size();++i){
            const auto& item=*n.children[i];
            const auto temp=fresh("item");
            if(i)values+=",";
            if(is_fixed(ty(item))&&ty(item)==types[i]){
                code+="auto "+temp+"="+sequence_expr(item)+";";
                values+=fixed_from_view(temp,types[i],n);
            }else{
                code+=evaluated(item,ty(item),temp,true);
                values+=coerce(temp,ty(item),types[i],item);
            }
        }
        // List initialization evaluates tuple conversions left to right.
        std::string pack_type="multi<";
        for(std::size_t i=0;i<types.size();++i){
            if(i)pack_type+=",";
            pack_type+=types[i];
        }
        return code+ct(pack_type+">",n)+"{"+values+"};})";
    }
    std::string indirect_call(const std::string& kind,const Signature& sig,const std::string& target,
                              const std::string& args,const Node& site){
        const auto fallback=target+".invoke("+args+")";
        const auto family=indirect_candidates.find(kind);
        if(family==indirect_candidates.end()||family->second.size()!=2||!indirect_scalar(sig.result)||
           !std::all_of(sig.types.begin(),sig.types.end(),[&](const auto& t){return indirect_scalar(t);}))
            return fallback;
        const auto small_expression=[](auto&& visit,const Node& node,unsigned& left)->bool{
            if(left==0)return false;
            --left;
            switch(node.kind){
            case N::Name:case N::Integer:case N::Float:case N::Boolean:return true;
            case N::Unary:case N::Binary:
                return std::all_of(node.children.begin(),node.children.end(),[&](const auto& c){return visit(visit,*c,left);});
            default:return false;
            }
        };
        for(const auto& name:family->second){
            const auto& fn=*m.functions.at(name);
            const auto& body=*fn.children.back();
            if(body.kind!=N::Block||body.children.size()!=1||body.children[0]->kind!=N::Return||
               body.children[0]->children.size()!=1)return fallback;
            unsigned max_nodes=12;
            if(!small_expression(small_expression,*body.children[0]->children[0],max_nodes)||!borrows_self(fn))
                return fallback;
        }
        std::string code="([&](){";
        for(const auto& name:family->second){
            const auto* fn=m.functions.at(name);
            indirect_identities.insert(name);
            code+="if("+target+".type.data()==hn::"+mangle("FID_",name)+")return "+mangle("hf_",name)+"(";
            if(m.nested.contains(fn)){
                code+=ct("fn:"+name,site)+"{"+target+".data,{}}";
                if(!args.empty())code+=",";
            }else need(fn);
            code+=args+");";
        }
        return code+"return "+fallback+";})()";
    }
    // A call; as a statement its value (if any) is dropped.
    const std::string* deferred_callee{};
    std::string call(const Node& n,bool statement){
        const auto& callee=*n.children[0];
        const bool method=callee.kind==N::Member&&ty(callee).starts_with("fn:");
        const auto& kind=ty(callee);const std::size_t count=n.children.size()-1;
        auto arg=[&](std::size_t i)->const Node&{return *n.children[i+1];};
        const bool indirect=kind.starts_with("callable:");
        if(kind=="@function"||kind=="@dynamic"){
            const auto target=fresh("callee"),values=fresh("args");
            const auto site=span(n.span);
            std::string code="({auto "+target+"="+(deferred_callee?*deferred_callee:expr(callee))+";";
            std::string args,contextual;
            for(std::size_t i=0;i<count;++i){
                if(i){args+=",";contextual+=",";}
                args+=given?dynamic_pack((*given)[i],ty(arg(i)),arg(i),false):dynamic_argument(arg(i));
                contextual+=contextual_literal(arg(i))?"true":"false";
            }
            code+="const std::array<hn::DynamicValue,"+std::to_string(count)+"> "+values+"{"+args+"};";
            std::string function=target;
            if(kind=="@dynamic"){
                function=fresh("function");
                code+="const auto& "+function+"=hn::dynamic_callee("+target+","+site+");";
            }
            code+="if("+function+".arity!="+std::to_string(count)+")hn::fail("+site+",\"function argument count mismatch\",\"E4003\");";
            return code+function+".entry("+function+","+values+",std::array<bool,"+
                std::to_string(count)+">{"+contextual+"},"+site+");})";
        }
        if(kind.starts_with("fn:")||indirect){
            const auto* f=indirect?nullptr:m.functions.at(kind.substr(3));
            const auto sig=indirect?indirect_signatures.at(kind):signature(*f);
            const bool bound=!method&&((is_struct(sig.self)&&located_records.contains(sig.self))||is_interface(sig.self));
            if(sig.names.size()!=count){
                if(!indirect)no(n,"argument count");
                std::string code="({(void)("+(deferred_callee?*deferred_callee:expr(callee))+");";
                for(std::size_t i=0;i<count;++i)code+="(void)("+(given?(*given)[i]:expr(arg(i)))+");";
                return code+"hn::fail("+span(n.span)+",\"function argument count mismatch\",\"E4003\");"+
                    (sig.result.empty()?"hn::Unit":ct(sig.result,n))+"{};})";
            }
            const bool nested=f&&m.nested.contains(f);
            if(f&&!nested)need(f);
            if((method||bound)!=!sig.self.empty())no(n,"method called as a function (or the reverse)");
            std::string s="({",list,bindings,target;
            if(nested||indirect){
                target=fresh("callee");
                // Save the value before arguments which can rebind a mutable slot.
                // An immutable local and an already saved defer value have their
                // own owners through argument evaluation and the call.
                const auto* local=callee.kind==N::Name?find(callee.text,callee):nullptr;
                const bool borrow=deferred_callee||(local&&local->immutable_callable);
                s+=std::string(borrow?"const auto& ":"auto ")+target+"="+(deferred_callee?*deferred_callee:expr(callee))+";";
                if(nested)list=target;
            }else if(bound){
                const auto saved=fresh("bound_method"),receiver=fresh("receiver");
                s+="auto "+saved+"="+(deferred_callee?*deferred_callee:expr(callee))+";";
                if(is_interface(sig.self))
                    s+=ct(sig.self,n)+" "+receiver+"{"+saved+".receiver.object.alias()};";
                else s+=ct(sig.self,n)+" "+receiver+"{hn::RecordStorage<hn::"+mangle("SB_",sig.self)+">::alias("+saved+".receiver)};";
                list=receiver;
            }else if(!method&&(callee.kind!=N::Name||find(callee.text,callee))){
                // The type identifies its entry point, but evaluation may have side effects.
                s+="(void)("+expr(callee)+");";
            }
            if(method){
                const auto& receiver=*callee.children[0];
                auto r=fresh("self");
                s+="auto&& "+r+"="+(sig.self_mut?lvalue(receiver):expr(receiver))+";";
                list=r;
            }
            for(std::size_t i=0;i<count;++i){
                if(i||method||nested||bound)list+=",";
                if(given){list+=(*given)[i];continue;}
                const auto a=fresh("arg");
                const auto& parameter=sig.types[i];
                const auto& actual=ty(arg(i));
                if(parameter=="@dynamic"){
                    s+="auto "+a+"="+dynamic_argument(arg(i))+";";
                    if(sig.mut[i])bindings+=a+"=hn::dynamic_mut("+a+","+span(n.span)+");";
                    else bindings+=a+"=hn::capture_value("+a+");";
                }else if(indirect&&indirect_scalar(parameter)&&(indirect_scalar(actual)||actual=="@dynamic")&&actual!=parameter){
                    const auto raw=fresh("evaluated");
                    s+="auto "+raw+"="+expr(arg(i))+";";
                    bindings+=ct(parameter,n)+" "+a+"=hn::dynamic_convert<"+ct(parameter,n)+">(hn::to_value("+raw+"),"+
                        quoted(parameter)+","+span(n.span)+","+(contextual_literal(arg(i))?"true":"false")+");";
                }else if(sig.mut[i]&&is_interface(parameter)&&actual!=parameter){
                    if(!(is_interface(actual)||is_struct(actual)||is_enum(actual))||!m.relations.at(parameter).contains(actual))
                        no(arg(i),"mutable interface conversion from "+actual);
                    s+="auto "+a+"=hn::"+(is_interface(actual)?"interface_alias":"interface_borrow")+"<"+ct(parameter,arg(i))+">("+
                        lvalue(arg(i))+(is_interface(actual)?"":","+std::to_string(interface_tags.at(actual)))+");";
                }else if(sig.mut[i]&&(record_type(parameter)||is_fixed(parameter))){
                    if(parameter!=actual)no(arg(i),"mutable fixed array requires identical layout");
                    if(arg(i).kind==N::Index&&is_sequence(ty(*arg(i).children[0]))){
                        const auto storage=fresh("borrowed_storage"),index=fresh("borrowed_index");
                        s+="auto "+storage+"="+sequence_expr(*arg(i).children[0])+";auto "+index+"="+
                            expr(*arg(i).children[1])+";auto& "+a+"=hn::slot("+storage+","+index+","+span(arg(i).span)+");";
                    }else s+="auto& "+a+"="+lvalue(arg(i))+";";
                    if(is_fixed(parameter))
                        bindings+="hn::require_writable("+a+".view,"+span(n.span)+",\"mut parameter requires mutable argument\");";
                }else if(sig.mut[i]&&parameter.starts_with("[]")&&is_fixed(actual)&&element_type(parameter)==element_type(actual)){
                    s+="auto "+a+"="+sequence_expr(arg(i))+";";
                    bindings+="hn::require_writable("+a+","+span(n.span)+",\"mut parameter requires mutable argument\");";
                }else if(is_fixed(parameter)&&parameter==actual){
                    const auto view=fresh("fixed_view");
                    s+="auto "+view+"="+sequence_expr(arg(i))+";";
                    bindings+=ct(parameter,n)+" "+a+"="+fixed_from_view(view,parameter,n)+";";
                }else if(parameter!=actual&&((hua::numeric_spec(parameter)&&hua::numeric_spec(actual))||
                         (is_sequence(parameter)&&is_sequence(actual)))){
                    auto raw=fresh("evaluated");
                    s+="auto "+raw+"="+expr(arg(i))+";";
                    if(hua::numeric_spec(parameter))
                        bindings+=ct(parameter,n)+" "+a+"="+numeric_conversion(raw,actual,parameter,n,arg(i))+";";
                    else bindings+=ct(parameter,n)+" "+a+"="+coerce(raw,actual,parameter,arg(i))+";";
                }else if(is_interface(parameter)&&record_type(actual)&&place_expression(arg(i))){
                    s+="auto&& "+a+"="+expr(arg(i))+";";
                    list+=coerce(a,actual,parameter,arg(i));
                    continue;
                }else s+=evaluated(arg(i),parameter,a,true);
                list+=!sig.mut[i]&&(is_fixed(parameter)||is_interface(parameter))?"std::move("+a+")":a;
            }
            auto d=fresh("d");
            const auto enter="const unsigned "+d+"=hn::enter<NATIVE_COUNT_STEPS>("+span(n.span)+");";
            s+=indirect?bindings+enter:enter+bindings;
            if(auto slot=stack_call_slots.find(&n);slot!=stack_call_slots.end()){
                if(!list.empty())list+=",";
                list+="&"+slot->second;
            }
            const auto invoked=indirect?indirect_call(kind,sig,target,list,n):
                mangle("hf_",sig.name)+"("+list+(is_interface(sig.self)?","+span(n.span):"")+")";
            if(sig.result.empty())return s+invoked+";hn::leave("+d+");hn::Unit{};})";
            auto r=fresh("r");return s+"auto "+r+"="+invoked+";hn::leave("+d+");std::move("+r+");})";
        }
        if(!kind.starts_with("builtin:"))no(n,"callee of kind `"+kind+"`");
        const std::string name=kind.substr(8);const auto s=span(n.span);
        auto a=[&](std::size_t i){return given?(*given)[i]:expr(arg(i));};
        auto optional_arg=[&]{
            const auto& input=arg(0);
            if(!given&&input.kind==N::Name)if(auto local=find(input.text,input);local&&local->type.ends_with('?'))
                return local->cpp;
            return a(0);
        };
        if(name=="print"){std::string list;for(std::size_t i=0;i<count;++i){if(i)list+=",";list+="hn::to_value("+a(i)+")";}return "(hn::print("+s+",{"+list+"}),hn::Unit{})";}
        if(name=="len"&&count==1){const auto& t=ty(arg(0));if(t=="string"||is_sequence(t)||is_map(t))return "hn::len("+(is_fixed(t)&&!given?sequence_expr(arg(0)):a(0))+")";}
        if(name=="str"&&count==1)return "hn::str("+a(0)+")";
        if(count==1){
            const auto& input_type=ty(arg(0));
            if((!exact_type(name).empty()&&hua::numeric_spec(input_type))||
               ((name=="int"||name=="float")&&!exact_type(input_type).empty()))
                return "hn::numeric_coerce<"+ct(name,n)+">("+a(0)+","+s+",true)";
        }
        if((name=="sizeof"||name=="alignof")&&count==1){
            auto type=ty(arg(0));
            if(name=="alignof")while(is_fixed(type))type=element_type(type);
            if(const auto size=hua::packed_width(type))
                return "((void)("+a(0)+"),hn::I("+std::to_string(*size)+"))";
        }
        if(name=="ok"){
            if(count==0)return "hn::ok()";
            const auto& parts=type_arguments(ty(n),"Result");const auto payload=!parts.empty()&&!parts[0].empty()?parts[0]:ty(arg(0));
            return "hn::ok<"+ct(payload,n)+">("+coerce(a(0),ty(arg(0)),payload,n)+")";}
        if(name=="err"&&count==1)return "hn::err<"+ct(ty(arg(0)),n)+">("+a(0)+","+s+")";
        if((name=="is_ok"||name=="is_err")&&count==1)return "hn::"+name+"("+a(0)+")";
        if((name=="is_some"||name=="is_none")&&count==1)return "hn::"+name+"("+optional_arg()+")";
        if(name=="unwrap"&&count==1)return "hn::unwrap("+optional_arg()+","+s+")";
        if(name=="unwrap_or"&&count==2){auto x=fresh("u"),d=fresh("d");return "({auto "+x+"="+optional_arg()+";auto "+d+"="+coerce(a(1),ty(arg(1)),ty(n),n)+";hn::unwrap_or(std::move("+x+"),std::move("+d+"));})";}
        if(name=="has"&&count==2&&is_map(ty(arg(0)))){const auto parts=map_parts(ty(arg(0)));auto mv=fresh("m");
            return "({auto "+mv+"="+a(0)+";"+ct(parts[0],n)+" "+mv+"k="+a(1)+";hn::has("+mv+","+mv+"k);})";}
        if(name=="clone"&&count==1){
            const auto& type=ty(arg(0));
            if(might_hold_file(type,{},true))
                return "hn::clone_checked("+a(0)+","+s+")";
            return "hn::clone("+a(0)+")";
        }
        if(name=="unwrap_err"&&count==1)return "hn::unwrap_err("+a(0)+","+s+")";
        if(name=="delete"&&count==2&&is_map(ty(arg(0)))){const auto parts=map_parts(ty(arg(0)));auto k=fresh("k");
            return "({auto& "+k+"m="+lvalue(arg(0))+";"+ct(parts[0],n)+" "+k+"="+a(1)+";hn::erase("+k+"m,"+k+","+s+");})";}
        if(name=="$std$list$set"&&count==3){const auto& parts=type_arguments(ty(arg(0)),"List");auto l=fresh("l");
            return "({auto "+l+"="+a(0)+";auto "+l+"i="+a(1)+";"+ct(parts[0],n)+" "+l+"v="+coerce(a(2),ty(arg(2)),parts[0],n)+";hn::list_set("+l+","+l+"i,std::move("+l+"v),"+s+");})";}
        if(name=="$std$list$pop"&&count==1){auto l=fresh("l");return "({auto "+l+"="+a(0)+";hn::list_pop("+l+","+s+");})";}
        if(name.starts_with("$std$list$")||name.starts_with("$std$maps$")){
            const auto f=name.substr(name.find('$',5)+1);const bool list=name.starts_with("$std$list$");
            const auto element=list?type_arguments(ty(arg(0)),"List"):std::vector<std::string>{};
            const auto parts=list?std::vector<std::string>{}:map_parts(ty(arg(0)));
            auto c=fresh("c");std::string h="({auto "+c+"="+a(0)+";";
            auto val=[&](std::size_t i,const std::string& t){auto v=fresh("v");h+=ct(t,n)+" "+v+"="+coerce(a(i),ty(arg(i)),t,n)+";";return v;};
            if(list&&element.size()==1){
                const auto& e=element[0];
                if((f=="index_of"||f=="contains")&&count==2){auto v=val(1,e);return h+"hn::list_"+f+"("+c+","+v+");})";}
                if(f=="insert"&&count==3){auto i=val(1,"int");auto v=val(2,e);return h+"hn::list_insert("+c+","+i+",std::move("+v+"),"+s+");})";}
                if(f=="remove_at"&&count==2){auto i=val(1,"int");return h+"hn::list_remove_at("+c+","+i+","+s+");})";}
                if(f=="extend"&&count==2){auto v=val(1,"[]"+e);return h+"hn::list_extend("+c+","+v+","+s+");})";}
                if(f=="reserve"&&count==2){auto i=val(1,"int");return h+"hn::list_reserve("+c+","+i+","+s+");})";}
                if(f=="reverse"&&count==1)return h+"hn::list_reverse("+c+","+s+");hn::Unit{};})";
                if(f=="dedup"&&count==1)return h+"hn::list_dedup("+c+","+s+");})";
                if(f=="sort"&&count==1&&(e=="int"||e=="float"||e=="string"))
                    return h+"hn::list_sort("+c+","+s+");hn::Unit{};})";
            }else if(parts.size()==2){
                if(f=="keys"&&count==1)return h+"hn::map_keys("+c+");})";
                if(f=="values"&&count==1)return h+"hn::map_values("+c+");})";
                if(f=="entries"&&count==1)return h+"hn::map_entries("+c+","+s+");})";
                if(f=="merge"&&count==2){const auto src=val(1,ty(arg(0)));return h+"hn::map_merge("+c+","+src+","+s+");})";}
                if(f=="get_or"&&count==3){auto k=val(1,parts[0]);auto d=val(2,parts[1]);return h+"hn::map_get_or("+c+","+k+",std::move("+d+"));})";}
                if(f=="pop"&&count==2){auto k=val(1,parts[0]);return h+"hn::map_pop("+c+","+k+","+s+");})";}
            }
        }
        if(name=="$std$list$clear"&&count==1){auto l=fresh("l");return "({auto "+l+"="+a(0)+";hn::list_clear("+l+","+s+");hn::Unit{};})";}
        if(name=="$std$list$from_slice"&&count==1)return "hn::list_from_slice("+a(0)+")";
        if(name=="$std$list$len"&&count==1)return "hn::list_len("+a(0)+")";
        if(name=="$std$list$snapshot"&&count==1)return "hn::list_snapshot("+a(0)+")";
        if(name=="$std$list$get"&&count==2){auto l=fresh("l"),i=fresh("i");return "({auto "+l+"="+a(0)+";auto "+i+"="+a(1)+";hn::list_get("+l+","+i+","+s+");})";}
        if(name=="$std$list$append"&&count==2){const auto& parts=type_arguments(ty(arg(0)),"List");auto l=fresh("l");
            return "({auto "+l+"="+a(0)+";"+ct(parts[0],n)+" "+l+"v="+a(1)+";hn::list_append("+l+",std::move("+l+"v),"+s+");})";}
        if((name=="$std$strings$starts_with"||name=="$std$strings$ends_with")&&count==2){auto x=fresh("s"),p=fresh("p");
            return "({auto&& "+x+"="+a(0)+";auto&& "+p+"="+a(1)+";hn::"+name.substr(13)+"("+x+","+p+");})";}
        if(name=="$std$strings$split"&&count==2){auto x=fresh("s"),p=fresh("p");return "({auto&& "+x+"="+a(0)+";auto&& "+p+"="+a(1)+";hn::split("+x+","+p+","+s+");})";}
        if(name.starts_with("$std$strings$")){
            const auto operation=name.substr(13);
            if(operation=="trim"||operation=="lower_ascii"||operation=="upper_ascii"){
                const auto function=operation=="trim"?"text_trim":operation=="lower_ascii"?"text_ascii_case<false>":"text_ascii_case<true>";
                return "hn::"+std::string(function)+"("+a(0)+","+s+")";
            }
            if(operation=="count"||operation=="last_index"||operation=="template"){
                const auto left=fresh("text"),right=fresh("arg");
                return "({auto "+left+"="+a(0)+";auto "+right+"="+a(1)+";hn::text_"+operation+"("+left+","+right+","+s+");})";
            }
        }
        if(!given&&(name.starts_with("$std$deque$")||name.starts_with("$std$heap$")||name.starts_with("$std$sets$")||name.starts_with("$std$random$"))){
            if(auto typed=collection_call(n,name,s);!typed.empty())return typed;
        }
        if(has_typed_callback(name)&&!given){
            auto implementation=name.substr(5);
            implementation[implementation.find('$')]='_';
            if(implementation=="maps_update")implementation="map_update";
            return typed_callback(n,implementation,s);
        }
        // Any other builtin runs the VM's implementation on boxed copies: only for
        // value-like types, and never for functions that call back or may suspend.
        const auto& info=builtin_info(name);
        if((info.standard&&info.standard->callback)||info.always_suspends||info.core==CoreBuiltin::task_start||info.core==CoreBuiltin::parallel_map||info.core==CoreBuiltin::task_await||info.core==CoreBuiltin::task_group_begin||info.core==CoreBuiltin::task_group_end)no(n,"builtin `"+name+"`");
        const auto& out=ty(n);
        auto unboxable=[&](const std::string& t){if(value_like(t))return true;if(t.starts_with("List<")){const auto& a=type_arguments(t,"List");return a.size()==1&&value_like(a[0]);}return false;};
        if(!out.empty()&&!unboxable(out)&&name!="$std$os$parse_args")no(n,"builtin `"+name+"` returning `"+out+"`");
        std::string list,flags;
        for(std::size_t i=0;i<count;++i){
            const auto& at=ty(arg(i));
            if(!boxable(at))no(arg(i),"builtin `"+name+"` with a `"+at+"` argument");
            // A boxed native container is a copy: a library call must not be the one to mutate it.
            if(i==0&&info.standard&&info.standard->mutates_first&&!opaque_tag(at)&&!value_like(at))no(arg(i),"builtin `"+name+"` mutating a `"+at+"` argument");
            if(i){list+=",";flags+=",";}list+="hn::to_value("+a(i)+")";flags+=contextual_literal(arg(i))?"true":"false";}
        const auto site=fresh("site");
        const std::string boxed="hn::call_bridge("+site+","+s+",{"+list+"})";
        const std::string head="({static hn::Bridge "+site+"(\""+name+"\",{"+flags+"},\""+out+"\");";
        if(statement||out=="void"||out.empty())return head+boxed+";hn::Unit{};})";
        return head+"hn::from_value<"+ct(out,n)+">("+boxed+");})";
    }
    std::string collection_call(const Node& n,const std::string& name,const std::string& site){
        const auto split=name.find('$',5);
        const auto module=name.substr(5,split-5),op=name.substr(split+1);
        static const std::map<std::string,std::string> deque{
            {"from_slice","deque_from_slice"},{"len","deque_len"},{"clear","deque_clear"},{"get","deque_get"},
            {"snapshot","deque_snapshot"},{"push_back","deque_push<true>"},{"push_front","deque_push<false>"},
            {"back","deque_end<true,false>"},{"front","deque_end<false,false>"},
            {"pop_back","deque_end<true,true>"},{"pop_front","deque_end<false,true>"}
        };
        static const std::map<std::string,std::string> heap{
            {"push","heap_push"},{"pop","heap_end<true>"},{"peek","heap_end<false>"},
            {"heapify","heap_heapify"},{"smallest","heap_top<false>"},{"largest","heap_top<true>"}
        };
        static const std::map<std::string,std::string> sets{
            {"from_slice","set_from_slice"},{"is_subset","set_is_subset"},{"is_disjoint","set_is_disjoint"},
            {"union","set_combine<hn::SetOperation::Union>"},{"intersection","set_combine<hn::SetOperation::Intersection>"},
            {"difference","set_combine<hn::SetOperation::Difference>"},
            {"symmetric_difference","set_combine<hn::SetOperation::SymmetricDifference>"}
        };
        static const std::map<std::string,std::string> random{
            {"new","random_new"},{"int","random_int"},{"float","random_float"},{"bool","random_bool"},
            {"shuffle","random_shuffle"},{"choice","random_choice"},{"sample","random_sample"}
        };
        const auto& registry=module=="deque"?deque:module=="heap"?heap:module=="sets"?sets:random;
        const auto found=registry.find(op);
        if(found==registry.end())return {};
        const bool takes_item=op=="push"||op=="push_front"||op=="push_back";
        const bool simple_call=op=="len"||op=="clear"||op=="snapshot"||op=="new";
        std::string code="({",arguments;
        for(std::size_t i=1;i<n.children.size();++i){
            const auto& arg=*n.children[i];
            const auto var=fresh("arg");
            auto type=ty(arg);
            if(i==2&&takes_item)type=type_arguments(ty(*n.children[1]),module=="heap"?"List":"Deque").at(0);
            code+=evaluated(arg,type,var,false);
            if(i>1)arguments+=",";
            arguments+=var;
        }
        if(!simple_call)arguments+=","+site;
        return code+"hn::"+found->second+"("+arguments+");})";
    }
    std::string typed_callback(const Node& n,const std::string& name,const std::string& site){
        const auto fn_type=ty(*n.children.back());
        if(!fn_type.starts_with("fn:"))no(n,"callback without a known function type");
        const auto& fn=*m.functions.at(fn_type.substr(3));
        const auto sig=signature(fn);
        if(!sig.self.empty()||sig.any_mut||sig.result.empty())no(n,"native algorithm callback signature");
        const bool nested=m.nested.contains(&fn);
        if(!nested)need(&fn);
        std::string code="({",arguments;
        for(std::size_t i=1;i+1<n.children.size();++i){
            const auto temp=fresh("arg");
            const auto& arg=*n.children[i];
            auto type=ty(arg);
            if(((name=="alg_fold"||name=="heap_push_by_key")&&i==2)||(name=="map_update"&&i==3))
                type=sig.types.front();
            code+=evaluated(arg,type,temp,false);
            arguments+=temp+",";
        }
        auto target=fresh("callback");
        code+="auto "+target+"="+expr(*n.children.back())+";";
        const auto depth=fresh("depth");
        return code+"hn::"+name+"("+arguments+"[&](auto&&... args){const auto "+depth+"=hn::enter<NATIVE_COUNT_STEPS>("+site+");auto result="+
               mangle("hf_",sig.name)+"("+(nested?target+",":"")+"std::forward<decltype(args)>(args)...);hn::leave("+depth+");return result;},"+site+");})";
    }
    // Defer: arguments evaluated now (into the guard), the call run at block exit.
    const std::vector<std::string>* given{};
    void defer(const Node& n){
        const auto& c=*n.children[0];if(c.kind!=N::Call)no(n,"defer of a non-call");
        const auto& callee=*c.children[0];if(callee.kind!=N::Name)no(n,"deferred method or expression call");
        std::vector<std::string> args;std::string capture,callee_snapshot;
        const auto& kind=ty(callee);
        const auto* function=kind.starts_with("fn:")?m.functions.at(kind.substr(3)):nullptr;
        if(kind=="@function"||kind=="@dynamic"||kind.starts_with("callable:")||(function&&(m.nested.contains(function)||!signature(*function).self.empty()))){
            callee_snapshot=fresh("saved_callee");
            line("auto "+callee_snapshot+"="+expr(callee)+";");
        }
        for(std::size_t i=1;i<c.children.size();++i){
            const auto& arg=*c.children[i];
            auto name=fresh("deferred_arg");
            line(evaluated(arg,ty(arg),name,true));
            args.push_back(name);
        }
        for(auto& arg:args){
            const auto saved=fresh("deferred_value");
            capture+=(capture.empty()?"":",")+saved+"="+arg;
            arg=saved;
        }
        if(ty(callee).starts_with("fn:")){
            const auto& f=*m.functions.at(ty(callee).substr(3));const auto sig=signature(f);
            for(bool mut:sig.mut)if(mut)no(n,"deferred call with a mut argument");
        }
        given=&args;
        deferred_callee=callee_snapshot.empty()?nullptr:&callee_snapshot;
        const auto text=call(c,true);
        deferred_callee=nullptr;
        given=nullptr;
        auto g=fresh("defer");line("hn::Deferred "+g+"([= "+(capture.empty()?"":","+capture)+"]()mutable{(void)"+text+";});");
    }
    void line(const std::string& text){body<<text<<'\n';}
    void block(const Node& n){scopes.emplace_back();line("{");for(const auto& c:n.children)statement(*c);line("}");scopes.pop_back();}
    void declare(const std::string& name,const std::string& type,const std::string& init,const Node& at){
        if(scopes.back().contains(name))no(at,"redeclaration");
        auto cpp=fresh(name);line(ct(type,at)+" "+cpp+"="+init+";");scopes.back()[name]=Local{cpp,type};
    }
    // Survey mode (HUA_NATIVE_SURVEY): instead of rejecting at the first construct outside
    // the subset, count every one, per statement, across all reachable functions.
    bool survey{};std::map<std::string,unsigned> missing;
    void statement(const Node& n){
        if(!survey){statement_checked(n);return;}
        const auto mark=body.tellp();
        try{statement_checked(n);}catch(const NativeUnsupported& u){++missing[u.what];body.seekp(mark);}
    }
    void statement_checked(const Node& n){
        switch(n.kind){
        case N::Block:block(n);break;
        case N::Let:case N::Var:{
            const auto& init=*n.children.back();
            const auto& t=ty(n);
            if(const auto plan=stack_closure_plans.find(&n);plan!=stack_closure_plans.end()){
                for(const auto& [call,fn]:plan->second.calls){
                    const auto slot=fresh("closure_slot");
                    const auto& closure=*enabled_factories.at(fn);
                    line("std::optional<hn::"+mangle("FE_",declared_name(closure))+"> "+slot+";");
                    stack_call_slots.emplace(call,slot);
                }
                if(plan->second.array){
                    const auto local=fresh(n.text);
                    std::string items;
                    for(const auto& item:init.children){
                        if(!items.empty())items+=",";
                        items+=expr(*item);
                    }
                    line("const std::array<"+ct(element_type(t),n)+","+std::to_string(init.children.size())+"> "+local+"{"+items+"};");
                    scopes.back()[n.text]=Local{local,t,true};
                    break;
                }
            }
            declare(n.text,t,binding_value(n,t,coerce(expr(init),ty(init),t,n)),n);
            if(n.kind==N::Let&&function_value_type(t))scopes.back().at(n.text).immutable_callable=true;
            break;}

        case N::MultiBinding:case N::MultiAssignment:multiple(n);break;
        case N::Function:nested_function(n);break;
        case N::ExpressionStatement:{
            const auto& e=*n.children[0];
            if(e.kind==N::Assignment||e.kind==N::Update){assignment(e);break;}
            if(e.kind==N::Call){line(call(e,true)+";");break;}
            line("(void)("+expr(e)+");");break;}
        case N::If:{
            line("if("+condition(*n.children[0])+")");statement_block(*n.children[1]);
            if(n.children.size()==3){line("else");statement_block(*n.children[2]);}break;}
        case N::While:{line("while(true){if constexpr(NATIVE_COUNT_STEPS)hn::step();if(!("+condition(*n.children[0])+"))break;");statement_block(*n.children[1]);line("}");break;}
        case N::For:loop(n);break;
        case N::Match:match(n);break;
        case N::Defer:defer(n);break;
        case N::Break:line("break;");break;
        case N::Continue:line("continue;");break;
        case N::Return:{
            if(n.children.size()>1){
                if(result_type=="@dynamic"){
                    std::vector<std::string> types;
                    std::string shape="multi<";
                    for(const auto& item:n.children){
                        if(!types.empty())shape+=",";
                        types.push_back(ty(*item));
                        shape+=types.back();
                    }
                    line("return "+dynamic_pack(pack(n,types),shape+">",n,true)+";");
                }else line("return "+pack(n,type_arguments(result_type,"multi"))+";");
                break;
            }
            if(n.children.empty()){line(result_type.empty()?"return;":"return {};");break;}
            const auto& e=*n.children[0];
            if(result_type.empty())no(n,"value returned from a void function");
            const auto from=ty(e);
            if(!pending_return.empty()&&return_can_fail(from,result_type)){
                const auto raw=fresh("raw_return");
                line(pending_return+"=["+raw+"="+expr(e)+"](const SourceSpan& native_return_site) mutable { return "+
                    coerce(raw,from,result_type,n)+"; };");
                line("return {};");
            }else if(result_type=="@dynamic"&&from=="@dynamic")
                line("return hn::copy_dynamic("+expr(e)+");");
            else line("return "+coerce(expr(e),from,result_type,n)+";");
            break;}
        default:no(n,std::string("statement ")+std::string(node_name(n.kind)));
        }
    }
    enum class PlaceKind { Local, Field, Sequence, Map };
    struct Place { std::string name, type; PlaceKind kind; };
    Place locate(const Node& n){
        const auto name=fresh("place");
        if(n.kind==N::Name){
            line("auto& "+name+"="+lvalue(n)+";");
            return {name,local_type(n),PlaceKind::Local};
        }
        const auto& base=*n.children[0];
        const auto& type=ty(base);
        if(n.kind==N::Member&&located_records.contains(type)){
            const auto receiver=locate_receiver(base);
            line("auto "+name+"=hn::locate_field("+receiver+".fields,&hn::"+mangle("SB_",type)+"::"+mangle("f_",n.text)+");");
            return {name,ty(n),PlaceKind::Field};
        }
        if(n.kind==N::Index&&(is_sequence(type)||is_map(type))){
            const auto owner=fresh("owner"),index=fresh("index");
            const bool map=is_map(type);
            const auto parts=map?map_parts(type):std::vector<std::string>{"int",element_type(type)};
            const auto storage=is_fixed(type)&&base.kind==N::Member
                ?"hn::sequence_view("+locate_receiver(base)+")":map?expr(base):sequence_expr(base);
            line("auto "+owner+"="+storage+";");
            const auto& key=*n.children[1];
            line(ct(parts[0],key)+" "+index+"="+coerce(expr(key),ty(key),parts[0],key)+";");
            line("auto "+name+"=hn::"+(map?"locate_map":"locate_sequence")+"(std::move("+owner+"),std::move("+index+"),"+span(n.span)+");");
            return {name,parts[1],map?PlaceKind::Map:PlaceKind::Sequence};
        }
        no(n,"assignment target "+std::string(node_name(n.kind)));
    }
    template<class F> std::string location_expression(F finish){
        std::ostringstream saved;
        body.swap(saved);
        try{
            const auto tail=finish();
            const auto code=body.str();
            body.swap(saved);
            return "({"+code+tail+";})";
        }catch(...){body.swap(saved);throw;}
    }
    bool possible_place(const Node& n){
        return n.kind==N::Name||n.kind==N::Index||(n.kind==N::Member&&possible_place(*n.children[0]));
    }
    std::string dynamic_argument(const Node& arg){
        const auto type=ty(arg);
        const auto writable=m.writable.find(&arg);
        const bool can_write=writable!=m.writable.end()&&writable->second;
        if(is_fixed(type)&&can_write){
            if(arg.kind==N::Member)
                return location_expression([&]{
                    const auto place=locate_receiver(arg);
                    return dynamic_pack(ct(type,arg)+"(hn::sequence_view("+place+"))",type,arg,true);
                });
            return dynamic_pack(ct(type,arg)+"("+sequence_expr(arg)+")",type,arg,true);
        }
        if(is_struct(type)&&located_records.contains(type)&&can_write&&possible_place(arg)){
            return location_expression([&]{
                const auto receiver=locate_receiver(arg);
                return dynamic_pack("hn::dynamic_record_alias("+receiver+")",type,arg,true);
            });
        }
        return dynamic_pack(expr(arg),type,arg,can_write);
    }
    std::string locate_receiver(const Node& n){
        if(n.kind==N::Name)return lvalue(n);
        if(n.kind==N::Member)return field_access(locate_receiver(*n.children[0]),ty(*n.children[0]),n.text);
        if(n.kind==N::Index){
            const auto place=locate(n);
            if(place.kind==PlaceKind::Map)return "hn::map_record("+place.name+".map,"+place.name+".key,"+span(n.span)+")";
            return place.name+".read()";
        }
        no(n,"field receiver is not an assignable place");
    }
    void write_place(const Place& place,const std::string& value,const std::string& site){
        if(place.kind==PlaceKind::Local)line(place.name+"="+value+";");
        else line(place.name+".write("+value+","+site+");");
    }
    void multiple(const Node& n){
        const auto& targets=n.children[0]->children;
        const auto& rhs=*n.children[1];
        const bool dynamic=ty(rhs)=="@dynamic";
        const auto types=dynamic?std::vector<std::string>(targets.size(),dynamic_value_type):type_arguments(ty(rhs),"multi");
        if(types.size()!=targets.size())no(n,"multiple value arity");
        const bool bind=n.kind==N::MultiBinding;
        if(!bind)line("{");
        std::vector<std::optional<Place>> locations(targets.size());
        std::vector<std::string> values(targets.size());
        std::string maps;
        if(!bind)for(std::size_t i=0;i<targets.size();++i){
            const auto& target=*targets[i];
            if(target.kind==N::Name&&target.text=="_")continue;
            locations[i]=locate(target);
            if(locations[i]->kind==PlaceKind::Map)maps+=","+locations[i]->name;
        }
        const auto tuple=fresh("values");
        line("auto "+tuple+"="+expr(rhs)+";");
        const auto site=span(n.span);
        const auto items=dynamic?fresh("items"):std::string{};
        if(dynamic)line("const auto& "+items+"=hn::unpack_dynamic("+tuple+","+std::to_string(targets.size())+","+site+");");
        std::vector<std::string> binding_types(targets.size());
        for(std::size_t i=0;i<targets.size();++i){
            const auto& target=*targets[i];
            if((bind||target.kind==N::Name)&&target.text=="_")continue;
            auto type=bind?ty(target):locations[i]->type;
            if(dynamic&&bind&&type.empty())type=dynamic_value_type;
            binding_types[i]=type;
            const auto& source=rhs.kind==N::Pack?*rhs.children[i]:rhs;
            values[i]=fresh("value");
            const auto item=dynamic?items+"["+std::to_string(i)+"]":"std::get<"+std::to_string(i)+">(std::move("+tuple+"))";
            std::string conversion;
            if(type=="@dynamic"&&dynamic){
                if(!bind&&locations[i]->kind==PlaceKind::Local)
                    conversion="hn::prepare_dynamic("+locations[i]->name+","+item+","+site+")";
                else conversion="hn::copy_dynamic("+item+")";
            }else if(dynamic)conversion=dynamic_unpack(item,type,n,site);
            else if(hua::numeric_spec(types[i])&&hua::numeric_spec(type)&&types[i]!=type)
                conversion=numeric_conversion(item,types[i],type,n,source);
            else conversion=coerce(item,types[i],type,source,true);
            line(ct(type,target)+" "+values[i]+"="+conversion+";");
            if(!bind&&type=="@function"){
                const auto& place=*locations[i];
                if(place.kind==PlaceKind::Local)
                    line("hn::check_function_type("+values[i]+","+place.name+".type,"+span(n.span)+");");
                else if(place.kind==PlaceKind::Sequence)
                    line("hn::check_function_type("+values[i]+","+place.name+".read().type,"+span(n.span)+");");
            }
        }
        if(!maps.empty())line("hn::prepare_map_writes("+site+maps+");");
        for(std::size_t i=0;i<targets.size();++i){
            const auto& target=*targets[i];
            if(values[i].empty())continue;
            if(bind)declare(target.text,binding_types[i],"std::move("+values[i]+")",target);
            else write_place(*locations[i],"std::move("+values[i]+")",site);
        }
        if(!bind)line("}");
    }
    void match(const Node& n){
        const auto& t=ty(*n.children[0]);const bool enumeration=is_enum(t);
        auto v=fresh("match");scopes.emplace_back();line("{auto&& "+v+"="+expr(*n.children[0])+";");
        bool first=true,fallback=false;
        for(std::size_t i=1;i<n.children.size();++i){
            const auto& arm=*n.children[i];const auto& pattern=*arm.children[0];
            const bool any=pattern.kind==N::Name&&display_type(pattern.text)=="_";
            if(fallback)break; // arms after `_` are unreachable
            if(any){line(first?"{":"else{");fallback=true;}
            else{
                auto c=m.constants.find(&pattern);if(c==m.constants.end())no(pattern,"match pattern");
                std::string test;
                if(enumeration){auto tag=hua::string_data(&c->second.data);if(!tag)no(pattern,"enum pattern");test=v+".tag=="+std::to_string(tag_of(t,*tag,pattern));}
                else test=binary("==",v,t,scalar_literal(pattern),t,"bool",pattern,false);
                line(std::string(first?"if(":"else if(")+test+"){");
            }
            first=false;scopes.emplace_back();
            if(enumeration&&!any){
                const auto tag=hua::string_data(&m.constants.at(&pattern).data);const auto& fields=enum_info(t).fields.at(*tag);
                const auto& bindings=*arm.children[1];
                for(std::size_t j=0;j<bindings.children.size();++j){const auto& p=*bindings.children[j];if(p.text=="_")continue;
                    if(j>=fields.size())no(p,"pattern arity");const auto pt=ty(p);auto cpp=fresh(p.text);
                    line("const "+ct(pt,p)+" "+cpp+"="+v+"."+mangle("f_",fields[j].first)+";");scopes.back()[p.text]=Local{cpp,pt};}
            }
            block(*arm.children[2]);scopes.pop_back();line("}");
        }
        line("}");scopes.pop_back();
    }
    void statement_block(const Node& n){if(n.kind==N::Block)block(n);else{scopes.emplace_back();line("{");statement(n);line("}");scopes.pop_back();}}
    void assignment(const Node& n){
        const auto& target=*n.children[0];
        const std::string op=n.kind==N::Update?(n.text=="++"?"+=":"-="):n.text;
        const bool compound=op!="=";
        const auto target_type=local_type(target);
        if(target.kind==N::Name&&target_type=="string"&&n.kind==N::Assignment){
            const auto& rhs=*n.children[1];
            const Node* suffix=nullptr;
            if(op=="+="&&simple(rhs)&&ty(rhs)=="string")suffix=&rhs;
            if(op=="="&&rhs.kind==N::Binary&&rhs.text=="+"&&rhs.children[0]->kind==N::Name
                &&rhs.children[0]->text==target.text&&simple(*rhs.children[1])&&ty(*rhs.children[1])=="string")
                suffix=rhs.children[1].get();
            if(suffix){
                line("hn::append_text("+lvalue(target)+","+expr(*suffix)+","+span(op=="="?rhs.span:n.span)+");");
                return;
            }
        }
        std::string value,value_type;
        if(n.kind==N::Update){value="hn::I(1LL)";value_type="int";}else{value=expr(*n.children[1]);value_type=ty(*n.children[1]);}
        const bool literal_right=n.kind==N::Update||contextual_literal(*n.children[1]);
        if(target.kind==N::Index&&is_map(ty(*target.children[0]))){
            const auto parts=map_parts(ty(*target.children[0]));auto mv=fresh("m"),k=fresh("k"),v=fresh("v");
            line("{auto "+mv+"="+expr(*target.children[0])+";"+ct(parts[0],n)+" "+k+"="+expr(*target.children[1])+";");
            if(!compound)line(ct(parts[1],n)+" "+v+"="+coerce(value,value_type,parts[1],n)+";hn::put("+mv+",std::move("+k+"),std::move("+v+"),"+span(n.span)+");}");
            else {
                line("hn::require_writable("+mv+","+span(n.span)+",\"cannot mutate a read-only map\");");
                line("auto "+v+"r="+value+";auto& "+v+"=hn::slot("+mv+","+k+","+span(n.span)+");");
                line(v+"="+binary(op.substr(0,op.size()-1),v,parts[1],v+"r",value_type,parts[1],n,literal_right)+";}");
            }
            return;
        }
        auto t=fresh("t");
        if(stable_local_fields.contains(&n)&&n.kind==N::Assignment&&may_rebind(*n.children[1])){
            line("{auto "+t+"="+value+";");
            const auto destination=lvalue(target);
            const auto replacement=compound
                ?binary(op.substr(0,op.size()-1),destination,target_type,t,value_type,target_type,n,literal_right)
                :coerce(t,value_type,target_type,n,true);
            line(destination+"="+replacement+";}");
            return;
        }
        if(target.kind==N::Member&&((n.kind==N::Assignment&&may_rebind(*n.children[1]))||!place_expression(target))){
            line("{");
            const auto place=locate(target);
            if(compound){
                line("auto "+t+"="+value+";");
                value=binary(op.substr(0,op.size()-1),place.name+".read()",target_type,t,value_type,target_type,n,literal_right);
            }else value=coerce(value,value_type,target_type,n);
            write_place(place,value,span(n.span));
            line("}");
            return;
        }
        if(target.kind==N::Index&&is_sequence(ty(*target.children[0]))){
            const auto storage=fresh("sequence"),index=fresh("index");
            line("{auto "+storage+"="+sequence_expr(*target.children[0])+";auto "+index+"="+expr(*target.children[1])+";");
            line("decltype(auto) "+t+"=hn::slot("+storage+","+index+","+span(target.span)+");");
            if(!compound&&target_type=="@function")
                line("hn::assign_function("+t+","+coerce(value,value_type,target_type,n)+","+span(n.span)+");}");
            else if(!compound)line(t+"="+coerce(value,value_type,target_type,n)+";}");
            else line("auto "+t+"r="+value+";"+t+"="+binary(op.substr(0,op.size()-1),t,target_type,t+"r",value_type,target_type,n,literal_right)+";}");
            return;
        }
        if(target.kind==N::Name&&!compound){
            const auto replacement=coerce(value,value_type,target_type,n);
            if(target_type=="@function"||target_type.starts_with("callable:"))
                line("hn::assign_function("+lvalue(target)+","+replacement+","+span(n.span)+");");
            else if(target_type=="@dynamic")
                line("hn::assign_dynamic("+lvalue(target)+","+replacement+","+span(n.span)+");");
            else line(lvalue(target)+"="+replacement+";");
            return;
        }
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
            line("while(!"+done+"&&("+st+">0?("+c+(inclusive?"<=":"<")+e+"):("+c+(inclusive?">=":">")+e+"))){if constexpr(NATIVE_COUNT_STEPS)hn::step();const hn::I "+name+"="+c+";");
            line(std::string("if(")+(inclusive?"("+c+"=="+e+")||":"")+"("+st+">0&&"+c+">std::numeric_limits<hn::I>::max()-"+st+")||("+st+"<0&&"+c+"<std::numeric_limits<hn::I>::min()-"+st+"))"+done+"=true;else "+c+"+="+st+";");
            scopes.back()[n.children[0]->text]=Local{name,"int"};
        }else{
            const auto& t=ty(iterable);
            if(is_sequence(t)){
                const auto element=element_type(t);auto sl=fresh("sl"),ix=fresh("ix");
                line("auto "+sl+"="+sequence_expr(iterable)+";for(hn::I "+ix+"=0;"+ix+"<"+sl+".length;++"+ix+"){if constexpr(NATIVE_COUNT_STEPS)hn::step();");
                if(count==2){auto i=fresh(n.children[0]->text);line("const hn::I "+i+"="+ix+";");scopes.back()[n.children[0]->text]=Local{i,"int"};}
                auto v=fresh(n.children[count-1]->text);line("const "+ct(element,n)+" "+v+"=(*"+sl+".storage)[static_cast<std::size_t>("+sl+".start+"+ix+")];");
                scopes.back()[n.children[count-1]->text]=Local{v,element};
            }else if(is_map(t)){
                const auto parts=map_parts(t);auto mp=fresh("mp"),snap=fresh("snap"),kv=fresh("kv");
                line("auto "+mp+"="+expr(iterable)+";std::vector<std::pair<"+ct(parts[0],n)+","+ct(parts[1],n)+">> "+snap+"("+mp+".data->begin(),"+mp+".data->end());for(const auto& "+kv+":"+snap+"){if constexpr(NATIVE_COUNT_STEPS)hn::step();");
                auto k=fresh(n.children[0]->text);line("const "+ct(parts[0],n)+" "+k+"="+kv+".first;");scopes.back()[n.children[0]->text]=Local{k,parts[0]};
                if(count==2){auto v=fresh(n.children[1]->text);line("const "+ct(parts[1],n)+" "+v+"="+kv+".second;");scopes.back()[n.children[1]->text]=Local{v,parts[1]};}
            }else no(n,"loop over `"+t+"`");
        }
        for(const auto& c:code.children)statement(*c);
        line("}}");scopes.pop_back();
    }
    void nested_function(const Node& fn){
        const auto name=declared_name(fn);
        const auto env=mangle("FE_",name);
        const auto type="fn:"+name;
        const auto signature_type=ct(type,fn);
        const auto sig=signature(fn);

        auto outer=std::move(scopes);
        ClosureFrame frame{closure_frame,outer,{}};
        closure_frame=&frame;
        auto saved_result=std::exchange(result_type,sig.result);
        auto saved_pending=std::exchange(pending_return,std::string{});
        std::ostringstream saved_body;
        body.swap(saved_body);
        scopes.clear();
        scopes.emplace_back();
        scopes.back()[name]=Local{"native_self",type};
        std::string head=(sig.result.empty()?"void":ct(sig.result,fn))+" "+mangle("hf_",name)+"(const "+signature_type+"& native_self";
        for(std::size_t i=0;i<sig.names.size();++i){
            auto cpp=fresh(sig.names[i]);
            head+=","+parameter_type(sig,i,fn)+" "+cpp;
            scopes.back()[sig.names[i]]={cpp,sig.types[i]};
        }
        head+=")";
        protos<<head<<";\n";
        try {
            emit_function_body(fn);
        } catch (...) {
            body.swap(saved_body);
            result_type=std::move(saved_result);
            pending_return=std::move(saved_pending);
            scopes=std::move(outer);
            closure_frame=frame.parent;
            throw;
        }
        // VM closures retain all visible locals. Materialize unused bindings which
        // can carry a File so clone observes the same live resource graph.
        std::set<std::string> visible;
        for(auto* active=&frame;active;active=active->parent){
            for(auto scope=active->outer.rbegin();scope!=active->outer.rend();++scope){
                for(const auto& [key,local]:*scope)
                    if(visible.insert(key).second&&might_hold_file(local.type))resolve_capture(frame,key,fn);
            }
        }
        auto statements=body.str();
        body.swap(saved_body);
        result_type=std::move(saved_result);
        pending_return=std::move(saved_pending);
        scopes=std::move(outer);
        closure_frame=frame.parent;

        std::string init=signature_type+"{}";
        closure_code<<head<<"{\n";
        if(!frame.fields.empty()){
            std::string fields,values,edges,resources;
            for(const auto& [key,capture]:frame.fields){
                fields+="    "+ct(capture.source.type,fn)+" "+capture.field+";\n";
                if(!values.empty())values+=",";
                values+="hn::capture_value("+capture.source.cpp+")";
                if(needs_tracing(capture.source.type))
                    edges+="    trace_native(v."+capture.field+",visit);\n";
                if(might_hold_file(capture.source.type))
                    resources+="    if(file_in(v."+capture.field+",scan,depth+1))return true;\n";
            }
            if(!resources.empty()){
                resource_functions.insert(name);
                escaping_functions.insert(name);
            }
            closure_layouts<<"struct "<<env<<"{\n"<<fields;
            if(!edges.empty()){
                closure_layouts<<"    void clear(){*this="<<env<<"{};}\n"
                    <<"    friend void heap_edges(const "<<env<<"& v,const hua::HeapVisitor& visit){\n"<<edges<<"    }\n";
            }
            closure_layouts<<"};\n";
            if(!resources.empty()){
                closure_layouts<<"template<> struct ClosureFile<"<<mangle("FT_",name)<<">{\n"
                    <<"    static bool scan(const void* ptr,FileScan& scan,unsigned depth){\n"
                    <<"    const auto& v=*static_cast<const "<<env<<"*>(ptr);\n"
                    <<resources<<"    return false;\n    }\n};\n";
            }
            closure_code<<"const auto& native_env=*static_cast<const "<<env<<"*>(native_self.data);\n";
            if(factory_closures.contains(&fn))
                init="hn::make_function_in<"+signature_type+">(native_factory_slot,"+env+"{"+values+"})";
            else if(escaping_functions.contains(name))init="hn::make_function<"+signature_type+">("+env+"{"+values+"})";
            else {
                const auto storage=fresh("environment");
                line("const "+env+" "+storage+"{"+values+"};");
                init=signature_type+"{&"+storage+",{}}";
            }
        }
        if(frame.fields.empty()&&factory_closures.contains(&fn))
            closure_layouts<<"struct "<<env<<" {};\n";
        closure_code<<statements<<"}\n";
        declare(name,type,init,fn);
        scopes.back().at(name).immutable_callable=true;
    }
    void function(const Node& fn){
        if(survey){try{(void)signature(fn);for(const auto& c:fn.children)if(c->kind==N::Parameter&&!c->children.empty())(void)ct(type_name(*c->children[0]),*c);}catch(const NativeUnsupported& u){++missing["signature: "+u.what];survey_functions.push_back(&fn);return;}}
        if(survey){try{const auto probe=signature(fn);if(!probe.result.empty())(void)ct(probe.result,fn);}catch(const NativeUnsupported& u){++missing["signature: "+u.what];return;}}
        const auto sig=signature(fn);
        if(is_interface(sig.self)){interface_method(fn,sig);return;}
        result_type=sig.result;
        std::string head=(sig.result.empty()?std::string("void"):ct(sig.result,fn))+" "+mangle("hf_",sig.name)+"(";
        scopes.clear();scopes.emplace_back();
        bool first=true;
        if(!sig.self.empty()){head+=self_type(sig,fn)+" v_self";scopes.back()["self"]=Local{"v_self",sig.self};first=false;}
        for(std::size_t i=0;i<sig.names.size();++i){auto cpp=fresh(sig.names[i]);if(!first)head+=",";first=false;head+=parameter_type(sig,i,fn)+" "+cpp;scopes.back()[sig.names[i]]=Local{cpp,sig.types[i]};}
        if(const auto factory=enabled_factories.find(&fn);factory!=enabled_factories.end()){
            if(!first)head+=",";
            head+="std::optional<hn::"+mangle("FE_",declared_name(*factory->second))+">* native_factory_slot";
            protos<<head<<"=nullptr);\n";
            head+=")";
        }else {head+=")";protos<<head<<";\n";}
        line(head);
        line("{");
        for(std::size_t i=0;i<sig.names.size();++i){
            const auto& type=sig.types[i];
            if(!sig.mut[i]&&is_fixed(type))line(scopes.back().at(sig.names[i]).cpp+".view.writable=false;");
            if(!sig.mut[i]&&is_interface(type))line(scopes.back().at(sig.names[i]).cpp+".object.writable=false;");
        }
        emit_function_body(fn);
        line("}");
    }
    void emit_function_body(const Node& fn){
        const auto& source=*fn.children.back();
        const auto needs_check=[&](auto&& self,const Node& n)->bool{
            if(n.kind==N::Function)return false;
            if(n.kind==N::Return&&n.children.size()==1){
                const auto type=m.types.find(n.children[0].get());
                return type!=m.types.end()&&return_can_fail(type->second.empty()?dynamic_value_type:type->second,result_type);
            }
            return std::any_of(n.children.begin(),n.children.end(),[&](const auto& c){return self(self,*c);});
        };
        if(!needs_check(needs_check,source)){block(source);return;}
        const auto result=ct(result_type,fn),temporary=fresh("returned");
        pending_return=fresh("return_conversion");
        line("std::function<"+result+"(const SourceSpan&)> "+pending_return+";");
        line("auto "+temporary+"=[&]() -> "+result);
        block(source);
        line("();");
        line("if("+pending_return+")return hn::complete_return_conversion("+pending_return+");");
        line("return "+temporary+";");
        pending_return.clear();
    }
    void interface_method(const Node& fn,const Signature& sig){
        const auto type=ct(sig.self,fn);
        const auto result=sig.result.empty()?"void":ct(sig.result,fn);
        std::string head=result+" "+mangle("hf_",sig.name)+"("+
            (sig.self_mut?type+"&":"const "+type+"&")+" receiver";
        std::string args;
        for(std::size_t i=0;i<sig.types.size();++i){
            const auto var=fresh("arg");
            head+=","+parameter_type(sig,i,fn)+" "+var;
            args+=","+var;
        }
        head+=",const SourceSpan& site)";
        protos<<head<<";\n";
        line(head+"{");
        if(sig.self_mut)line("hn::require_writable(receiver.object,site,\"mutating method requires mutable receiver\");");
        line("switch(receiver.object.tag){");
        for(const auto& type_name:m.relations.at(sig.self)){
            if(!usable_structs.contains(type_name))continue;
            const auto method=type_name+sig.name.substr(sig.self.size());
            const auto target=m.functions.at(method);
            need(target);
            const auto pointer="static_cast<"+ct(type_name,fn)+"*>(receiver.object.data)";
            line("case "+std::to_string(interface_tags.at(type_name))+":return "+mangle("hf_",method)+"(*"+pointer+args+");");
        }
        line("default:hn::fail(site,\"invalid interface receiver\",\"E4003\");}}");
    }
    std::string structs(){
        std::ostringstream o;std::vector<const Node*> order;std::set<std::string> done,active;
        std::function<void(const std::string&)> visit_any;
        std::function<void(const std::string&)> visit=[&](const std::string& name){
            if(done.contains(name)||!is_struct(name))return;if(!active.insert(name).second)return;
            const auto& decl=*m.structures.at(name);
            for(const auto& f:decl.children)if(f->kind==N::Field){auto t=type_name(*f->children[0]);if(t.size()>1&&t.back()=='?')t.pop_back();if(is_struct(t))visit(t);else if(is_enum(t))visit_any(t);}
            done.insert(name);order.push_back(&decl);
        };
        visit_any=[&](const std::string& name){
            if(!is_enum(name)){visit(name);return;}
            if(done.contains(name)||!active.insert(name).second)return;
            for(const auto& [variant,fields]:enum_info(name).fields)for(const auto& [f,t]:fields){if(is_struct(t)||is_enum(t))visit_any(t);}
            done.insert(name);order.push_back(m.structures.at(name));
        };
        std::vector<std::string> names;for(const auto& [name,decl]:m.structures)if(is_struct(name)||is_enum(name))names.push_back(name);
        std::sort(names.begin(),names.end());
        // Only structs whose field types are all in the subset are emitted; using any other
        // struct is rejected where its type is first needed.
        std::set<std::string> usable;
        auto field_types=[&](const std::string& name){std::vector<std::string> out;
            if(is_enum(name)){for(const auto& [variant,fields]:enum_info(name).fields)for(const auto& [f,t]:fields)if(t!="void")out.push_back(t);return out;}
            for(const auto& f:m.structures.at(name)->children)if(f->kind==N::Field)out.push_back(type_name(*f->children[0]));return out;};
        // Iterate to a fixpoint: a struct is usable when every field type is (a field may name another struct).
        for(bool changed=true;changed;){changed=false;
            for(const auto& name:names){if(usable.contains(name))continue;
                try{const auto& decl=*m.structures.at(name);for(const auto& f:decl.children)if(f->kind==N::GenericParameters)throw NativeUnsupported{"generic struct",decl.span};
                    structs_ready=true;usable_structs=usable;usable_structs.insert(name);
                    for(const auto& t:field_types(name))(void)ct(t,decl);
                    usable.insert(name);changed=true;}
                catch(const NativeUnsupported&){}
                structs_ready=false;}}
        for(const auto& name:names){if(usable.contains(name))continue;try{structs_ready=true;usable_structs=usable;for(const auto& t:field_types(name))(void)ct(t,*m.structures.at(name));}catch(const NativeUnsupported& u){if(survey)++missing["struct "+name+": "+u.what];}structs_ready=false;}
        structs_ready=true;usable_structs=usable;
        for(const auto& name:names)if(usable.contains(name)){
            o<<"struct "<<mangle(is_enum(name)?"E_":"S_",name)<<";\n";
            if(located_records.contains(name))o<<"struct "<<mangle("SB_",name)<<";\n";
            if(needs_tracing(name)){
                o<<"template<> inline constexpr bool tracks_native_edges<"<<mangle(is_enum(name)?"E_":"S_",name)<<"> = true;\n";
                if(located_records.contains(name))
                    o<<"template<> inline constexpr bool tracks_native_edges<"<<mangle("SB_",name)<<"> = true;\n";
            }
        }
        for(const auto& name:names)if(usable.contains(name))visit_any(name);
        for(const auto* decl:order){
            const auto name=declared_name(*decl);const auto key=[&]{for(const auto& [k,d]:m.structures)if(d==decl)return k;return name;}();
            if(!usable.contains(key))continue;
            if(is_enum(key)){o<<enum_code(key);continue;}
            const auto cpp=mangle("S_",key);std::string fields,names_list,types_list,boxing,cloning,readonly,edges,resource_fields,body_resources;
            for(const auto& f:decl->children)if(f->kind==N::Field){
                const auto t=type_name(*f->children[0]);const auto fc=mangle("f_",f->text);
                fields+="    "+ct(t,*f)+" "+fc+"{};\n";
                names_list+=(names_list.empty()?"":",")+quoted(f->text);types_list+=(types_list.empty()?"":",")+("{"+quoted(f->text)+","+quoted(t)+"}");
                const auto access=field_access("v",key,f->text);
                boxing+="    d->fields.emplace("+quoted(f->text)+",to_value("+access+"));\n";
                cloning+="    "+field_access("out",key,f->text)+"=clone("+access+");\n";
                readonly+="    "+access+"=capture_value(std::move("+access+"));\n";
                if(needs_tracing(t))edges+="    trace_native(v."+fc+",visit);\n";
                if(might_hold_file(t)){
                    resource_fields+="    if(file_in("+access+",scan,depth+1))return true;\n";
                    body_resources+="    if(file_in(v."+fc+",scan,depth+1))return true;\n";
                }
            }
            if(located_records.contains(key)){
                const auto body=mangle("SB_",key);
                o<<"struct "<<body<<" {\n"<<fields;
                if(!edges.empty())o<<"    void clear(){*this="<<body<<"{};}\n";
                o<<"};\nstruct "<<cpp<<" { RecordStorage<"<<body<<"> fields;";
                if(!edges.empty())o<<" void clear(){fields={};}";
                o<<" };\n";
                if(!body_resources.empty())
                    o<<"inline bool resource_edges(const "<<body<<"& v,FileScan& scan,unsigned depth){\n"<<body_resources<<"    return false;\n}\n";
                if(!edges.empty()){
                    o<<"inline void heap_edges(const "<<body<<"& v,const hua::HeapVisitor& visit){\n"<<edges<<"}\n";
                    o<<"inline void heap_edges(const "<<cpp<<"& v,const hua::HeapVisitor& visit){trace_native(v.fields,visit);}\n";
                }
            }else {
                o<<"struct "<<cpp<<" {\n"<<fields;
                if(!edges.empty())o<<"    void clear(){*this="<<cpp<<"{};}\n";
                o<<"};\n";
                if(!edges.empty())o<<"inline void heap_edges(const "<<cpp<<"& v,const hua::HeapVisitor& visit){\n"<<edges<<"}\n";
            }
            if(!resource_fields.empty())
                o<<"inline bool resource_edges(const "<<cpp<<"& v,FileScan& scan,unsigned depth){\n"<<resource_fields<<"    return false;\n}\n";
            o<<"template<> struct TypeName<"<<cpp<<">{static std::string get(){return "<<quoted(key)<<";}};\n";
            o<<"inline Value to_value(const "<<cpp<<"& v){\n    static const auto layout=std::make_shared<const hua::RecordLayout>(std::vector<std::string>{"<<names_list<<"},"<<quoted(key)<<",std::map<std::string,std::string>{"<<types_list<<"});\n"
             <<"    auto d=hua::managed<hua::StructData>();d->name="<<quoted(key)<<";d->fields=hua::RecordFields(layout);\n"<<boxing<<"    return Value(hua::StructValue{d,true});\n}\n";
            o<<"inline "<<cpp<<" clone(const "<<cpp<<"& v){\n    "<<cpp<<" out{};\n"<<cloning<<"    return out;\n}\n";
            if(capture_safe(key))
                o<<"inline "<<cpp<<" capture_value("<<cpp<<" v){\n"<<readonly<<"    return v;\n}\n";
        }
        usable_structs=std::move(usable);structs_ready=true;
        return o.str();
    }
    std::string enum_code(const std::string& key){
        const auto& info=enum_info(key);const auto cpp=mangle("E_",key);std::ostringstream o;
        std::string fields,boxing,cloning,layout,readonly,edges,resources;
        layout+="        auto l=std::make_shared<hua::EnumLayout>();l->name="+quoted(key)+";\n";
        for(std::size_t i=0;i<info.variants.size();++i){
            const auto& variant=info.variants[i];const auto& vf=info.fields.at(variant);
            layout+="        {auto& v=l->variants["+quoted(variant)+"];v.name="+quoted(variant)+";\n";
            std::string active="        if(v.tag=="+std::to_string(i)+"){d->variant=&layout->variants.at("+quoted(variant)+");d->fields.emplace(std::string(\"$case\"),Value(std::string("+quoted(variant)+")));\n";
            for(std::size_t j=0;j<vf.size();++j){
                const auto& [name,type]=vf[j];
                layout+="         v.fields.emplace("+quoted(name)+","+quoted(type)+");l->fields.emplace("+quoted(name)+",hua::EnumField{"+quoted(variant)+","+quoted(type)+","+std::to_string(j)+"});\n";
                if(type=="void"){active+="            d->fields.emplace("+quoted(name)+",Value{});\n";continue;}
                const auto fc=mangle("f_",name);
                fields+="    "+ct(type,*m.structures.at(key))+" "+fc+"{};\n";
                active+="            d->fields.emplace("+quoted(name)+",to_value(v."+fc+"));\n";
                cloning+="    out."+fc+"=clone(v."+fc+");\n";
                readonly+="    if(v.tag=="+std::to_string(i)+")v."+fc+"=capture_value(std::move(v."+fc+"));\n";
                if(needs_tracing(type))edges+="    if(v.tag=="+std::to_string(i)+")trace_native(v."+fc+",visit);\n";
                if(might_hold_file(type))resources+="    if(v.tag=="+std::to_string(i)+"&&file_in(v."+fc+",scan,depth+1))return true;\n";
            }
            layout+="        }\n";boxing+=active+"        }\n";
        }
        o<<"struct "<<cpp<<" {\n    std::uint32_t tag{};\n"<<fields;
        if(!edges.empty())o<<"    void clear(){*this="<<cpp<<"{};}\n";
        o<<"};\n";
        if(!edges.empty())o<<"inline void heap_edges(const "<<cpp<<"& v,const hua::HeapVisitor& visit){\n"<<edges<<"}\n";
        if(!resources.empty())o<<"inline bool resource_edges(const "<<cpp<<"& v,FileScan& scan,unsigned depth){\n"<<resources<<"    return false;\n}\n";
        o<<"template<> struct TypeName<"<<cpp<<">{static std::string get(){return "<<quoted(key)<<";}};\n";
        o<<"inline Value to_value(const "<<cpp<<"& v){\n    static const std::shared_ptr<const hua::EnumLayout> layout=[]{\n"<<layout<<"        return std::shared_ptr<const hua::EnumLayout>(l);}();\n"
         <<"    auto d=hua::managed<hua::StructData>();d->name="<<quoted(key)<<";d->enumeration=layout;\n"<<boxing<<"    return Value(hua::StructValue{d,true});\n}\n";
        o<<"inline "<<cpp<<" clone(const "<<cpp<<"& v){\n    "<<cpp<<" out{};out.tag=v.tag;\n"<<cloning<<"    return out;\n}\n";
        if(capture_safe(key))
            o<<"inline "<<cpp<<" capture_value("<<cpp<<" v){\n"<<readonly<<"    return v;\n}\n";
        return o.str();
    }
    std::set<std::string> usable_structs;bool structs_ready{};std::vector<const Node*> survey_functions;
    std::string run(){
        collect_inferred_returns();
        collect_closure_owners(tree,nullptr);
        classify_closure_uses(tree,nullptr,nullptr);
        collect_stack_factories();
        if(!eligible_factories.empty())
            for(const auto& [name,fn]:m.functions)
                if(!fn->children.empty()&&fn->children.back()->kind==N::Block)plan_stack_closures(*fn);
        for(const auto& c:tree.children)if(c->kind==N::Const)constants[c->text]=c.get();
        std::set<std::string> concrete;
        for(const auto& [name,types]:m.relations)
            for(const auto& type:types)if(!is_interface(type))concrete.insert(type);
        for(const auto& type:concrete)interface_tags.emplace(type,interface_tags.size()+1);
        for(const auto& child:tree.children)
            if(child->kind==N::Let||child->kind==N::Var)module_bindings.insert(child->text);
        plan_locations(tree);
        const auto struct_code=structs();
        // The module body executes in source order, before an optional entry function.
        std::string global_declarations;
        scopes.clear();scopes.emplace_back();result_type.clear();
        for(const auto& c:tree.children){
            if(c->kind==N::Function||c->kind==N::Struct||c->kind==N::Const||c->kind==N::Import)continue;
            try{
                if(c->kind==N::Let||c->kind==N::Var){
                    const auto& t=ty(*c);const auto cpp=mangle("g_",c->text);const auto& init=*c->children.back();
                    line(cpp+"="+binding_value(*c,t,coerce(expr(init),ty(init),t,*c))+";");
                    global_declarations+="static "+ct(t,*c)+" "+cpp+"{};\n";
                    globals[c->text]=Local{cpp,t};
                }else statement(*c);
            }catch(const NativeUnsupported& u){if(!survey)throw;++missing["global: "+u.what];}
        }
        auto global_init=body.str();
        body.str({});
        body.clear();
        auto main=m.functions.find("main");
        Signature main_sig;
        if(main!=m.functions.end()){
            main_sig=signature(*main->second);
            if(!main_sig.names.empty())no(*main->second,"main with parameters");
            need(main->second);
        }
        while(!queue.empty()){auto fn=queue.front();queue.pop_front();function(*fn);}
        const auto main_site=span(main!=m.functions.end()?main->second->span:tree.span);
        std::ostringstream o;
        o<<"// Generated by `hua native`. Do not edit.\n#include \"hua/native_rt.hpp\"\nnamespace hn {\n";
        o<<"#ifndef HUA_NATIVE_COUNT_STEPS\n#define HUA_NATIVE_COUNT_STEPS 1\n#endif\n"
          <<"static constexpr bool NATIVE_COUNT_STEPS=HUA_NATIVE_COUNT_STEPS!=0;\n";
        o<<"static const hua::SourceSpan SP[]={\n";
        for(const auto& s:spans)o<<"    {"<<quoted(s.file_id)<<","<<s.start_offset<<","<<s.end_offset<<","<<s.line<<","<<s.column<<"},\n";
        o<<"};\n";
        for(std::size_t i=0;i<literals.size();++i)o<<"static StrRep L"<<i<<"(1u<<30,"<<quoted(literals[i])<<");\n";
        std::set<std::string> function_tags;
        for(const auto& [name,decl]:m.functions)function_tags.insert(mangle("FT_",name));
        for(const auto& tag:function_tags)o<<"struct "<<tag<<";\n";
        for(const auto& name:indirect_identities){
            o<<"static constexpr char "<<mangle("FID_",name)<<"[]={";
            for(unsigned char c:"fn:"+name)o<<"static_cast<char>("<<static_cast<unsigned>(c)<<"),";
            o<<"0};\n";
        }
        o<<interface_layouts()<<struct_code<<global_declarations<<closure_layouts.str()<<protos.str()<<closure_code.str()<<body.str();
        o<<"static Value entry(){\n"<<global_init;
        if(main==m.functions.end())o<<"    return Value{};\n";
        else if(main_sig.result.empty())o<<"    hf_main();return Value{};\n";
        else o<<"    return to_value(hf_main());\n";
        o<<"}\n}\nint main(int argc,char** argv){return hn::run_main(argc,argv,hn::"<<main_site<<",&hn::entry);}\n";
        return o.str();
    }
};
}
std::string dump_typed_ast(const Node& tree,const SemanticModel& model){std::ostringstream o;dump(tree,model,o,0);return o.str();}
std::string emit_native(const Node& tree,const SemanticModel& model){
    Emitter e{tree,model};
    if(std::getenv("HUA_NATIVE_SURVEY")){
        e.survey=true;
        // Every function, reachable or not, so one run lists the whole gap.
        try{for(const auto& [name,fn]:model.functions)if(!model.nested.contains(fn))e.need(fn);(void)e.run();}catch(const NativeUnsupported& u){++e.missing["program: "+u.what];}
        std::vector<std::pair<unsigned,std::string>> rows;unsigned total=0;for(const auto& [what,count]:e.missing){rows.emplace_back(count,what);total+=count;}
        std::sort(rows.rbegin(),rows.rend());std::string out="native survey: "+std::to_string(total)+" unsupported statements/signatures";out+=char(10);
        for(const auto& [count,what]:rows){out+=std::to_string(count);out+=char(9);out+=what;out+=char(10);}
        throw NativeUnsupported{out,{}};
    }
    return e.run();
}
}
