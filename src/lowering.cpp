#include "hua/lowering.hpp"
#include "hua/concurrency_lowering.hpp"
#include "hua/value.hpp"
#include <functional>
#include <map>
#include <set>
namespace hua {
namespace {
using N = NodeKind;
NodePtr clone(const Node &n) {
  auto out = std::make_unique<Node>(n.kind, n.span, n.text);
  for (const auto &c : n.children)
    out->add(clone(*c));
  return out;
}
[[noreturn]] void fail(const Node &n, const std::string &message) {
  throw Diagnostic("E3010", n.span, message);
}
std::string path(const Node &n) {
  if (n.kind == N::Name)
    return n.text;
  if (n.kind == N::Member)
    return path(*n.children[0]) + "." + n.text;
  return {};
}
struct Lower {
  Node &program;
  std::size_t serial{};
  explicit Lower(Node &root) : program(root) {}
  std::map<std::string, const Node *> templates;
  std::map<std::string, std::string> instances;
  std::vector<NodePtr> generated;
  std::vector<std::map<std::string, std::string>> names, types;
  std::string unique(const std::string &name) {
    return "$local" + std::to_string(++serial) + "$" + name;
  }
  std::string resolve(const std::string &name, const auto &scopes) {
    for (auto i = scopes.rbegin(); i != scopes.rend(); ++i)
      if (auto f = i->find(name); f != i->end())
        return f->second;
    return name;
  }
  std::string instantiate(const Node &site, const std::string &name,
                          const std::vector<const Node *> &args) {
    auto found = templates.find(name);
    if (found == templates.end())
      fail(site, "unknown user generic `" + name + "`");
    auto &parameters = *found->second->children[0];
    if (parameters.children.size() != args.size())
      fail(site, "generic type argument count mismatch");
    std::string key = name + "<";
    for (auto arg : args)
      key += type_name(*arg) + ",";
    key += '>';
    if (auto f = instances.find(key); f != instances.end())
      return f->second;
    if (instances.size() >= 128)
      fail(site, "generic instantiation limit exceeded (128)");
    auto result = "$generic" + std::to_string(instances.size()) + "$" + name;
    instances[key] = result;
    auto instance = clone(*found->second);
    instance->children.erase(instance->children.begin());
    instance->text = result + found->second->text.substr(name.size());
    std::function<void(Node &)> substitute = [&](Node &n) {
      for (std::size_t i = 0; i < args.size(); ++i)
        if (n.text == parameters.children[i]->text) {
          if (n.kind == N::TypeName) {
            auto copy = clone(*args[i]);
            n.kind = copy->kind;
            n.text = copy->text;
            n.children = std::move(copy->children);
            break;
          }
          if (n.kind == N::StructLiteral || n.kind == N::Name)
            n.text = type_name(*args[i]);
        }
      for (auto &c : n.children)
        substitute(*c);
    };
    substitute(*instance);
    specialize(*instance);
    generated.push_back(std::move(instance));
    return result;
  }
  void specialize(Node &n) {
    for (auto &c : n.children)
      specialize(*c);
    if (n.kind == N::GenericType && templates.contains(n.text)) {
      std::vector<const Node *> args;
      for (const auto &c : n.children)
        args.push_back(c.get());
      n.text = instantiate(n, n.text, args);
      n.kind = N::TypeName;
      n.children.clear();
    } else if (n.kind == N::Specialize) {
      auto name = path(*n.children[0]);
      std::vector<const Node *> args;
      for (std::size_t i = 1; i < n.children.size(); ++i)
        args.push_back(n.children[i].get());
      n.text = instantiate(n, name, args);
      n.kind = N::Name;
      n.children.clear();
    } else if (n.kind == N::StructLiteral && !n.children.empty() &&
               n.children[0]->kind == N::TypeName) {
      n.text = n.children[0]->text;
      n.children.erase(n.children.begin());
    }
  }
  void lexical(Node &n, bool top = false) {
    if (n.kind == N::Program || n.kind == N::Block) {
      names.emplace_back();
      types.emplace_back();
      std::set<std::string> declared;
      for (auto &c : n.children) {
        if (c->kind == N::Function || c->kind == N::Struct ||
            c->kind == N::Let || c->kind == N::Var || c->kind == N::Const) {
          auto name = declared_name(*c);
          if (!declared.insert(name).second)
            throw Diagnostic("E3002", c->span,
                             "duplicate declaration `" + name + "`");
        }
        lexical(*c, n.kind == N::Program);
      }
      names.pop_back();
      types.pop_back();
      return;
    }
    if (n.kind == N::For) {
      auto count = n.children.size() - 2;
      lexical(*n.children[count]);
      names.emplace_back();
      types.emplace_back();
      for (std::size_t i = 0; i < count; ++i)
        names.back()[n.children[i]->text] = n.children[i]->text;
      lexical(*n.children.back());
      names.pop_back();
      types.pop_back();
      return;
    }
    if (n.kind == N::MatchArm) {
      lexical(*n.children[0]);
      names.emplace_back();
      types.emplace_back();
      for (const auto &p : n.children[1]->children)
        names.back()[p->text] = p->text;
      lexical(*n.children[2]);
      names.pop_back();
      types.pop_back();
      return;
    }
    if (n.kind == N::Function) {
      auto old = declared_name(n);
      if (!top) {
        auto replacement = unique(old);
        names.back()[old] = replacement;
        n.text = replacement + n.text.substr(old.size());
      }
      names.emplace_back();
      types.emplace_back();
      for (const auto &c : n.children)
        if (c->kind == N::Parameter)
          names.back()[declared_name(*c)] = declared_name(*c);
      for (auto &c : n.children)
        lexical(*c);
      names.pop_back();
      types.pop_back();
      return;
    }
    if (n.kind == N::Struct && !top) {
      auto old = declared_name(n), replacement = unique(old);
      types.back()[old] = replacement;
      n.text = replacement + n.text.substr(old.size());
      for (auto &c : n.children)
        lexical(*c);
      generated.push_back(clone(n));
      n.kind = N::Block;
      n.children.clear();
      n.text.clear();
      return;
    }
    if (n.kind == N::Name)
      n.text = resolve(n.text, names);
    if (n.kind == N::TypeName || n.kind == N::StructLiteral)
      n.text = resolve(n.text, types);
    for (auto &c : n.children)
      lexical(*c);
    if (n.kind == N::Let || n.kind == N::Var || n.kind == N::Const)
      names.back()[n.text] = n.text;
  }
  void run() {
    for (const auto &n : program.children)
      if (!n->children.empty() && n->children[0]->kind == N::GenericParameters)
        templates.emplace(declared_name(*n), n.get());
    for (auto &n : program.children)
      if (!templates.contains(declared_name(*n)))
        specialize(*n);
    std::vector<NodePtr> kept;
    for (auto &n : program.children)
      if (!templates.contains(declared_name(*n)))
        kept.push_back(std::move(n));
    for (auto &n : generated)
      kept.push_back(std::move(n));
    generated.clear();
    program.children = std::move(kept);
    lower_concurrency(program);
    lexical(program, true);
    for (auto &n : generated)
      program.children.push_back(std::move(n));
    const Node *entry = nullptr;
    bool main = false;
    for (const auto &n : program.children)
      if (n->kind == N::Function) {
        main |= declared_name(*n) == "main";
        if (n->text.find(" @entry") != std::string::npos) {
          if (entry)
            fail(*n, "multiple entry attributes");
          if (declared_name(*n).find('$') != std::string::npos)
            fail(*n, "entry is allowed only in the entry module");
          entry = n.get();
        }
      }
    if (entry && declared_name(*entry) != "main") {
      if (main)
        fail(*entry, "entry attribute conflicts with main");
      for (const auto &c : entry->children)
        if (c->kind == N::Parameter)
          fail(*entry, "entry function requires zero parameters");
      auto fn = std::make_unique<Node>(N::Function, entry->span, "main"),
           body = std::make_unique<Node>(N::Block, entry->span),
           statement =
               std::make_unique<Node>(N::ExpressionStatement, entry->span),
           call = std::make_unique<Node>(N::Call, entry->span);
      call->add(
          std::make_unique<Node>(N::Name, entry->span, declared_name(*entry)));
      statement->add(std::move(call));
      body->add(std::move(statement));
      fn->add(std::move(body));
      program.add(std::move(fn));
    }
  }
};
} // namespace
void lower_declarations(Node &program) {
  std::vector<NodePtr> out;
  for (auto &n : program.children) {
    if (n->kind == N::Interface) {
      auto structure =
          std::make_unique<Node>(N::Struct, n->span, n->text + " interface");
      for (auto &method : n->children)
        out.push_back(std::move(method));
      out.push_back(std::move(structure));
    } else if (n->kind == N::Enum) {
      auto name = declared_name(*n);
      auto structure =
          std::make_unique<Node>(N::Struct, n->span, n->text + " enum");
      std::set<std::string> cases;
      auto tag = std::make_unique<Node>(N::Field, n->span, "$case");
      tag->add(std::make_unique<Node>(N::TypeName, n->span, "string"));
      structure->add(std::move(tag));
      for (const auto &variant : n->children) {
        if (!cases.insert(variant->text).second)
          fail(*variant, "duplicate enum variant");
        auto size = std::max<std::size_t>(1, variant->children.size());
        for (std::size_t i = 0; i < size; ++i) {
          auto field = std::make_unique<Node>(
              N::Field, variant->span,
              "$" + variant->text + "#" + std::to_string(i) + "#" +
                  (variant->children.empty()
                       ? "void"
                       : type_name(*variant->children[i])));
          field->add(std::make_unique<Node>(N::TypeName, variant->span, ""));
          if (!variant->children.empty())
            field->add(clone(*variant->children[i]));
          structure->add(std::move(field));
        }
      }
      if (cases.empty())
        fail(*n, "enum requires at least one variant");
      for (const auto &variant : n->children) {
        auto fn = std::make_unique<Node>(
            N::Function, variant->span,
            name + "." + variant->text +
                (n->text.find(" pub") != std::string::npos ? " pub" : ""));
        for (std::size_t i = 0; i < variant->children.size(); ++i) {
          auto param = std::make_unique<Node>(N::Parameter, variant->span,
                                              "arg" + std::to_string(i));
          param->add(clone(*variant->children[i]));
          fn->add(std::move(param));
        }
        auto returns = std::make_unique<Node>(N::ReturnTypes, variant->span);
        returns->add(std::make_unique<Node>(N::TypeName, variant->span, name));
        fn->add(std::move(returns));
        auto body = std::make_unique<Node>(N::Block, variant->span),
             ret = std::make_unique<Node>(N::Return, variant->span),
             value =
                 std::make_unique<Node>(N::StructLiteral, variant->span, name);
        for (const auto &field : structure->children) {
          auto init =
              std::make_unique<Node>(N::FieldInit, variant->span, field->text);
          if (field->text == "$case")
            init->add(std::make_unique<Node>(N::String, variant->span,
                                             variant->text));
          else if (field->text.starts_with("$" + variant->text + "#") &&
                   !variant->children.empty()) {
            auto start = field->text.find('#') + 1,
                 end = field->text.find('#', start);
            init->add(std::make_unique<Node>(
                N::Name, variant->span,
                "arg" + field->text.substr(start, end - start)));
          } else
            init->add(std::make_unique<Node>(N::Nil, variant->span));
          value->add(std::move(init));
        }
        ret->add(std::move(value));
        body->add(std::move(ret));
        fn->add(std::move(body));
        out.push_back(std::move(fn));
      }
      out.push_back(std::move(structure));
    } else
      out.push_back(std::move(n));
  }
  program.children = std::move(out);
}
void lower_language(Node &program) {Lower{program}.run();}
} // namespace hua
