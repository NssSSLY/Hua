#include "hua/module.hpp"
#include "hua/lowering.hpp"
#include "hua/lexer.hpp"
#include "hua/parser.hpp"
#include "hua/value.hpp"
#include "hua/runtime.hpp"
#include <algorithm>
#include <cwctype>
#include <fstream>
#include <functional>
#include <unordered_set>
namespace hua {
namespace {
using N=NodeKind;
std::string utf8(const std::filesystem::path& p){auto s=p.u8string();return {s.begin(),s.end()};}
std::string key(const std::filesystem::path& p) {
#ifdef _WIN32
    auto s=p.wstring();std::transform(s.begin(),s.end(),s.begin(),[](wchar_t c){return std::towlower(c);});
    return utf8(std::filesystem::path(s));
#else
    return utf8(p);
#endif
}
[[noreturn]] void error(const SourceSpan& s,const std::string& reason,std::string code="E5001") {throw Diagnostic(std::move(code),s,reason);}
std::string dotted(const Node& n) {
    if(n.kind==N::Name)return n.text;
    if(n.kind==N::Member){auto path=dotted(*n.children[0]);if(!path.empty())return path+"."+n.text;}
    return "";
}
}
std::vector<const Source*> ModuleLoader::sources() const {std::vector<const Source*> out;for(const auto& m:modules_)if(m->source)out.push_back(m->source.get());return out;}
const Source* ModuleLoader::source(const std::string& id) const {
    for(const auto& module:modules_)if(module->source && module->source->filename()==id)return module->source.get();return nullptr;
}
const Node& ModuleLoader::load(const std::filesystem::path& entry) {
    linked_.reset();modules_.clear();cache_.clear();ordered_.clear();loading_.clear();bytes_=0;
    std::error_code ec;auto path=std::filesystem::canonical(entry,ec);
    if(ec)error({utf8(entry),0,0,1,1},"cannot read source file: "+utf8(entry),"E0001");
    root_=path.parent_path();external_=std::make_shared<ExternalRegistry>(root_);auto& main=visit(path,{utf8(path),0,0,1,1},true);
    linked_=std::make_unique<Node>(N::Program,main.tree->span);
    for(auto module:ordered_) {auto tree=link(*module);for(auto& n:tree->children)linked_->add(std::move(n));}
    lower_language(*linked_);
    return *linked_;
}
std::filesystem::path ModuleLoader::resolve(const std::string& name,const SourceSpan& site) const {
    // Import grammar accepts ASCII identifier components only; it cannot express absolute paths or '..'.
    std::string relative=name;std::replace(relative.begin(),relative.end(),'.','/');
    auto base=root_/relative;auto file=base;file+=".hua";
    std::error_code ec;auto native_file=base;native_file+=".huam";auto wasm_file=base;wasm_file+=".wasm";
    for(const auto& candidate:{file,base/"package.hua",native_file,base/"package.huam",wasm_file,base/"package.wasm"}) {
        if(std::filesystem::is_regular_file(candidate,ec) && !ec) {
            auto path=std::filesystem::canonical(candidate,ec);if(ec)error(site,"cannot resolve module `"+name+"`");
            auto rel=std::filesystem::relative(path,root_,ec);
            if(ec || rel.empty() || *rel.begin()=="..")error(site,"module `"+name+"` resolves outside the entry source directory");
            return path;
        }
        ec.clear();
    }
    error(site,"cannot resolve module `"+name+"`; searched "+utf8(file)+" and "+utf8(base/"package.hua"));
}
ModuleLoader::Module& ModuleLoader::visit(const std::filesystem::path& path,const SourceSpan& site,bool entry) {
    auto cache_key=key(path);
    if(auto found=cache_.find(cache_key);found!=cache_.end()) {
        if(found->second->state==Module::State::Loading) {
            std::string chain;for(auto module:loading_){if(!chain.empty())chain+=" -> ";chain+=utf8(module->path.filename());}
            error(site,"cyclic module import: "+chain+" -> "+utf8(path.filename()),"E5002");
        }
        return *found->second;
    }
    if(modules_.size()>=128 || loading_.size()>=64)error(site,"module graph limit exceeded (128 files / 64 dependency levels)","E5005");
    std::ifstream input(path,std::ios::binary|std::ios::ate);
    if(!input)error(site,"cannot read source file: "+utf8(path),entry?"E0001":"E5001");
    auto size=input.tellg();
    if(size<0 || size>16*1024*1024)error(site,"source file is unreadable or exceeds 16 MiB",entry?"E0001":"E5005");
    if(bytes_+static_cast<std::size_t>(size)>64*1024*1024)error(site,"module source graph exceeds 64 MiB","E5005");
    bytes_+=static_cast<std::size_t>(size);std::string text(static_cast<std::size_t>(size),'\0');input.seekg(0);
    if(!text.empty() && !input.read(text.data(),size))error(site,"failed to read complete source file",entry?"E0001":"E5001");
    auto owned=std::make_unique<Module>();owned->path=path;owned->prefix=entry?"":"$"+std::to_string(modules_.size())+"$";
    bool native=path.extension()==".huam",wasm=path.extension()==".wasm";
    std::string binary;std::vector<ExternalExport> wasm_exports;
    if(wasm) {
        binary=std::move(text);wasm_exports=ExternalRegistry::inspect_wasm(binary,site);
        text="# generated WASM interface\n";
        for(const auto& e:wasm_exports){text+="pub fn "+e.name+"(";for(std::size_t i=0;i<e.parameters.size();++i){if(i)text+=",";text+="a"+std::to_string(i)+" "+e.parameters[i];}text+=") "+e.result+" {}\n";}
    }
    owned->source=std::make_unique<Source>(utf8(path),std::move(text));auto& module=*owned;
    modules_.push_back(std::move(owned));cache_.emplace(cache_key,&module);loading_.push_back(&module);
    if(native) {
        if(module.source->line_text(1)!="HUA_NATIVE 1")error(module.source->span(0,1),"native manifest requires HUA_NATIVE 1 header","E7002");
        auto parse_text=module.source->text();parse_text[0]='#';Source interface(module.source->filename(),std::move(parse_text));
        module.tree=Parser(Lexer(interface).scan()).parse();
    }else module.tree=Parser(Lexer(*module.source).scan()).parse();
    if(native||wasm) {
        ExternalModule external{};external.id=module.prefix+"module";external.span=module.tree->span;
        external.kind=native?ExternalKind::Native:ExternalKind::Wasm;external.wasm=std::move(binary);
        auto artifact=path;
        if(native) {
#ifdef _WIN32
            artifact.replace_extension(".dll");
#elif __APPLE__
            artifact.replace_extension(".dylib");
#else
            artifact.replace_extension(".so");
#endif
            if(!std::filesystem::is_regular_file(artifact))error(site,"native manifest binary is missing","E7001");
        }
        external.artifact=utf8(std::filesystem::relative(artifact,root_));
        for(auto& n:module.tree->children) {
            if(n->kind!=N::Function || n->text.find(" pub")==std::string::npos || n->text.find(" unsafe")!=std::string::npos || declared_name(*n).find('.')!=std::string::npos || !n->children.back()->children.empty())error(n->span,"native/WASM interfaces require public bodyless scalar functions","E7003");
            ExternalExport e;e.name=declared_name(*n);e.linked_name=module.prefix+e.name;e.result="void";
            for(const auto& c:n->children) {if(c->kind==N::Parameter){if(c->children.size()!=1 || c->text.find(" mut")!=std::string::npos)error(c->span,"external parameters require scalar type annotations","E7003");e.parameters.push_back(type_name(*c->children[0]));}
                if(c->kind==N::ReturnTypes){if(c->children.size()!=1)error(c->span,"external functions require one return type","E7003");e.result=type_name(*c->children[0]);}}
            external.exports.push_back(std::move(e));n->text+=" external";
        }
        external_->add(std::move(external));
        module.tree->children.insert(module.tree->children.begin(),std::make_unique<Node>(N::ExternalInit,module.tree->span,module.prefix+"module"));
    }
    if(!native&&!wasm)lower_declarations(*module.tree);
    bool declarations=false;
    for(const auto& n:module.tree->children) {
        if(n->kind==N::Import) {
            if(declarations)error(n->span,"imports must precede all declarations and statements","E5004");
            auto binding=n->children.empty()?n->text:n->children[0]->text;
            if(module.imports.contains(binding))error(n->span,"duplicate import namespace `"+binding+"`","E5004");
            if(n->text=="std"||n->text.starts_with("std."))module.imports.emplace(binding,&standard(n->text,n->span));
            else {auto dependency=resolve(n->text,n->span);module.imports.emplace(binding,&visit(dependency,n->span));}
        }else {
            declarations=true;if(n->kind==N::Struct&&(declared_name(*n)=="Json"||declared_name(*n)=="Bytes"||declared_name(*n)=="Buffer"||declared_name(*n)=="List"||declared_name(*n)=="Task"))error(n->span,"reserved standard value type","E3002");
            if(n->kind==N::MultiBinding){for(const auto& p:n->children[0]->children)if(p->text!="_")module.globals.emplace(p->text,module.prefix+p->text);}
            if(n->kind==N::Function || n->kind==N::Struct || n->kind==N::Let || n->kind==N::Var || n->kind==N::Const) {
                auto name=declared_name(*n);module.globals.emplace(name,module.prefix+name);
                if(n->text.find(" pub")!=std::string::npos)module.exports.emplace(name,n.get());
            }
        }
    }
    for(const auto& [name,_]:module.imports)if(module.globals.contains(name.substr(0,name.find('.'))))
        error(module.tree->span,"import namespace conflicts with declaration `"+name+"`","E5004");
    loading_.pop_back();module.state=Module::State::Loaded;ordered_.push_back(&module);return module;
}
ModuleLoader::Module& ModuleLoader::standard(const std::string& name,const SourceSpan& site) {
    auto id="builtin:"+name;if(auto found=cache_.find(id);found!=cache_.end())return *found->second;
    if(!standard_module(name))error(site,"unknown standard module `"+name+"`");
    if(modules_.size()>=128)error(site,"module graph limit exceeded","E5005");
    auto owned=std::make_unique<Module>();owned->standard=true;owned->state=Module::State::Loaded;
    std::string text="# built-in standard interface; implemented by the shared runtime\n";
    for(const auto& f:standard_functions())if(name=="std."+f.module){text+="pub fn "+f.name+"(";for(std::size_t i=0;i<f.parameters.size();++i){if(i)text+=",";text+="a"+std::to_string(i);if(!f.parameters[i].empty())text+=" "+f.parameters[i];}text+=") "+f.result+" {}\n";owned->globals.emplace(f.name,"$core$"+standard_name(f));}
    if(name=="std.error"){text+="pub struct Value {}\n";owned->globals.emplace("Value","std.error.Value");}
    if(name=="std.json"){text+="pub struct Value {}\n";owned->globals.emplace("Value","Json");}
    owned->source=std::make_unique<Source>("<"+name+">",std::move(text));owned->tree=Parser(Lexer(*owned->source).scan()).parse();
    for(const auto& n:owned->tree->children)owned->exports.emplace(declared_name(*n),n.get());
    auto& module=*owned;modules_.push_back(std::move(owned));cache_.emplace(id,&module);ordered_.push_back(&module);return module;
}
NodePtr ModuleLoader::link(Module& module) {
    if(module.standard)return std::make_unique<Node>(N::Program,module.tree->span);
    std::vector<std::unordered_set<std::string>> locals;
    std::vector<std::unordered_set<std::string>> local_types;
    auto local=[&](const std::string& name){for(auto i=locals.rbegin();i!=locals.rend();++i)if(i->contains(name))return true;return false;};
    auto imported=[&](const std::string& name,const SourceSpan& span,bool require_type)->std::string {
        auto root=name.substr(0,name.find('.'));if(!require_type && local(root))return "";
        for(const auto& [prefix,target]:module.imports)if(name.starts_with(prefix+".")) {
            auto member=name.substr(prefix.size()+1);
            auto found=target->exports.find(member);
            if(found==target->exports.end())error(span,"module `"+prefix+"` has no public export `"+member+"`","E5003");
            if(require_type && found->second->kind!=N::Struct)error(span,"module export `"+name+"` is not a struct type","E5003");
            if(!require_type && found->second->kind==N::Struct)error(span,"struct type `"+name+"` requires a named-field initializer","E5003");
            return target->globals.at(member);
        }
        return "";
    };
    auto type=[&](const std::string& name,const SourceSpan& span) {
        if(name.empty())return std::string{};
        for(auto i=local_types.rbegin();i!=local_types.rend();++i)if(i->contains(name))return name;
        if(auto linked=imported(name,span,true);!linked.empty())return linked;
        static const std::unordered_set<std::string> primitives={"int","float","bool","string","byte","i8","i16","i32","i64","u8","u16","u32","u64","f32","f64","usize","isize","void","Result","Json","Bytes","Buffer","Task"};
        if(primitives.contains(name))return name;
        if(auto found=module.globals.find(name);found!=module.globals.end())return found->second;
        return module.prefix+name;
    };
    std::function<NodePtr(const Node&,bool)> rewrite;
    rewrite=[&](const Node& n,bool top)->NodePtr {
        if(n.kind==N::Import)error(n.span,"imports are allowed only at the start of a module","E5004");
        if(n.kind==N::Member) {
            auto path=dotted(n);auto linked=imported(path,n.span,false);
            if(!linked.empty())return std::make_unique<Node>(N::Name,n.span,linked);
            if(auto found=module.globals.find(path);found!=module.globals.end())return std::make_unique<Node>(N::Name,n.span,found->second);
        }
        auto out=std::make_unique<Node>(n.kind,n.span,n.text);
        if(n.kind==N::MatchArm){out->add(rewrite(*n.children[0],false));out->add(rewrite(*n.children[1],false));locals.emplace_back();for(const auto& p:n.children[1]->children)locals.back().insert(p->text);out->add(rewrite(*n.children[2],false));locals.pop_back();return out;}
        if(n.kind==N::Name) {
            if(n.text.find('.')!=std::string::npos){auto linked=imported(n.text,n.span,false);if(!linked.empty()){out->text=linked;return out;}}
            if(!local(n.text)) {
                for(const auto& [prefix,_]:module.imports)if(prefix.substr(0,prefix.find('.'))==n.text)
                    error(n.span,"module namespace `"+n.text+"` must be used as a qualified export","E5003");
                if(auto found=module.globals.find(n.text);found!=module.globals.end())out->text=found->second;
                else {
                    out->text=builtin_names().contains(n.text)?"$core$"+n.text:module.prefix+n.text;
                }
            }
            return out;
        }
        if(n.kind==N::MultiAssignment) {
            auto list=std::make_unique<Node>(N::BindingList,n.children[0]->span);for(const auto& t:n.children[0]->children)list->add(t->kind==N::Name&&t->text=="_"?std::make_unique<Node>(N::Name,t->span,"_"):rewrite(*t,false));out->add(std::move(list));out->add(rewrite(*n.children[1],false));return out;
        }
        if(n.kind==N::TypeName || n.kind==N::StructLiteral || (n.kind==N::GenericType&&n.text!="map"&&n.text!="Result"&&n.text!="List"&&n.text!="Task"&&n.text!="ptr"&&n.text!="ref"))out->text=type(n.text,n.span);
        if(n.kind==N::Function) {
            if(!top && !locals.empty())locals.back().insert(declared_name(n));
            if(top)out->text=module.prefix+n.text;
            local_types.emplace_back();for(const auto& c:n.children)if(c->kind==N::GenericParameters)for(const auto& p:c->children)local_types.back().insert(p->text);
            locals.emplace_back();for(const auto& c:n.children)if(c->kind==N::Parameter)locals.back().insert(declared_name(*c));
            if(declared_name(n).find('.')!=std::string::npos)locals.back().insert("self");
            for(const auto& c:n.children)out->add(rewrite(*c,false));locals.pop_back();local_types.pop_back();return out;
        }
        if(n.kind==N::Struct){if(top)out->text=module.prefix+n.text;else if(!local_types.empty())local_types.back().insert(declared_name(n));local_types.emplace_back();for(const auto& c:n.children)if(c->kind==N::GenericParameters)for(const auto& p:c->children)local_types.back().insert(p->text);for(const auto& c:n.children)out->add(rewrite(*c,false));local_types.pop_back();return out;}
        if(n.kind==N::MultiBinding) {
            auto list=std::make_unique<Node>(N::BindingList,n.children[0]->span);
            for(const auto& p:n.children[0]->children){auto item=rewrite(*p,false);if(top&&p->text!="_")item->text=module.prefix+p->text;list->add(std::move(item));}
            out->add(std::move(list));out->add(rewrite(*n.children[1],false));
            if(!top&&!locals.empty())for(const auto& p:n.children[0]->children)if(p->text!="_")locals.back().insert(p->text);return out;
        }
        if(n.kind==N::Let || n.kind==N::Var || n.kind==N::Const) {
            if(top)out->text=module.prefix+n.text;
            for(const auto& c:n.children)out->add(rewrite(*c,false));
            if(!top && !locals.empty())locals.back().insert(n.text);return out;
        }
        if(n.kind==N::Block) {
            locals.emplace_back();local_types.emplace_back();for(const auto& c:n.children)out->add(rewrite(*c,false));locals.pop_back();local_types.pop_back();return out;
        }
        if(n.kind==N::For) {
            auto count=n.children.size()-2;
            for(std::size_t i=0;i<count;++i)out->add(std::make_unique<Node>(N::Name,n.children[i]->span,n.children[i]->text));
            out->add(rewrite(*n.children[count],false));locals.emplace_back();
            for(std::size_t i=0;i<count;++i)locals.back().insert(n.children[i]->text);
            out->add(rewrite(*n.children.back(),false));locals.pop_back();return out;
        }
        for(const auto& c:n.children)out->add(rewrite(*c,false));if(n.kind==N::Field&&n.text.starts_with('$')&&n.text.find('#')!=std::string::npos&&out->children.size()>1)out->text=n.text.substr(0,n.text.rfind('#')+1)+type_name(*out->children[1]);return out;
    };
    auto program=std::make_unique<Node>(N::Program,module.tree->span);
    for(const auto& n:module.tree->children)if(n->kind!=N::Import)program->add(rewrite(*n,true));return program;
}
}
