#include "hua/parser.hpp"
#include <functional>
#include <utility>
#include <algorithm>
namespace hua {
namespace {
using K = TokenKind;
using N = NodeKind;
struct Depth {
    std::size_t& value;
    explicit Depth(std::size_t& counter, const SourceSpan& span) : value(counter) {
        if (value >= 192) throw Diagnostic("E2008", span, "syntax nesting limit exceeded");
        ++value;
    }
    ~Depth() { --value; }
};
template<class T> struct Restore {
    T& value; T old;
    Restore(T& target, T next) : value(target), old(target) { value = next; }
    ~Restore() { value = old; }
};
int precedence(K kind) {
    switch (kind) {
    case K::Assign: case K::PlusAssign: case K::MinusAssign: case K::StarAssign:
    case K::SlashAssign: case K::PercentAssign: case K::PowerAssign: case K::FloorAssign: return 1;
    case K::Or: return 2;
    case K::And: return 3;
    case K::Equal: case K::NotEqual: return 4;
    case K::Less: case K::LessEqual: case K::Greater: case K::GreaterEqual: return 5;
    case K::BitOr: return 6;
    case K::BitXor: return 7;
    case K::BitAnd: return 8;
    case K::ShiftLeft: case K::ShiftRight: return 9;
    case K::Plus: case K::Minus: return 10;
    case K::Star: case K::Slash: case K::FloorDivide: case K::Percent: return 11;
    case K::Power: return 13;
    default: return 0;
    }
}
bool assignable(const Node& node) {
    return node.kind == N::Name || node.kind == N::Member || node.kind == N::Index;
}
bool reserved(K kind) { return kind >= K::Let && kind <= K::Nil; }
}
Parser::Parser(std::vector<Token> tokens) : tokens_(std::move(tokens)) {
    if (tokens_.empty() || tokens_.back().kind != K::End)
        throw std::invalid_argument("Parser requires an EOF-terminated token stream");
}
const Token& Parser::peek(std::size_t ahead) const {
    return tokens_[std::min(pos_ + ahead, tokens_.size() - 1)];
}
bool Parser::at(K kind) const { return peek().kind == kind; }
Token Parser::take() { auto token = peek(); if (!at(K::End)) ++pos_; return token; }
bool Parser::accept(K kind) { if (!at(kind)) return false; take(); return true; }
[[noreturn]] void Parser::fail(const Token& token, std::string message, std::string code, std::string help) const {
    throw Diagnostic(std::move(code), token.span, std::move(message), std::move(help));
}
Token Parser::expect(K kind, std::string_view reason) {
    // >> remains a shift token in expressions; split it only when closing nested generic types.
    if (kind == K::Greater && at(K::ShiftRight)) {
        auto first = peek(); first.kind = K::Greater; first.text = ">"; --first.span.end_offset;
        auto& second = tokens_[pos_]; second.kind = K::Greater; second.text = ">";
        ++second.span.start_offset; ++second.span.column;
        return first;
    }
    if(kind==K::Greater&&at(K::GreaterEqual)){auto first=peek();first.kind=K::Greater;first.text=">";--first.span.end_offset;auto& second=tokens_[pos_];second.kind=K::Assign;second.text="=";++second.span.start_offset;++second.span.column;return first;}
    if (!at(kind)) fail(peek(), std::string(reason) + "; got " +
        (peek().text.empty() ? std::string(token_name(peek().kind)) : "`" + peek().text + "`"));
    return take();
}
void Parser::newlines() { while (accept(K::Newline)) {} }
void Parser::statement_end() {
    if (at(K::Newline)) { newlines(); return; }
    if (!at(K::End) && !at(K::RBrace))
        fail(peek(), "expected a newline or end of block after statement", "E2003", "put each statement on its own line");
}
NodePtr Parser::make(N kind, const Token& token, std::string text) {
    return std::make_unique<Node>(kind, token.span, std::move(text));
}
void Parser::finish(Node& node) {
    if (pos_ > 0) node.span.end_offset = tokens_[pos_ - 1].span.end_offset;
}
NodePtr Parser::parse() {
    auto root = make(N::Program, peek());
    newlines();
    while (!at(K::End)) root->add(statement());
    root->span.end_offset = peek().span.end_offset;
    return root;
}
NodePtr Parser::statement() {
    Depth depth(depth_, peek().span);
    NodePtr node;
    if(at(K::At)) {
        std::vector<std::string> attributes;
        while(accept(K::At)) {
            auto name=expect(K::Identifier,"expected attribute name");std::string value=name.text;
            if(value!="deprecated"&&value!="doc"&&value!="inline"&&value!="noinline"&&value!="entry")fail(name,"unsupported attribute `"+value+"`","E2002");
            if(accept(K::LParen)){auto argument=expect(K::String,"attribute argument must be a string");if(name.text!="deprecated"&&name.text!="doc")fail(name,"attribute takes no arguments","E2002");value+="=";for(unsigned char c:argument.text){constexpr char hex[]="0123456789abcdef";value+=hex[c>>4];value+=hex[c&15];}expect(K::RParen,"expected ) after attribute");}
            else if(value=="deprecated"||value=="doc")fail(name,"attribute requires a string argument","E2002");
            for(const auto& old:attributes)if(old.substr(0,old.find('='))==name.text)fail(name,"duplicate attribute","E2002");
            attributes.push_back(value);newlines();
        }
        bool pub=accept(K::Pub);
        if(accept(K::Async)){node=function(pub,false);node->text+=" async";}else if(at(K::Fn))node=function(pub,false);else if(at(K::Struct))node=structure(pub);else fail(peek(),"attributes require a function or struct declaration","E2002");
        for(const auto& attribute:attributes){if(node->kind!=N::Function&&(attribute=="inline"||attribute=="noinline"||attribute=="entry"))fail(peek(),"function attribute used on a struct","E2002");node->text+=" @"+attribute;}
        if(node->text.find(" @inline")!=std::string::npos&&node->text.find(" @noinline")!=std::string::npos)fail(peek(),"conflicting inline attributes","E2002");
    }
    else if (at(K::Let) || at(K::Var) || at(K::Const)) node = variable();
    else if (at(K::Defer)) {
        auto token=take();if(!function_depth_)fail(token,"defer requires a function scope","E2004");
        node=make(N::Defer,token);node->add(expression());if(node->children[0]->kind!=N::Call)fail(token,"defer requires a function call","E2002");finish(*node);
    }
    else if(at(K::Async)){auto modifier=take();node=function(false,false);node->text+=" async";node->span.start_offset=modifier.span.start_offset;node->span.line=modifier.span.line;node->span.column=modifier.span.column;}
    else if(at(K::Taskgroup)){auto token=take();if(!function_depth_)fail(token,"taskgroup requires a function scope","E2004");node=block();node->text="taskgroup";node->span.start_offset=token.span.start_offset;node->span.line=token.span.line;node->span.column=token.span.column;}
    else if(at(K::Parallel)||at(K::Simd)){auto modifier=take();std::string mode=modifier.text;if(modifier.kind==K::Parallel&&accept(K::Simd))mode+=" simd";node=for_loop();node->text=mode;node->span.start_offset=modifier.span.start_offset;node->span.line=modifier.span.line;node->span.column=modifier.span.column;}
    else if (at(K::Fn)) node = function(false, false);
    else if (at(K::Struct)) node = structure(false);
    else if (at(K::Interface)){if(function_depth_)fail(peek(),"interface requires module scope","E2002");node=interface_declaration(false);}
    else if (at(K::Enum)){if(function_depth_)fail(peek(),"enum requires module scope","E2002");node=enum_declaration(false);}
    else if (at(K::Match))node=match_statement();
    else if (at(K::Pub)) {
        auto modifier = take();
        if(accept(K::Async)){node=function(true,false);node->text+=" async";}
        else if (at(K::Fn)) node = function(true, false);
        else if (at(K::Struct)) node = structure(true);
        else if(at(K::Interface))node=interface_declaration(true);
        else if(at(K::Enum))node=enum_declaration(true);
        else if (accept(K::Unsafe)) {
            if (!at(K::Fn)) fail(peek(), "expected fn after pub unsafe");
            node = function(true, true);
        } else fail(peek(), "pub is supported only on struct and function declarations in Phase 1", "E2002");
        node->span.start_offset = modifier.span.start_offset;
        node->span.line = modifier.span.line; node->span.column = modifier.span.column;
    } else if (at(K::Unsafe)) {
        auto token = take();
        if (at(K::Fn)) node = function(false, true);
        else if (at(K::LBrace)) {
            node = make(N::Unsafe, token);
            Restore scope(unsafe_depth_, unsafe_depth_ + 1);
            node->add(block()); finish(*node);
        } else fail(peek(), "expected fn or block after unsafe");
        node->span.start_offset = token.span.start_offset;
        node->span.line = token.span.line; node->span.column = token.span.column;
    } else if (at(K::LBrace)) node = block();
    else if (at(K::If)) node = conditional();
    else if (at(K::While)) node = while_loop();
    else if (at(K::For)) node = for_loop();
    else if (at(K::Return)) {
        auto token = take();
        if (!function_depth_) fail(token, "return is allowed only inside a function", "E2004");
        node = make(N::Return, token);
        if (!at(K::Newline) && !at(K::RBrace) && !at(K::End)) {
            node->add(expression());
            while (accept(K::Comma)) node->add(expression());
        }
        finish(*node);
    } else if (at(K::Break) || at(K::Continue)) {
        auto token = take();
        if (!loop_depth_) fail(token, "break/continue is allowed only inside a loop", "E2004");
        node = make(token.kind == K::Break ? N::Break : N::Continue, token);
    } else if (at(K::Import)) {
        auto token = take();
        auto path = expect(K::Identifier, "expected module name");
        node = make(N::Import, token, path.text);
        while (accept(K::Dot)) node->text += "." + (node->text=="std"&&at(K::Simd)?take():expect(K::Identifier, "expected module path component")).text;
        if (accept(K::As)) { auto alias = expect(K::Identifier, "expected import alias"); node->add(make(N::Name, alias, alias.text)); }
        finish(*node);
    } else {
        if ((reserved(peek().kind) && !at(K::True) && !at(K::False) && !at(K::Nil) && !at(K::Await) && !at(K::Spawn)) || at(K::At) || at(K::Question))
            fail(peek(), "syntax `" + peek().text + "` is not implemented in Phase 1", "E2002");
        node = make(N::ExpressionStatement, peek());
        auto first=expression();
        if(accept(K::Comma)) {
            auto targets=std::make_unique<Node>(N::BindingList,first->span);targets->add(std::move(first));
            do {targets->add(expression(2));}while(accept(K::Comma));
            for(const auto& t:targets->children)if(!assignable(*t))fail(peek(),"multiple assignment requires assignment targets","E2007");
            finish(*targets);expect(K::Assign,"expected = in multiple assignment");
            auto rhs=expression();if(accept(K::Comma)){auto pack=std::make_unique<Node>(N::Pack,rhs->span);pack->add(std::move(rhs));do{pack->add(expression());}while(accept(K::Comma));finish(*pack);rhs=std::move(pack);}
            node=std::make_unique<Node>(N::MultiAssignment,node->span);node->add(std::move(targets));node->add(std::move(rhs));
        }else node->add(std::move(first));finish(*node);
    }
    statement_end();
    return node;
}
NodePtr Parser::variable() {
    const auto token=take();const auto name=expect(K::Identifier,"expected binding name");
    auto item=make(N::Parameter,name,name.text);if(!at(K::Assign)&&!at(K::Comma))item->add(type());finish(*item);
    if(accept(K::Comma)) {
        if(token.kind==K::Const)fail(token,"multiple const bindings are not supported","E2002");
        auto list=make(N::BindingList,name);list->add(std::move(item));
        do {auto next=expect(K::Identifier,"expected binding name");auto p=make(N::Parameter,next,next.text);if(!at(K::Assign)&&!at(K::Comma))p->add(type());finish(*p);list->add(std::move(p));}while(accept(K::Comma));
        finish(*list);expect(K::Assign,"expected = and an initializer for this declaration");auto rhs=expression();
        if(accept(K::Comma)){auto pack=std::make_unique<Node>(N::Pack,rhs->span);pack->add(std::move(rhs));do{pack->add(expression());}while(accept(K::Comma));finish(*pack);rhs=std::move(pack);}
        auto node=make(N::MultiBinding,token,token.text);node->add(std::move(list));node->add(std::move(rhs));finish(*node);return node;
    }
    auto node=make(token.kind==K::Let?N::Let:token.kind==K::Var?N::Var:N::Const,token,name.text);
    if(!item->children.empty())node->add(std::move(item->children[0]));
    expect(K::Assign,"expected = and an initializer for this Phase 1 declaration");node->add(expression());finish(*node);return node;
}
NodePtr Parser::function(bool is_public, bool is_unsafe, bool prototype) {
    const auto token = expect(K::Fn, "expected fn");
    auto name = expect(K::Identifier, "expected function name");
    if (accept(K::Dot)) name.text += "." + expect(K::Identifier, "expected method name").text;
    auto node = make(N::Function, token, name.text + (is_public ? " pub" : "") + (is_unsafe ? " unsafe" : ""));
    if(at(K::Less))node->add(generic_parameters());
    Restore unsafe(unsafe_depth_, is_unsafe ? 1 : 0);
    expect(K::LParen, "expected ( before parameters"); newlines();
    if (!at(K::RParen)) {
        do {
            newlines();
            bool mutable_parameter = at(K::Identifier) && peek().text == "mut";
            if (mutable_parameter) take();
            const auto parameter = expect(K::Identifier, "expected parameter name");
            auto param = make(N::Parameter, parameter, parameter.text + (mutable_parameter ? " mut" : ""));
            if (!at(K::Comma) && !at(K::RParen) && !at(K::Newline)) param->add(type());
            finish(*param); node->add(std::move(param)); newlines();
        } while (accept(K::Comma));
    }
    expect(K::RParen, "expected ) after parameters");
    if (!at(K::LBrace) && !(prototype&&(at(K::Newline)||at(K::RBrace)))) {
        auto result = make(N::ReturnTypes, peek());
        if (accept(K::LParen)) {
            newlines(); result->add(type()); newlines();
            while (accept(K::Comma)) { newlines(); result->add(type()); newlines(); }
            expect(K::RParen, "expected ) after return types");
        } else result->add(type());
        finish(*result); node->add(std::move(result));
    }
    Restore fn(function_depth_, function_depth_ + 1);
    Restore loops(loop_depth_, 0);
    if(prototype){node->add(make(N::Block,peek()));node->text+=" abstract";}else node->add(block());finish(*node);
    return node;
}
NodePtr Parser::interface_declaration(bool pub){
    auto token=expect(K::Interface,"expected interface"),name=expect(K::Identifier,"expected interface name");auto node=make(N::Interface,token,name.text+(pub?" pub":""));
    expect(K::LBrace,"expected { before interface methods");newlines();
    while(!at(K::RBrace)&&!at(K::End)){bool mut=at(K::Identifier)&&peek().text=="mut";if(mut)take();auto method=function(pub,false,true);method->text=name.text+"."+method->text+(mut?" mutreceiver":"");node->add(std::move(method));statement_end();}
    expect(K::RBrace,"expected } after interface");finish(*node);return node;
}
NodePtr Parser::enum_declaration(bool pub){
    auto token=expect(K::Enum,"expected enum"),name=expect(K::Identifier,"expected enum name");auto node=make(N::Enum,token,name.text+(pub?" pub":""));expect(K::LBrace,"expected { before variants");newlines();
    while(!at(K::RBrace)&&!at(K::End)){auto variant=expect(K::Identifier,"expected variant name");auto entry=make(N::Variant,variant,variant.text);if(accept(K::LParen)){newlines();if(!at(K::RParen))do{entry->add(type());newlines();}while(accept(K::Comma));expect(K::RParen,"expected ) after variant payload");}finish(*entry);node->add(std::move(entry));if(!accept(K::Comma))statement_end();newlines();}
    expect(K::RBrace,"expected } after enum");finish(*node);return node;
}
NodePtr Parser::match_statement(){
    auto node=make(N::Match,expect(K::Match,"expected match"));{Restore header(in_header_,true);node->add(expression());}
    expect(K::LBrace,"expected { after match value");newlines();
    while(!at(K::RBrace)&&!at(K::End)){
        auto arm=make(N::MatchArm,peek());NodePtr pattern;
        if(at(K::Identifier)){auto token=take();pattern=make(N::Name,token,token.text);while(accept(K::Dot))pattern->text+="."+expect(K::Identifier,"expected qualified variant").text;}
        else {auto token=take();if(token.kind==K::Integer)pattern=make(N::Integer,token,token.text);else if(token.kind==K::String)pattern=make(N::String,token,token.text);else if(token.kind==K::True||token.kind==K::False)pattern=make(N::Boolean,token,token.text);else if(token.kind==K::Nil)pattern=make(N::Nil,token);else fail(token,"match requires a literal, variant or _","E2002");}
        arm->add(std::move(pattern));auto bindings=make(N::BindingList,peek());if(accept(K::LParen)){if(!at(K::RParen))do{auto token=expect(K::Identifier,"expected payload binding");bindings->add(make(N::Parameter,token,token.text));}while(accept(K::Comma));expect(K::RParen,"expected ) after payload bindings");}arm->add(std::move(bindings));
        expect(K::Arrow,"expected => after match pattern");if(at(K::LBrace))arm->add(block());else {auto body=make(N::Block,peek()),statement=make(N::ExpressionStatement,peek());statement->add(expression());body->add(std::move(statement));arm->add(std::move(body));}finish(*arm);node->add(std::move(arm));if(!accept(K::Comma))statement_end();newlines();
    }
    expect(K::RBrace,"expected } after match");finish(*node);return node;
}
NodePtr Parser::generic_parameters(){
    auto node=make(N::GenericParameters,expect(K::Less,"expected <"));
    do{auto name=expect(K::Identifier,"expected generic type parameter");for(const auto& p:node->children)if(p->text==name.text)fail(name,"duplicate generic parameter","E2006");node->add(make(N::TypeName,name,name.text));}while(accept(K::Comma));
    expect(K::Greater,"expected > after generic parameters");finish(*node);return node;
}
bool Parser::specialization_ahead() const {
    if(!at(K::Less))return false;int depth=0;
    for(std::size_t i=pos_;i<tokens_.size()&&i<pos_+192;++i){auto k=tokens_[i].kind;if(k==K::Less)++depth;else if(k==K::Greater)--depth;else if(k==K::ShiftRight)depth-=2;else if(k==K::Newline||k==K::End||k==K::Assign)return false;if(depth<=0)return i+1<tokens_.size()&&(tokens_[i+1].kind==K::LParen||tokens_[i+1].kind==K::LBrace);}
    return false;
}
NodePtr Parser::structure(bool is_public) {
    const auto token = expect(K::Struct, "expected struct");
    const auto name = expect(K::Identifier, "expected struct name");
    auto node = make(N::Struct, token, name.text + (is_public ? " pub" : ""));
    if(at(K::Less))node->add(generic_parameters());
    expect(K::LBrace, "expected { before struct fields"); newlines();
    while (!at(K::RBrace) && !at(K::End)) {
        auto field = expect(K::Identifier, "expected field name");
        auto child = make(N::Field, field, field.text);
        child->add(type()); finish(*child); node->add(std::move(child));
        statement_end();
    }
    expect(K::RBrace, "expected } after struct fields"); finish(*node); return node;
}
NodePtr Parser::block() {
    Depth depth(depth_, peek().span);
    auto node = make(N::Block, expect(K::LBrace, "expected { to start block"));
    newlines();
    while (!at(K::RBrace) && !at(K::End)) node->add(statement());
    expect(K::RBrace, "expected } to close block"); finish(*node); return node;
}
NodePtr Parser::conditional() {
    Depth depth(depth_, peek().span);
    auto node = make(N::If, expect(K::If, "expected if"));
    {
        Restore header(in_header_, true);
        node->add(expression());
    }
    node->add(block());
    const auto after_block = pos_;
    newlines();
    if (accept(K::Else)) node->add(at(K::If) ? conditional() : block());
    else pos_ = after_block;
    finish(*node); return node;
}
NodePtr Parser::while_loop() {
    auto node = make(N::While, expect(K::While, "expected while"));
    {
        Restore header(in_header_, true);
        node->add(expression());
    }
    Restore loop(loop_depth_, loop_depth_ + 1);
    node->add(block()); finish(*node); return node;
}
NodePtr Parser::for_loop() {
    auto node = make(N::For, expect(K::For, "expected for"));
    auto first = expect(K::Identifier, "expected loop binding");
    node->add(make(N::Name, first, first.text));
    if (accept(K::Comma)) {
        auto second = expect(K::Identifier, "expected second loop binding");
        node->add(make(N::Name, second, second.text));
    }
    expect(K::In, "expected in after loop binding");
    {
        Restore header(in_header_, true);
        auto iterable = expression(2);
        if (at(K::Range) || at(K::RangeInclusive)) {
            auto op = take(); auto range = std::make_unique<Node>(N::Range, iterable->span, op.text);
            range->add(std::move(iterable)); range->add(expression(2));
            if (at(K::Identifier) && peek().text == "by") { take(); range->add(expression(2)); }
            finish(*range); iterable = std::move(range);
        } else if (at(K::Identifier) && peek().text == "by") fail(peek(), "by requires a range", "E2005");
        node->add(std::move(iterable));
    }
    Restore loop(loop_depth_, loop_depth_ + 1);
    node->add(block()); finish(*node); return node;
}
NodePtr Parser::type() {
    Depth depth(depth_, peek().span);
    const auto token = peek(); NodePtr result;
    if (at(K::Identifier) && peek().text == "mut") {
        take(); result = make(N::MutableType, token); result->add(type());
    } else if (accept(K::LBracket)) {
        if (at(K::Integer)) {
            auto count = take(); result = make(N::ArrayType, token, count.text);
        } else result = make(N::SliceType, token);
        expect(K::RBracket, "expected ] in array/slice type"); result->add(type());
    } else {
        auto name = expect(K::Identifier, "expected type name");
        if (name.text == "map") {
            result = make(N::GenericType, name, "map");
            expect(K::LBracket, "expected [ after map"); result->add(type());
            expect(K::RBracket, "expected ] after map key type"); result->add(type());
        } else {
            while (accept(K::Dot)) name.text += "." + expect(K::Identifier, "expected qualified type name").text;
            if (accept(K::Less)) {

                if (name.text == "ptr" && !unsafe_depth_)
                    fail(name, "raw ptr<T> requires an unsafe declaration or block", "E2010");
                result = make(N::GenericType, name, name.text); result->add(type());
                while (accept(K::Comma)) result->add(type());
                auto close = expect(K::Greater, "expected > after type arguments");
                if ((name.text == "Result" && result->children.size() > 2) ||
                    ((name.text=="ptr"||name.text=="ref"||name.text=="List"||name.text=="Task") && result->children.size() != 1))
                    fail(name, "incorrect number of type arguments", "E2006");
                result->span.end_offset = close.span.end_offset;
            } else {
                if (name.text == "ptr" || name.text == "ref") fail(name, "expected <T> after ptr/ref", "E2006");
                result = make(N::TypeName, name, name.text);
            }
        }
    }
    if (result->kind != N::GenericType || result->text == "map") finish(*result);
    if (accept(K::Question)) {
        auto optional = std::make_unique<Node>(N::OptionalType, result->span);
        optional->add(std::move(result)); finish(*optional); result = std::move(optional);
        if (at(K::Question)) fail(peek(), "duplicate optional type suffix", "E2006");
    }
    return result;
}
bool Parser::struct_literal_ahead() const {
    if (!at(K::LBrace)) return false;
    auto i = pos_ + 1;
    while (i < tokens_.size() && tokens_[i].kind == K::Newline) ++i;
    if (i == tokens_.size()) return false;
    if (tokens_[i].kind == K::RBrace) return !in_header_;
    return tokens_[i].kind == K::Identifier && i + 1 < tokens_.size() && tokens_[i + 1].kind == K::Colon;
}
NodePtr Parser::primary(bool multiline) {
    if (multiline) newlines();
    auto token = take();
    switch (token.kind) {
    case K::Integer: return make(N::Integer, token, token.text);
    case K::Float: return make(N::Float, token, token.text);
    case K::String: return make(N::String, token, token.text);
    case K::True: case K::False: return make(N::Boolean, token, token.text);
    case K::Nil: return make(N::Nil, token);
    case K::Identifier: {
        if(token.text=="map"&&at(K::LBracket)) {
            auto map=make(N::MapLiteral,token);auto t=make(N::GenericType,token,"map");take();t->add(type());expect(K::RBracket,"expected ] after map key type");t->add(type());finish(*t);map->add(std::move(t));
            expect(K::LBrace,"expected { after map value type");newlines();
            if(!at(K::RBrace))do{auto entry=make(N::MapEntry,peek());entry->add(expression(2,true));expect(K::Colon,"expected : after map key");entry->add(expression(1,true));finish(*entry);map->add(std::move(entry));newlines();}while(accept(K::Comma));
            expect(K::RBrace,"expected } after map literal");finish(*map);return map;
        }
        if (!struct_literal_ahead()) return make(N::Name, token, token.text);
        auto node = make(N::StructLiteral, token, token.text); take(); newlines();
        if (!at(K::RBrace)) {
            do {
                newlines(); auto name = expect(K::Identifier, "expected named struct field");
                auto field = make(N::FieldInit, name, name.text);
                expect(K::Colon, "expected : after field name");
                field->add(expression(1, true)); finish(*field);
                node->add(std::move(field)); newlines();
            } while (accept(K::Comma));
        }
        expect(K::RBrace, "expected } after struct literal"); finish(*node); return node;
    }
    case K::LParen: {
        Restore header(in_header_, false);
        auto node = expression(1, true); newlines(); expect(K::RParen, "expected ) after expression");
        node->span.start_offset = token.span.start_offset; node->span.line = token.span.line; node->span.column = token.span.column;
        finish(*node); return node;
    }
    case K::LBracket: {
        auto node = make(N::Array, token); newlines();
        if (!at(K::RBracket)) {
            do { node->add(expression(1, true)); newlines(); }
            while (accept(K::Comma));
        }
        expect(K::RBracket, "expected ] after array literal"); finish(*node); return node;
    }
    default:
        fail(token, reserved(token.kind) || token.kind == K::At || token.kind == K::Question
            ? "syntax `" + token.text + "` is not implemented in Phase 1"
            : "expected expression; got " + (token.text.empty() ? std::string(token_name(token.kind)) : "`" + token.text + "`"),
            reserved(token.kind) || token.kind == K::At || token.kind == K::Question ? "E2002" : "E2001");
    }
}
NodePtr Parser::postfix(NodePtr left, bool multiline) {
    std::size_t suffixes = 0;
    for (;;) {
        if (multiline) newlines();
        if (++suffixes > 192) fail(peek(), "postfix chain limit exceeded", "E2008");
        if(at(K::Less)&&specialization_ahead()) {
            auto node=std::make_unique<Node>(N::Specialize,left->span);node->add(std::move(left));take();node->add(type());while(accept(K::Comma))node->add(type());expect(K::Greater,"expected > after specialization");finish(*node);left=std::move(node);
        } else if (accept(K::LParen)) {
            auto node = std::make_unique<Node>(N::Call, left->span); node->add(std::move(left));
            Restore header(in_header_, false);
            newlines();
            if (!at(K::RParen)) {
                do { node->add(expression(1, true)); newlines(); }
                while (accept(K::Comma));
            }
            expect(K::RParen, "expected ) after call arguments"); finish(*node); left = std::move(node);
        } else if (accept(K::Dot)) {
            auto name = expect(K::Identifier, "expected member name after .");
            auto node = std::make_unique<Node>(N::Member, left->span, name.text);
            node->add(std::move(left)); finish(*node); left = std::move(node);
        } else if (accept(K::LBracket)) {
            Restore header(in_header_, false);
            auto start = peek(); newlines(); NodePtr first;
            if (!at(K::Colon)) first = expression(1, true);
            newlines();
            if (accept(K::Colon)) {
                auto node = std::make_unique<Node>(N::Slice, left->span);
                node->add(std::move(left));
                node->add(first ? std::move(first) : make(N::Omitted, start));
                newlines();
                node->add(at(K::RBracket) ? make(N::Omitted, peek()) : expression(1, true));
                newlines(); expect(K::RBracket, "expected ] after slice"); finish(*node); left = std::move(node);
            } else {
                auto node = std::make_unique<Node>(N::Index, left->span);
                node->add(std::move(left)); node->add(std::move(first));
                expect(K::RBracket, "expected ] after index"); finish(*node); left = std::move(node);
            }
        } else if (struct_literal_ahead() && (left->kind == N::Member||left->kind==N::Specialize)) {
            std::function<std::string(const Node&)> path = [&](const Node& n) -> std::string {
                if (n.kind == N::Name) return n.text;
                if (n.kind == N::Member) {
                    auto base = path(*n.children[0]);
                    if (!base.empty()) return base + "." + n.text;
                }
                return "";
            };
            auto name = path(left->kind==N::Specialize?*left->children[0]:*left);
            if (name.empty()) fail(peek(), "struct initializer requires a qualified type name");
            auto node = std::make_unique<Node>(N::StructLiteral, left->span, name);
            if(left->kind==N::Specialize){auto types=std::make_unique<Node>(N::GenericType,left->span,name);for(std::size_t i=1;i<left->children.size();++i)types->add(std::move(left->children[i]));node->add(std::move(types));}
            take(); newlines();
            if (!at(K::RBrace)) {
                do {
                    newlines(); auto field_name = expect(K::Identifier, "expected named struct field");
                    auto field = make(N::FieldInit, field_name, field_name.text);
                    expect(K::Colon, "expected : after field name");
                    field->add(expression(1, true)); finish(*field); node->add(std::move(field)); newlines();
                } while (accept(K::Comma));
            }
            expect(K::RBrace, "expected } after struct literal"); finish(*node); left = std::move(node);
        } else if (at(K::Increment) || at(K::Decrement)) {
            auto op = take(); if (!assignable(*left)) fail(op, "update requires a name, member or index", "E2007");
            auto node = std::make_unique<Node>(N::Update, left->span, op.text);
            node->add(std::move(left)); finish(*node); left = std::move(node);
        } else if (at(K::Question)) {
            auto q=take();if(!function_depth_)fail(q,"Result propagation requires a function","E2004");
            auto node=std::make_unique<Node>(N::Propagate,left->span);node->add(std::move(left));finish(*node);left=std::move(node);
        }
        else break;
    }
    return left;
}
NodePtr Parser::expression(int minimum, bool multiline) {
    Depth depth(depth_, peek().span);
    if (multiline) newlines();
    NodePtr left;
    if (at(K::Await) || at(K::Spawn) || at(K::Not) || at(K::BitNot) || at(K::Plus) || at(K::Minus) || at(K::BitAnd)) {
        auto op = take();if((op.kind==K::Await||op.kind==K::Spawn)&&!function_depth_)fail(op,"await/spawn requires a function scope","E2004");left = make(N::Unary, op, op.text);
        newlines(); left->add(expression(12, multiline)); finish(*left);
        if(op.kind==K::Await&&left->children[0]->kind==N::Propagate){auto propagation=std::move(left->children[0]);left->add(std::move(propagation->children[0]));left->children.erase(left->children.begin());propagation->children.clear();propagation->add(std::move(left));left=std::move(propagation);}
    } else left = postfix(primary(multiline), multiline);
    std::size_t operators = 0;
    for (;;) {
        if (multiline) newlines();
        int power = precedence(peek().kind);
        if (power < minimum) break;
        if (++operators > 192) fail(peek(), "expression chain limit exceeded", "E2008");
        auto op = take();
        if (power == 1 && !assignable(*left)) fail(op, "assignment requires a name, member or index", "E2007");
        auto node = std::make_unique<Node>(power == 1 ? N::Assignment : N::Binary, left->span, op.text);
        node->add(std::move(left)); newlines();
        node->add(expression(power == 1 || op.kind == K::Power ? power : power + 1, multiline));
        finish(*node); left = std::move(node);
    }
    return left;
}
}
