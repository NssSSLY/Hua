#include "hua/concurrency_lowering.hpp"
#include "hua/value.hpp"
#include <set>
#include <functional>
namespace hua {namespace {
using N=NodeKind;
NodePtr clone(const Node& n){auto p=std::make_unique<Node>(n.kind,n.span,n.text);for(const auto& c:n.children)p->add(clone(*c));return p;}
NodePtr node(N k,const Node& site,std::string text={}){return std::make_unique<Node>(k,site.span,std::move(text));}
[[noreturn]]void bad(const Node& n,std::string message){throw Diagnostic("E3011",n.span,std::move(message));}
NodePtr call(const Node& site,std::string name){auto p=node(N::Call,site);p->add(node(N::Name,site,"$core$"+name));return p;}
std::string intrinsic(const Node& n){auto name=n.text;if(name.starts_with("$core$"))name=name.substr(6);return name;}
bool kernel(const Node& n,const std::string& index){
    switch(n.kind){
    case N::Integer:case N::Float:case N::Boolean:case N::Name:return true;
    case N::Index:return n.children[0]->kind==N::Name&&n.children[1]->kind==N::Name&&n.children[1]->text==index;
    case N::Unary:return n.text!="&"&&n.text!="await"&&n.text!="spawn"&&kernel(*n.children[0],index);
    case N::Binary:return kernel(*n.children[0],index)&&kernel(*n.children[1],index);
    case N::Call:{if(n.children[0]->kind!=N::Name)return false;auto name=intrinsic(*n.children[0]);if(!numeric_spec(name)&&name!="sqrt"&&name!="abs"&&name!="min"&&name!="max"&&name!="clamp")return false;for(std::size_t i=1;i<n.children.size();++i)if(!kernel(*n.children[i],index))return false;return true;}
    default:return false;
    }
}
struct Lower {
    std::size_t serial{};
    void walk(Node& n){
        if(n.kind==N::Program||n.kind==N::Block){
            std::vector<NodePtr> out;
            for(auto& p:n.children){walk(*p);
                if(p->kind==N::Function&&p->text.find(" async")!=std::string::npos){
                    if(!p->children.empty()&&p->children[0]->kind==N::GenericParameters)bad(*p,"async generics require explicit specialization support in a later revision");
                    if(declared_name(*p).find('.')!=std::string::npos)bad(*p,"async methods are not supported in this revision; use an async function with an explicit readonly receiver parameter");
                    auto body_name="$async$"+std::to_string(serial++)+"$"+declared_name(*p);
                    auto wrapper=node(N::Function,*p,p->text);auto async_at=wrapper->text.find(" async");wrapper->text.erase(async_at,6);
                    std::string result="void";const Node* returns=nullptr;
                    auto launch=call(*p,"task_start");launch->add(node(N::Name,*p,body_name));
                    for(const auto& c:p->children){if(c->kind==N::Parameter){if(c->text.find(" mut")!=std::string::npos||(!c->children.empty()&&c->children[0]->kind==N::MutableType))bad(*c,"async parameters cannot borrow mutable data");wrapper->add(clone(*c));launch->add(node(N::Name,*c,declared_name(*c)));}if(c->kind==N::ReturnTypes){if(c->children.size()!=1)bad(*c,"async requires one return type; return a struct for multiple values");returns=c.get();result=type_name(*c->children[0]);}}
                    if(!returns){bool value_return=false;std::function<void(const Node&)> scan=[&](const Node& c){if(c.kind==N::Function)return;if(c.kind==N::Return&&!c.children.empty())value_return=true;for(const auto& item:c.children)scan(*item);};scan(*p->children.back());if(value_return)bad(*p,"async value functions require an explicit return type");}
                    auto r=node(N::ReturnTypes,*p),t=node(N::GenericType,*p,"Task");t->add(returns?clone(*returns->children[0]):node(N::TypeName,*p,"void"));r->add(std::move(t));wrapper->add(std::move(r));
                    auto b=node(N::Block,*p),ret=node(N::Return,*p);ret->add(std::move(launch));b->add(std::move(ret));wrapper->add(std::move(b));
                    auto flags=p->text.substr(p->text.find(' '));auto at=flags.find(" async");flags.erase(at,6);auto attr=flags.find(" @");if(attr!=std::string::npos)flags.erase(attr);p->text=body_name+flags;
                    out.push_back(std::move(p));out.push_back(std::move(wrapper));
                }else out.push_back(std::move(p));
            }n.children=std::move(out);
            if(n.kind==N::Block&&n.text=="taskgroup"){
                auto name="$taskgroup$"+std::to_string(serial++);auto start=node(N::Let,n,name);start->add(call(n,"task_group_begin"));auto deferred=node(N::Defer,n);auto end=call(n,"task_group_end");end->add(node(N::Name,n,name));deferred->add(std::move(end));n.children.insert(n.children.begin(),std::move(deferred));n.children.insert(n.children.begin(),std::move(start));n.text="";
            }return;
        }
        if(n.kind==N::For&&(n.text=="parallel"||n.text=="simd"||n.text=="parallel simd")){
            if(n.children.size()!=3||n.children[1]->kind!=N::Range)bad(n,"parallel/SIMD requires one index binding and a range");
            auto index=n.children[0]->text;auto& body=*n.children[2];
            if(body.children.size()!=1||body.children[0]->kind!=N::ExpressionStatement||body.children[0]->children[0]->kind!=N::Assignment)bad(body,"parallel/SIMD kernel requires one independent array assignment");
            auto& assignment=*body.children[0]->children[0];auto& target=*assignment.children[0];
            if(assignment.text!="="||target.kind!=N::Index||target.children[0]->kind!=N::Name||target.children[1]->kind!=N::Name||target.children[1]->text!=index||!kernel(*assignment.children[1],index))bad(assignment,"parallel/SIMD requires out[i] = pure numeric expression with only input[i] reads");
            if(n.text=="simd"){auto block=node(N::Block,n),check=node(N::ExpressionStatement,n),validate=call(n,"simd_check");validate->add(clone(*target.children[0]));check->add(std::move(validate));block->add(std::move(check));auto loop=clone(n);loop->text="";block->add(std::move(loop));n=std::move(*block);for(auto& c:n.children)walk(*c);return;}
            auto serial_name=std::to_string(serial++),name="$parallel$"+serial_name,results="$parallel_results$"+serial_name,slot="$parallel_slot$"+serial_name;
            auto block=node(N::Block,n),fn=node(N::Function,n,name),param=node(N::Parameter,n,index);param->add(node(N::TypeName,n,"int"));fn->add(std::move(param));
            auto fnbody=node(N::Block,n),ret=node(N::Return,n);ret->add(clone(*assignment.children[1]));fnbody->add(std::move(ret));fn->add(std::move(fnbody));block->add(std::move(fn));
            std::vector<std::string> bounds;for(std::size_t i=0;i<3;++i){auto key="$parallel_bound$"+serial_name+"$"+std::to_string(i);bounds.push_back(key);auto binding=node(N::Let,n,key);binding->add(i<n.children[1]->children.size()?clone(*n.children[1]->children[i]):node(N::Integer,n,"1"));block->add(std::move(binding));}
            auto launch=call(n,"parallel_map");launch->add(node(N::Name,n,name));for(const auto& key:bounds)launch->add(node(N::Name,n,key));launch->add(clone(*target.children[0]));launch->add(node(N::Boolean,n,n.children[1]->text=="..="?"true":"false"));launch->add(node(N::Boolean,n,contextual_literal(*assignment.children[1])?"true":"false"));
            auto binding=node(N::Let,n,results);binding->add(std::move(launch));block->add(std::move(binding));auto counter=node(N::Var,n,slot);counter->add(node(N::Integer,n,"0"));block->add(std::move(counter));
            auto loop=clone(n);loop->text="";loop->children[1]->children.clear();for(const auto& key:bounds)loop->children[1]->add(node(N::Name,n,key));auto& value=*loop->children[2]->children[0]->children[0];auto read=node(N::Index,n);read->add(node(N::Name,n,results));read->add(node(N::Name,n,slot));value.children[1]=std::move(read);
            auto increment=node(N::ExpressionStatement,n),update=node(N::Update,n,"++");update->add(node(N::Name,n,slot));increment->add(std::move(update));loop->children[2]->add(std::move(increment));block->add(std::move(loop));
            n=std::move(*block);for(auto& c:n.children)walk(*c);return;
        }
        for(auto& c:n.children)walk(*c);
        if(n.kind==N::Unary&&n.text=="spawn"){
            if(n.children[0]->kind!=N::Call)bad(n,"spawn requires a function call");
            auto launch=call(n,"task_start");for(auto& c:n.children[0]->children)launch->add(std::move(c));n=std::move(*launch);
        }else if(n.kind==N::Unary&&n.text=="await"){
            auto wait=call(n,"task_await");wait->add(std::move(n.children[0]));n=std::move(*wait);
        }else if(n.kind==N::Block&&n.text=="taskgroup"){
            auto name="$taskgroup$"+std::to_string(serial++);auto start=node(N::Let,n,name);start->add(call(n,"task_group_begin"));auto defer=node(N::Defer,n);auto end=call(n,"task_group_end");end->add(node(N::Name,n,name));defer->add(std::move(end));n.children.insert(n.children.begin(),std::move(defer));n.children.insert(n.children.begin(),std::move(start));n.text="";
        }
    }
};
}
void lower_concurrency(Node& program){Lower lower;lower.walk(program);}
}
