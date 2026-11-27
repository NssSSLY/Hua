#include "hua/regex.hpp"
#include "hua/stream.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace hua {
namespace {
constexpr std::size_t kMaxPatternBytes = 4096;
constexpr std::size_t kMaxNfaStates = 4096;
constexpr std::size_t kMaxTextBytes = 16 * 1024 * 1024;
constexpr std::size_t kMaxMatches = 1000000;

enum class NfaOp : std::uint8_t {
    Match,
    Byte,
    Any,       // any byte except '\n'
    Class,     // 256-bit bitmap
    Bol,       // ^ (start of text)
    Eol,       // $ (end of text)
    Jmp,
    Split,     // prefers out1 over out2 (leftmost-greedy)
};

struct NfaInst {
    NfaOp op{NfaOp::Match};
    std::uint8_t byte{};
    int out1{-1};
    int out2{-1};
    int class_idx{-1};
};

struct CharClass {
    std::array<std::uint64_t, 4> bits{};
    void set(unsigned char c) { bits[c >> 6] |= (std::uint64_t{1} << (c & 63)); }
    bool test(unsigned char c) const { return (bits[c >> 6] & (std::uint64_t{1} << (c & 63))) != 0; }
    void invert() { for (auto& w : bits) w = ~w; }
};

void add_shorthand(CharClass& cc, char code) {
    bool neg = (code == 'D' || code == 'W' || code == 'S');
    char base = static_cast<char>(std::tolower(static_cast<unsigned char>(code)));
    CharClass tmp;
    auto add_range = [&](int lo, int hi) {
        for (int c = lo; c <= hi; ++c) tmp.set(static_cast<unsigned char>(c));
    };
    if (base == 'd') {
        add_range('0', '9');
    } else if (base == 'w') {
        add_range('0', '9');
        add_range('A', 'Z');
        add_range('a', 'z');
        tmp.set('_');
    } else if (base == 's') {
        tmp.set(' '); tmp.set('\t'); tmp.set('\n'); tmp.set('\r'); tmp.set('\f'); tmp.set('\v');
    }
    if (neg) tmp.invert();
    for (int i = 0; i < 4; ++i) cc.bits[i] |= tmp.bits[i];
}
} // namespace

struct RegexData {
    std::string source;
    std::vector<NfaInst> code;
    std::vector<CharClass> classes;
    int start{0};
};

namespace {
struct PatchTarget {
    int inst{};
    bool second{};
};
struct NfaFrag {
    int start{-1};
    std::vector<PatchTarget> outs;
};

class Compiler {
    std::string_view pat_;
    std::size_t pos_{0};
    int depth_{0};
    std::vector<NfaInst> code_;
    std::vector<CharClass> classes_;
    std::string err_;

    bool fail(std::string msg) {
        if (err_.empty()) err_ = std::move(msg);
        return false;
    }
    int emit(NfaInst inst) {
        if (code_.size() >= kMaxNfaStates) {
            fail("regex exceeds maximum NFA state limit");
            return -1;
        }
        int id = static_cast<int>(code_.size());
        code_.push_back(inst);
        return id;
    }
    void patch(const std::vector<PatchTarget>& outs, int target) {
        for (const auto& p : outs) {
            if (p.inst < 0 || p.inst >= static_cast<int>(code_.size())) continue;
            if (p.second) code_[p.inst].out2 = target;
            else code_[p.inst].out1 = target;
        }
    }
    NfaFrag concat(NfaFrag a, NfaFrag b) {
        if (a.start < 0) return b;
        if (b.start < 0) return a;
        patch(a.outs, b.start);
        return {a.start, std::move(b.outs)};
    }
    NfaFrag make_empty() {
        int id = emit({NfaOp::Jmp, 0, -1, -1, -1});
        if (id < 0) return {};
        return {id, {{id, false}}};
    }
    NfaFrag clone_frag(const NfaFrag& f, std::vector<NfaInst>& snap, int base_start) {
        // Re-parse is simpler for {m,n}: we use substring re-parse for bounded repetition.
        (void)f; (void)snap; (void)base_start;
        return {};
    }

    bool parse_escape_byte(unsigned char& out, bool& is_class, char& class_code) {
        is_class = false;
        if (pos_ >= pat_.size()) return fail("trailing backslash in regex");
        char c = pat_[pos_++];
        switch (c) {
            case 'n': out = '\n'; return true;
            case 'r': out = '\r'; return true;
            case 't': out = '\t'; return true;
            case 'f': out = '\f'; return true;
            case 'v': out = '\v'; return true;
            case 'd': case 'D': case 'w': case 'W': case 's': case 'S':
                is_class = true; class_code = c; return true;
            default:
                out = static_cast<unsigned char>(c);
                return true;
        }
    }

    std::optional<NfaFrag> parse_class() {
        CharClass cc;
        bool negate = false;
        if (pos_ < pat_.size() && pat_[pos_] == '^') {
            negate = true;
            ++pos_;
        }
        bool first = true;
        while (pos_ < pat_.size()) {
            if (!first && pat_[pos_] == ']') {
                ++pos_;
                if (negate) cc.invert();
                int cidx = static_cast<int>(classes_.size());
                classes_.push_back(cc);
                int id = emit({NfaOp::Class, 0, -1, -1, cidx});
                if (id < 0) return std::nullopt;
                return NfaFrag{id, {{id, false}}};
            }
            first = false;
            unsigned char ch = 0;
            bool is_sh = false;
            char sh_code = 0;
            if (pat_[pos_] == '\\') {
                ++pos_;
                if (!parse_escape_byte(ch, is_sh, sh_code)) return std::nullopt;
                if (is_sh) {
                    add_shorthand(cc, sh_code);
                    continue;
                }
            } else {
                ch = static_cast<unsigned char>(pat_[pos_++]);
            }
            if (pos_ + 1 < pat_.size() && pat_[pos_] == '-' && pat_[pos_ + 1] != ']') {
                ++pos_; // skip '-'
                unsigned char hi = 0;
                bool hi_sh = false;
                char hi_code = 0;
                if (pat_[pos_] == '\\') {
                    ++pos_;
                    if (!parse_escape_byte(hi, hi_sh, hi_code)) return std::nullopt;
                    if (hi_sh) { fail("shorthand class cannot be range endpoint"); return std::nullopt; }
                } else {
                    hi = static_cast<unsigned char>(pat_[pos_++]);
                }
                if (hi < ch) { fail("invalid character range in regex class"); return std::nullopt; }
                for (int c = ch; c <= hi; ++c) cc.set(static_cast<unsigned char>(c));
            } else {
                cc.set(ch);
            }
        }
        fail("unterminated character class '[' in regex");
        return std::nullopt;
    }

    std::optional<NfaFrag> parse_atom() {
        if (pos_ >= pat_.size()) return std::nullopt;
        char c = pat_[pos_];
        if (c == '(') {
            ++pos_;
            if (++depth_ > 64) { fail("regex group nesting limit exceeded"); return std::nullopt; }
            auto inner = parse_alt();
            --depth_;
            if (!inner) return std::nullopt;
            if (pos_ >= pat_.size() || pat_[pos_] != ')') { fail("unclosed '(' in regex"); return std::nullopt; }
            ++pos_;
            return inner;
        }
        if (c == '[') {
            ++pos_;
            return parse_class();
        }
        if (c == '.') {
            ++pos_;
            int id = emit({NfaOp::Any, 0, -1, -1, -1});
            if (id < 0) return std::nullopt;
            return NfaFrag{id, {{id, false}}};
        }
        if (c == '^') {
            ++pos_;
            int id = emit({NfaOp::Bol, 0, -1, -1, -1});
            if (id < 0) return std::nullopt;
            return NfaFrag{id, {{id, false}}};
        }
        if (c == '$') {
            ++pos_;
            int id = emit({NfaOp::Eol, 0, -1, -1, -1});
            if (id < 0) return std::nullopt;
            return NfaFrag{id, {{id, false}}};
        }
        if (c == '\\') {
            ++pos_;
            unsigned char b = 0;
            bool is_sh = false;
            char sh_code = 0;
            if (!parse_escape_byte(b, is_sh, sh_code)) return std::nullopt;
            if (is_sh) {
                CharClass cc;
                add_shorthand(cc, sh_code);
                int cidx = static_cast<int>(classes_.size());
                classes_.push_back(cc);
                int id = emit({NfaOp::Class, 0, -1, -1, cidx});
                if (id < 0) return std::nullopt;
                return NfaFrag{id, {{id, false}}};
            }
            int id = emit({NfaOp::Byte, b, -1, -1, -1});
            if (id < 0) return std::nullopt;
            return NfaFrag{id, {{id, false}}};
        }
        if (c == '*' || c == '+' || c == '?') {
            fail("quantifier without preceding atom");
            return std::nullopt;
        }
        ++pos_;
        int id = emit({NfaOp::Byte, static_cast<unsigned char>(c), -1, -1, -1});
        if (id < 0) return std::nullopt;
        return NfaFrag{id, {{id, false}}};
    }

    std::optional<NfaFrag> parse_quantified() {
        std::size_t atom_start = pos_;
        auto atom = parse_atom();
        if (!atom) return std::nullopt;
        std::size_t atom_end = pos_;

        while (pos_ < pat_.size()) {
            char q = pat_[pos_];
            if (q == '*') {
                ++pos_;
                int sp = emit({NfaOp::Split, 0, atom->start, -1, -1});
                if (sp < 0) return std::nullopt;
                patch(atom->outs, sp);
                atom = NfaFrag{sp, {{sp, true}}};
            } else if (q == '+') {
                ++pos_;
                int sp = emit({NfaOp::Split, 0, atom->start, -1, -1});
                if (sp < 0) return std::nullopt;
                patch(atom->outs, sp);
                atom = NfaFrag{atom->start, {{sp, true}}};
            } else if (q == '?') {
                ++pos_;
                int sp = emit({NfaOp::Split, 0, atom->start, -1, -1});
                if (sp < 0) return std::nullopt;
                auto outs = std::move(atom->outs);
                outs.push_back({sp, true});
                atom = NfaFrag{sp, std::move(outs)};
            } else if (q == '{') {
                std::size_t save = pos_++;
                int lo = 0, hi = -1;
                bool has_lo = false, has_comma = false, has_hi = false;
                while (pos_ < pat_.size() && std::isdigit(static_cast<unsigned char>(pat_[pos_]))) {
                    has_lo = true;
                    lo = lo * 10 + (pat_[pos_++] - '0');
                    if (lo > 100) { fail("regex repetition bound exceeds 100"); return std::nullopt; }
                }
                if (pos_ < pat_.size() && pat_[pos_] == ',') {
                    has_comma = true;
                    ++pos_;
                    hi = 0;
                    while (pos_ < pat_.size() && std::isdigit(static_cast<unsigned char>(pat_[pos_]))) {
                        has_hi = true;
                        hi = hi * 10 + (pat_[pos_++] - '0');
                        if (hi > 100) { fail("regex repetition bound exceeds 100"); return std::nullopt; }
                    }
                }
                if (!has_lo || pos_ >= pat_.size() || pat_[pos_] != '}') {
                    pos_ = save;
                    break;
                }
                ++pos_; // consume '}'
                if (!has_comma) hi = lo;
                else if (!has_hi) hi = -1; // unbounded {lo,}
                else if (hi < lo) { fail("regex repetition {m,n} has n < m"); return std::nullopt; }

                std::size_t after_q = pos_;
                auto reparse_atom = [&]() -> std::optional<NfaFrag> {
                    std::size_t cur_save = pos_;
                    pos_ = atom_start;
                    auto f = parse_atom();
                    pos_ = cur_save;
                    return f;
                };
                NfaFrag res = make_empty();
                if (!err_.empty()) return std::nullopt;
                for (int i = 0; i < lo; ++i) {
                    auto f = (i == 0) ? atom : reparse_atom();
                    if (!f) return std::nullopt;
                    res = concat(std::move(res), std::move(*f));
                }
                if (hi < 0) {
                    auto f = (lo == 0) ? atom : reparse_atom();
                    if (!f) return std::nullopt;
                    int sp = emit({NfaOp::Split, 0, f->start, -1, -1});
                    if (sp < 0) return std::nullopt;
                    patch(f->outs, sp);
                    res = concat(std::move(res), NfaFrag{sp, {{sp, true}}});
                } else {
                    for (int i = lo; i < hi; ++i) {
                        auto f = (i == 0) ? atom : reparse_atom();
                        if (!f) return std::nullopt;
                        int sp = emit({NfaOp::Split, 0, f->start, -1, -1});
                        if (sp < 0) return std::nullopt;
                        auto outs = std::move(f->outs);
                        outs.push_back({sp, true});
                        res = concat(std::move(res), NfaFrag{sp, std::move(outs)});
                    }
                }
                pos_ = after_q;
                atom = std::move(res);
                (void)atom_end;
            } else {
                break;
            }
        }
        return atom;
    }

    std::optional<NfaFrag> parse_seq() {
        NfaFrag res;
        bool any = false;
        while (pos_ < pat_.size() && pat_[pos_] != '|' && pat_[pos_] != ')') {
            auto f = parse_quantified();
            if (!f) return std::nullopt;
            res = any ? concat(std::move(res), std::move(*f)) : std::move(*f);
            any = true;
        }
        if (!any) return make_empty();
        return res;
    }

    std::optional<NfaFrag> parse_alt() {
        auto left = parse_seq();
        if (!left) return std::nullopt;
        while (pos_ < pat_.size() && pat_[pos_] == '|') {
            ++pos_;
            auto right = parse_seq();
            if (!right) return std::nullopt;
            int sp = emit({NfaOp::Split, 0, left->start, right->start, -1});
            if (sp < 0) return std::nullopt;
            auto outs = std::move(left->outs);
            outs.insert(outs.end(), right->outs.begin(), right->outs.end());
            left = NfaFrag{sp, std::move(outs)};
        }
        return left;
    }

public:
    explicit Compiler(std::string_view p) : pat_(p) {}
    std::shared_ptr<RegexData> compile(std::string& err_out) {
        if (pat_.size() > kMaxPatternBytes) {
            err_out = "regex pattern exceeds 4096 bytes";
            return nullptr;
        }
        auto frag = parse_alt();
        if (!frag || !err_.empty()) {
            err_out = err_.empty() ? "invalid regex pattern" : err_;
            return nullptr;
        }
        if (pos_ != pat_.size()) {
            err_out = "unexpected ')' in regex pattern";
            return nullptr;
        }
        int match_id = emit({NfaOp::Match, 0, -1, -1, -1});
        if (match_id < 0) {
            err_out = err_;
            return nullptr;
        }
        patch(frag->outs, match_id);
        auto data = std::make_shared<RegexData>();
        data->source = std::string(pat_);
        data->code = std::move(code_);
        data->classes = std::move(classes_);
        data->start = frag->start;
        return data;
    }
};

struct MatchSpan {
    std::size_t start{};
    std::size_t end{};
};

// Linear-time leftmost-longest Thompson NFA search starting at or after `from`.
std::optional<MatchSpan> find_from(const RegexData& re, std::string_view text, std::size_t from) {
    const std::size_t n_states = re.code.size();
    std::vector<int> cur, next;
    std::vector<std::uint32_t> visited(n_states, 0);
    std::uint32_t gen = 1;

    auto add_state = [&](auto&& self, std::vector<int>& list, int s, std::size_t pos) -> void {
        if (s < 0 || visited[s] == gen) return;
        visited[s] = gen;
        const auto& inst = re.code[s];
        if (inst.op == NfaOp::Jmp) {
            self(self, list, inst.out1, pos);
        } else if (inst.op == NfaOp::Split) {
            self(self, list, inst.out1, pos);
            self(self, list, inst.out2, pos);
        } else if (inst.op == NfaOp::Bol) {
            if (pos == 0) self(self, list, inst.out1, pos);
        } else if (inst.op == NfaOp::Eol) {
            if (pos == text.size()) self(self, list, inst.out1, pos);
        } else {
            list.push_back(s);
        }
    };

    for (std::size_t start_pos = from; start_pos <= text.size(); ++start_pos) {
        cur.clear();
        ++gen;
        add_state(add_state, cur, re.start, start_pos);
        std::optional<std::size_t> best_end;
        for (std::size_t pos = start_pos; pos <= text.size(); ++pos) {
            for (int s : cur) {
                if (re.code[s].op == NfaOp::Match) {
                    best_end = pos;
                    break;
                }
            }
            if (pos == text.size() || cur.empty()) break;
            unsigned char ch = static_cast<unsigned char>(text[pos]);
            next.clear();
            ++gen;
            for (int s : cur) {
                const auto& inst = re.code[s];
                bool ok = false;
                if (inst.op == NfaOp::Byte) ok = (ch == inst.byte);
                else if (inst.op == NfaOp::Any) ok = (ch != '\n');
                else if (inst.op == NfaOp::Class) ok = re.classes[inst.class_idx].test(ch);
                if (ok) add_state(add_state, next, inst.out1, pos + 1);
            }
            cur.swap(next);
        }
        if (best_end) return MatchSpan{start_pos, *best_end};
    }
    return std::nullopt;
}

Value make_string_slice(const std::vector<std::string>& items) {
    auto storage = managed<std::vector<Value>>();
    storage->reserve(items.size());
    for (const auto& s : items) storage->emplace_back(s);
    return Value(SliceValue{std::move(storage), 0, items.size(), false, "string"});
}
} // namespace

Value invoke_regex_standard(const StandardFunction& function, const std::vector<Value>& args,
                            const SourceSpan& span) {
    const auto& op = function.name;
    if (op == "compile") {
        auto* pat = string_data(&args.at(0).data);
        if (!pat) runtime_error(span, "std.regex.compile expects string", "E4003");
        std::string err;
        auto data = Compiler(*pat).compile(err);
        if (!data) {
            return stream_error_result({"regex", "syntax", err, "", {}}, "std.regex.Pattern", "std.regex.compile");
        }
        return Value(ResultValue{true, result_box(Value(RegexValue{std::move(data)})),
                                 "std.regex.Pattern", "std.error.Value", true, false});
    }
    auto* rv = get_if<RegexValue>(&args.at(0).data);
    auto* text = string_data(&args.at(1).data);
    if (!rv || !rv->data || !text) runtime_error(span, "std.regex." + op + " expects (Pattern, string, ...)", "E4003");
    if (text->size() > kMaxTextBytes) runtime_error(span, "regex input exceeds 16 MiB", "E4099");
    const auto& re = *rv->data;

    if (op == "is_match") {
        return Value(find_from(re, *text, 0).has_value());
    }
    if (op == "find") {
        auto m = find_from(re, *text, 0);
        if (!m) return Value{};
        return Value(std::string(text->substr(m->start, m->end - m->start)));
    }
    if (op == "find_all") {
        auto* lim = get_if<std::int64_t>(&args.at(2).data);
        if (!lim) runtime_error(span, "std.regex.find_all expects (Pattern, string, int)", "E4003");
        std::size_t cap = (*lim <= 0) ? kMaxMatches : static_cast<std::size_t>(std::min<std::int64_t>(*lim, static_cast<std::int64_t>(kMaxMatches)));
        std::vector<std::string> out;
        std::size_t pos = 0;
        while (pos <= text->size() && out.size() < cap) {
            auto m = find_from(re, *text, pos);
            if (!m) break;
            out.emplace_back(text->substr(m->start, m->end - m->start));
            pos = (m->end > m->start) ? m->end : (m->end + 1);
        }
        return make_string_slice(out);
    }
    if (op == "replace_all") {
        auto* repl = string_data(&args.at(2).data);
        if (!repl) runtime_error(span, "std.regex.replace_all expects (Pattern, string, string)", "E4003");
        std::string out;
        std::size_t pos = 0;
        while (pos <= text->size()) {
            auto m = find_from(re, *text, pos);
            if (!m) {
                out.append(text->substr(pos));
                break;
            }
            out.append(text->substr(pos, m->start - pos));
            out.append(*repl);
            if (out.size() > kMaxTextBytes) runtime_error(span, "regex replace_all output exceeds 16 MiB", "E4099");
            if (m->end > m->start) {
                pos = m->end;
            } else {
                if (m->end < text->size()) out.push_back((*text)[m->end]);
                pos = m->end + 1;
            }
        }
        return Value(std::move(out));
    }
    if (op == "split") {
        auto* lim = get_if<std::int64_t>(&args.at(2).data);
        if (!lim) runtime_error(span, "std.regex.split expects (Pattern, string, int)", "E4003");
        std::size_t cap = (*lim <= 0) ? kMaxMatches : static_cast<std::size_t>(std::min<std::int64_t>(*lim, static_cast<std::int64_t>(kMaxMatches)));
        std::vector<std::string> out;
        std::size_t pos = 0;
        while (pos <= text->size()) {
            if (out.size() + 1 >= cap) {
                out.emplace_back(text->substr(pos));
                break;
            }
            auto m = find_from(re, *text, pos);
            if (!m) {
                out.emplace_back(text->substr(pos));
                break;
            }
            out.emplace_back(text->substr(pos, m->start - pos));
            pos = (m->end > m->start) ? m->end : (m->end + 1);
            if (m->end == m->start && pos > text->size()) break;
        }
        return make_string_slice(out);
    }
    runtime_error(span, "unknown std.regex function `" + op + "`", "E4001");
}
} // namespace hua
