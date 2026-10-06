#include "hua/bytecode.hpp"
#include <algorithm>
#include <iomanip>
#include <sstream>
namespace hua {
namespace { using N=NodeKind;
constexpr const char* op_names[]={"CONSTANT","LOAD","BIND","POP","UNARY","BINARY","JUMP","JUMP_FALSE","JUMP_TRUE",
    "ENTER_SCOPE","LEAVE_SCOPE","UNWIND","LOCATE_NAME","LOCATE_FIELD","LOCATE_INDEX","STORE",
    "MAKE_ARRAY","INDEX","SLICE","MAKE_STRUCT","MEMBER","CALL","RETURN",
    "RANGE_INIT","SLICE_INIT","ITER_NEXT","ITER_END","FAIL","CHECK_SLICE","ARRAY_APPEND","INIT_FIELD","EXTERNAL_INIT","MAKE_MAP","MAP_INSERT","MAKE_MULTI","BIND_MULTI","STORE_MULTI","PROPAGATE","CHECK_INDEX","CLOSURE","DEFER"};
}
std::size_t BytecodeCompiler::emit(Op op,const Node& n,std::string text) {
    Instruction i{};i.op=op;i.span=n.span;i.text=std::move(text);
    code_->instructions.push_back(std::move(i));return code_->instructions.size()-1;
}
void BytecodeCompiler::patch(std::size_t i,std::size_t to){code_->instructions.at(i).target=to;}
Bytecode BytecodeCompiler::compile(const Node& program,const SemanticModel& model) {
    model_=&model;Bytecode out;out.initializer.name="<initialize>";code_=&out.initializer;depth_=0;loops_.clear();
    for(const auto& n:program.children)if(n->kind!=N::Function && n->kind!=N::Struct)statement(*n);
    emit(Op::Constant,program);emit(Op::Return,program);
    for(const auto& [name,fn]:model.functions) {
        if(fn->text.find(" external")!=std::string::npos)continue;
        auto& code=out.functions[fn];code.name=name;code_=&code;depth_=0;loops_.clear();
        if(fn->text.find(" unsafe")!=std::string::npos)emit(Op::Fail,*fn,"unsafe function execution/FFI is not implemented");
        if(fn->text.find(" abstract")!=std::string::npos)emit(Op::Fail,*fn,"interface prototype requires a concrete receiver");
        block(*fn->children.back(),false);emit(Op::Constant,*fn);emit(Op::Return,*fn);
    }
    code_=nullptr;model_=nullptr;return out;
}
void BytecodeCompiler::block(const Node& n,bool scope) {
    if(scope){emit(Op::EnterScope,n);++depth_;}
    for(const auto& c:n.children)statement(*c);
    if(scope){emit(Op::LeaveScope,n);--depth_;}
}
void BytecodeCompiler::statement(const Node& n) {
    switch(n.kind) {
    case N::Function:{emit(Op::Closure,n,declared_name(n));auto i=emit(Op::Bind,n,declared_name(n));code_->instructions[i].type="fn:"+declared_name(n);code_->instructions[i].contextual={false};break;}
    case N::Defer:{const auto& call=*n.children[0];for(const auto& c:call.children)expression(*c);auto i=emit(Op::Defer,n);code_->instructions[i].argument=call.children.size()-1;for(std::size_t j=1;j<call.children.size();++j)code_->instructions[i].contextual.push_back(contextual_literal(*call.children[j]));break;}
    case N::ExternalInit:emit(Op::ExternalInit,n,n.text);break;
    case N::Block:block(n);break;
    case N::Let:case N::Var:case N::Const: {
        if(n.kind==N::Const){auto i=emit(Op::Constant,n);code_->instructions[i].constant=model_->constants.at(&n);}
        else expression(*n.children.back());
        auto i=emit(Op::Bind,n,n.text);auto& op=code_->instructions[i];op.type=model_->types.at(&n);
        op.flag=n.kind==N::Var;op.contextual={contextual_literal(*n.children.back())};break;
    }
    case N::MultiBinding: {
        expression(*n.children[1]);auto i=emit(Op::BindMulti,n);auto& op=code_->instructions[i];op.argument=n.children[0]->children.size();op.flag=n.text=="var";op.type="multi<";
        for(std::size_t j=0;j<op.argument;++j){const auto& p=*n.children[0]->children[j];op.names.push_back(p.text);if(j)op.type+=',';if(p.text!="_")op.type+=model_->types.at(&p);}op.type+='>';break;
    }
    case N::MultiAssignment:for(const auto& target:n.children[0]->children)location(*target);expression(*n.children[1]);code_->instructions[emit(Op::StoreMulti,n)].argument=n.children[0]->children.size();break;
    case N::ExpressionStatement:expression(*n.children[0]);emit(Op::Pop,n);break;
    case N::Match:{
        emit(Op::EnterScope,n);++depth_;auto name="$match$"+std::to_string(code_->instructions.size());expression(*n.children[0]);auto binding=emit(Op::Bind,n,name);code_->instructions[binding].type=model_->types.at(&n);code_->instructions[binding].contextual={false};std::vector<std::size_t> done;auto structure=model_->structures.find(model_->types.at(&n));bool enumeration=structure!=model_->structures.end()&&structure->second->text.find(" enum")!=std::string::npos;
        for(std::size_t i=1;i<n.children.size();++i){const auto& arm=*n.children[i];const auto& pattern=*arm.children[0];bool any=pattern.kind==N::Name&&display_type(pattern.text)=="_";std::size_t next=0;
            if(!any){emit(Op::Load,pattern,name);if(enumeration)emit(Op::Member,pattern,"$case");code_->instructions[emit(Op::Constant,pattern)].constant=model_->constants.at(&pattern);emit(Op::Binary,pattern,"==");next=emit(Op::JumpFalse,pattern);}
            emit(Op::EnterScope,arm);++depth_;
            if(!any&&enumeration)for(std::size_t j=0;j<arm.children[1]->children.size();++j){const auto& p=*arm.children[1]->children[j];if(p.text=="_")continue;emit(Op::Load,p,name);emit(Op::Member,p,"$"+show(model_->constants.at(&pattern))+"#"+std::to_string(j)+"#"+model_->types.at(&p));auto op=emit(Op::Bind,p,p.text);code_->instructions[op].type=model_->types.at(&p);code_->instructions[op].contextual={false};}
            block(*arm.children[2],false);emit(Op::LeaveScope,arm);--depth_;done.push_back(emit(Op::Jump,arm));if(!any)patch(next,code_->instructions.size());
        }
        for(auto jump:done)patch(jump,code_->instructions.size());emit(Op::LeaveScope,n);--depth_;break;
    }
    case N::If: {
        expression(*n.children[0]);auto branch=emit(Op::JumpFalse,*n.children[0]);statement(*n.children[1]);
        auto done=emit(Op::Jump,n);patch(branch,code_->instructions.size());
        if(n.children.size()==3)statement(*n.children[2]);patch(done,code_->instructions.size());break;
    }
    case N::While: {
        auto next=code_->instructions.size();expression(*n.children[0]);auto exit=emit(Op::JumpFalse,*n.children[0]);
        loops_.push_back({depth_,next,{}});statement(*n.children[1]);patch(emit(Op::Jump,n),next);
        auto end=code_->instructions.size();patch(exit,end);for(auto b:loops_.back().breaks)patch(b,end);loops_.pop_back();break;
    }
    case N::For: {
        auto count=n.children.size()-2;const auto& iterable=*n.children[count];
        if(iterable.kind==N::Range) {
            expression(*iterable.children[0]);expression(*iterable.children[1]);
            if(iterable.children.size()==3)expression(*iterable.children[2]);
            else {auto i=emit(Op::Constant,n);code_->instructions[i].constant=Value(std::int64_t{1});}
            auto i=emit(Op::RangeInit,iterable);code_->instructions[i].flag=iterable.text=="..=";
        } else {expression(iterable);emit(Op::SliceInit,iterable);}
        auto next=emit(Op::IterNext,n);for(std::size_t j=0;j<count;++j)code_->instructions[next].names.push_back(n.children[j]->text);
        loops_.push_back({depth_,next,{}});++depth_;block(*n.children.back(),false);--depth_;
        emit(Op::LeaveScope,n);patch(emit(Op::Jump,n),next);
        auto end=emit(Op::IterEnd,n);patch(next,end);for(auto b:loops_.back().breaks)patch(b,end);loops_.pop_back();break;
    }
    case N::Break:case N::Continue: {
        if(loops_.empty())throw Diagnostic("E6001",n.span,"loop control outside a compiled loop");
        auto i=emit(Op::Unwind,n);code_->instructions[i].argument=depth_-loops_.back().depth;
        auto jump=emit(Op::Jump,n);
        if(n.kind==N::Continue)patch(jump,loops_.back().next);else loops_.back().breaks.push_back(jump);break;
    }
    case N::Return:
        if(n.children.size()>1){for(const auto& c:n.children)expression(*c);auto i=emit(Op::MakeMulti,n);code_->instructions[i].argument=n.children.size();for(const auto& c:n.children)code_->instructions[i].contextual.push_back(contextual_literal(*c));emit(Op::Return,n);break;}
        if(n.children.empty())emit(Op::Constant,n);else expression(*n.children[0]);
        code_->instructions[emit(Op::Return,n)].flag=!n.children.empty()&&contextual_literal(*n.children[0]);break;
    case N::Import:emit(Op::Fail,n,"module loading requires the ModuleLoader entry point");break;
    case N::Unsafe:emit(Op::Fail,n,"unsafe execution/FFI is not implemented");break;
    default:emit(Op::Fail,n,"statement is not executable in this language subset");break;
    }
}
void BytecodeCompiler::location(const Node& n) {
    if(n.kind==N::Name)emit(Op::LocateName,n,n.text);
    else if(n.kind==N::Member){expression(*n.children[0]);emit(Op::LocateField,n,n.text);}
    else if(n.kind==N::Index){expression(*n.children[0]);emit(Op::CheckIndex,n);expression(*n.children[1]);emit(Op::LocateIndex,n);}
    else emit(Op::Fail,n,"invalid assignment target");
}
void BytecodeCompiler::expression(const Node& n) {
    if(numeric_literal(n) || n.kind==N::String || n.kind==N::Boolean || n.kind==N::Nil) {
        try {auto value=literal(n);code_->instructions[emit(Op::Constant,n)].constant=std::move(value);}
        catch(const Diagnostic& e){auto i=emit(Op::Fail,n,e.what());code_->instructions[i].type=e.code();}return;
    }
    switch(n.kind) {
    case N::Name:emit(Op::Load,n,n.text);break;
    case N::Pack:{for(const auto& c:n.children)expression(*c);auto i=emit(Op::MakeMulti,n);code_->instructions[i].argument=n.children.size();for(const auto& c:n.children)code_->instructions[i].contextual.push_back(contextual_literal(*c));break;}
    case N::MapLiteral:{auto i=emit(Op::MakeMap,n);code_->instructions[i].type=type_name(*n.children[0]);for(std::size_t j=1;j<n.children.size();++j){expression(*n.children[j]->children[0]);expression(*n.children[j]->children[1]);code_->instructions[emit(Op::MapInsert,*n.children[j])].flag=contextual_literal(*n.children[j]->children[1]);}break;}
    case N::Propagate:expression(*n.children[0]);emit(Op::Propagate,n);break;
    case N::Unary:
        if(n.text=="&"){emit(Op::Fail,n,"safe reference execution is not implemented");break;}
        expression(*n.children[0]);emit(Op::Unary,n,n.text);break;
    case N::Binary:
        expression(*n.children[0]);
        if(n.text=="&&" || n.text=="||") {
            // Conditional jumps consume the left value; the taken branch reconstructs a bool.
            auto jump=emit(n.text=="&&"?Op::JumpFalse:Op::JumpTrue,n);
            expression(*n.children[1]);emit(Op::Unary,n,"!");emit(Op::Unary,n,"!");
            auto done=emit(Op::Jump,n);patch(jump,code_->instructions.size());
            code_->instructions[emit(Op::Constant,n)].constant=Value(n.text=="||");patch(done,code_->instructions.size());
        }else{expression(*n.children[1]);code_->instructions[emit(Op::Binary,n,n.text)].flag=numeric_literal(*n.children[1]);}break;
    case N::Assignment:case N::Update: {
        location(*n.children[0]);
        if(n.kind==N::Update)code_->instructions[emit(Op::Constant,n)].constant=Value(std::int64_t{1});
        else expression(*n.children[1]);
        auto i=emit(Op::Store,n,n.kind==N::Update?(n.text=="++"?"+=":"-="):n.text);
        code_->instructions[i].flag=n.kind==N::Update||(n.kind==N::Assignment&&contextual_literal(*n.children[1]));break;
    }
    case N::Array:
        emit(Op::MakeArray,n);for(const auto& c:n.children){expression(*c);code_->instructions[emit(Op::ArrayAppend,*c)].flag=contextual_literal(*c);}break;
    case N::Index:expression(*n.children[0]);emit(Op::CheckIndex,n);expression(*n.children[1]);emit(Op::Index,n);break;
    case N::Slice: {
        expression(*n.children[0]);emit(Op::CheckSlice,n);
        for(std::size_t j=1;j<3;++j){if(n.children[j]->kind==N::Omitted)emit(Op::Constant,*n.children[j]);else expression(*n.children[j]);}
        auto i=emit(Op::Slice,n);code_->instructions[i].contextual={n.children[1]->kind==N::Omitted,n.children[2]->kind==N::Omitted};break;
    }
    case N::StructLiteral: {
        emit(Op::MakeStruct,n,n.text);
        for(const auto& c:n.children) {
            expression(*c->children[0]);auto i=emit(Op::InitField,*c,c->text);
            code_->instructions[i].flag=contextual_literal(*c->children[0]);
        }break;
    }
    case N::Member:expression(*n.children[0]);emit(Op::Member,n,n.text);break;
    case N::Call: {
        for(const auto& c:n.children)expression(*c);auto i=emit(Op::Call,n);auto& op=code_->instructions[i];op.argument=n.children.size()-1;op.type=model_->types.at(&n);
        for(std::size_t j=1;j<n.children.size();++j)op.contextual.push_back(contextual_literal(*n.children[j]));break;
    }
    default:emit(Op::Fail,n,"expression is not executable in this language subset");break;
    }
}
std::string disassemble(const Bytecode& bytecode) {
    std::ostringstream out;
    auto dump=[&](const Code& c) {
        out<<"code "<<c.name<<'\n';
        for(std::size_t pc=0;pc<c.instructions.size();++pc) {
            const auto& i=c.instructions[pc];out<<std::setw(5)<<pc<<"  "<<op_names[static_cast<unsigned>(i.op)];
            if(!i.text.empty())out<<' '<<std::quoted(i.text);
            if(i.op==Op::Constant)out<<' '<<std::quoted(show(i.constant));
            if(i.op==Op::Jump || i.op==Op::JumpFalse || i.op==Op::JumpTrue || i.op==Op::IterNext)out<<" -> "<<i.target;
            if(i.op==Op::Call || i.op==Op::MakeArray || i.op==Op::Unwind)out<<' '<<i.argument;
            out<<"  @ "<<i.span.file_id<<':'<<i.span.line<<':'<<i.span.column<<'\n';
        }
    };
    dump(bytecode.initializer);std::vector<const Code*> codes;
    for(const auto& [_,c]:bytecode.functions)codes.push_back(&c);
    std::sort(codes.begin(),codes.end(),[](auto a,auto b){return a->name<b->name;});for(auto c:codes)dump(*c);return out.str();
}
}
