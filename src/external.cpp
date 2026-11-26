#include "hua/external.hpp"
#include "hua/native.h"
#include <wasmtime.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <limits>
#include <unordered_set>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif
namespace hua {
struct ExternalRegistry::HandleTable {
    struct Slot {
        std::uint32_t generation{1};
        std::uint32_t module_token{};
        std::uint32_t type_tag{};
        std::uint8_t state{0}; // 0=Free, 1=Open, 2=Closed, 3=Poisoned
        void* payload{};
        hua_handle_destructor destructor{};
        std::uint64_t created_seq{};
    };
    std::mutex mutex;
    std::vector<Slot> slots;
    std::uint64_t next_seq{1};

    NativeHandleValue create(std::uint32_t module_token, std::uint32_t type_tag, void* payload,
                             hua_handle_destructor destructor) {
        std::lock_guard<std::mutex> lock(mutex);
        std::uint32_t idx = static_cast<std::uint32_t>(slots.size());
        if (slots.size() >= 65536) {
            for (std::uint32_t i = 0; i < slots.size(); ++i) {
                if (slots[i].state == 2) {
                    idx = i;
                    break;
                }
            }
        }
        if (idx == slots.size()) {
            if (slots.size() >= 131072) return {};
            slots.push_back(Slot{});
        } else {
            slots[idx].generation += 1;
            if (slots[idx].generation == 0) slots[idx].generation = 1;
        }
        auto& s = slots[idx];
        s.module_token = module_token;
        s.type_tag = type_tag;
        s.state = 1;
        s.payload = payload;
        s.destructor = destructor;
        s.created_seq = next_seq++;
        return NativeHandleValue{idx + 1, s.generation, module_token, type_tag};
    }

    int get(const NativeHandleValue& h, std::uint32_t module_token, std::uint32_t expected_type_tag,
            void** out_payload) {
        std::lock_guard<std::mutex> lock(mutex);
        if (h.slot == 0 || h.slot > slots.size()) return HUA_HANDLE_INVALID;
        const auto& s = slots[h.slot - 1];
        if (s.generation != h.generation || s.module_token != module_token ||
            h.module_token != module_token || s.type_tag != expected_type_tag ||
            h.type_tag != expected_type_tag) {
            return HUA_HANDLE_INVALID;
        }
        if (s.state == 2) return HUA_HANDLE_CLOSED;
        if (s.state == 3) return HUA_HANDLE_POISONED;
        if (s.state != 1) return HUA_HANDLE_INVALID;
        if (out_payload) *out_payload = s.payload;
        return HUA_HANDLE_OK;
    }

    int close(const NativeHandleValue& h, std::uint32_t module_token, std::uint32_t expected_type_tag,
              hua_handle_closer closer, char* err_buf, std::size_t err_cap) {
        void* payload = nullptr;
        hua_handle_destructor dtor = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (h.slot == 0 || h.slot > slots.size()) return HUA_HANDLE_INVALID;
            auto& s = slots[h.slot - 1];
            if (s.generation != h.generation || s.module_token != module_token ||
                h.module_token != module_token || s.type_tag != expected_type_tag ||
                h.type_tag != expected_type_tag) {
                return HUA_HANDLE_INVALID;
            }
            if (s.state == 2) return HUA_HANDLE_OK; // Idempotent repeat close
            if (s.state == 3) return HUA_HANDLE_POISONED;
            if (s.state != 1) return HUA_HANDLE_INVALID;
            payload = s.payload;
            dtor = s.destructor;
            s.state = 2;
            s.payload = nullptr;
            s.destructor = nullptr;
        }
        if (closer) {
            if (err_buf && err_cap > 0) err_buf[0] = '\0';
            int rc = closer(payload, err_buf, err_cap);
            if (rc != 0) {
                std::lock_guard<std::mutex> lock(mutex);
                if (h.slot > 0 && h.slot <= slots.size() && slots[h.slot - 1].generation == h.generation) {
                    slots[h.slot - 1].state = 3;
                }
                return HUA_HANDLE_POISONED;
            }
            return HUA_HANDLE_OK;
        }
        if (dtor) dtor(payload);
        return HUA_HANDLE_OK;
    }

    void destroy_all() {
        struct Pending {
            std::uint64_t seq;
            void* payload;
            hua_handle_destructor destructor;
        };
        std::vector<Pending> pending;
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (auto& s : slots) {
                if (s.state == 1) {
                    if (s.destructor) pending.push_back({s.created_seq, s.payload, s.destructor});
                    s.state = 2;
                    s.payload = nullptr;
                    s.destructor = nullptr;
                }
            }
        }
        std::sort(pending.begin(), pending.end(), [](const Pending& a, const Pending& b) {
            return a.seq > b.seq; // LIFO: child handles (e.g. Stmt) destroyed before parent (Conn)
        });
        for (const auto& item : pending) {
            try {
                item.destructor(item.payload);
            } catch (...) {
            }
        }
    }
};

namespace {
[[noreturn]] void fail(const SourceSpan& s, std::string msg, std::string code = "E7003") {
    runtime_error(s, std::move(msg), std::move(code));
}
bool identifier(const std::string& s) {
    if (s.empty() || s.size() > 256) return false;
    auto alpha = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
    if (!alpha(s[0])) return false;
    return std::all_of(s.begin(), s.end(), [&](char c) { return alpha(c) || (c >= '0' && c <= '9'); });
}
std::string error_message(wasmtime_error_t* e) {
    wasm_name_t message;
    wasmtime_error_message(e, &message);
    std::string out(message.data, message.size);
    wasm_name_delete(&message);
    wasmtime_error_delete(e);
    return out;
}
void checked(wasmtime_error_t* e, const SourceSpan& s) {
    if (e) fail(s, "WASM: " + error_message(e));
}
void trapped(wasm_trap_t* trap, const SourceSpan& s) {
    if (!trap) return;
    wasmtime_trap_code_t code{};
    bool fuel = wasmtime_trap_code(trap, &code) && code == WASMTIME_TRAP_CODE_OUT_OF_FUEL;
    wasm_message_t message;
    wasm_trap_message(trap, &message);
    std::string reason(message.data, message.size);
    wasm_byte_vec_delete(&message);
    wasm_trap_delete(trap);
    fail(s, "WASM trap: " + reason, fuel ? "E4099" : "E7004");
}
struct Wasm {
    wasm_engine_t* engine{};
    wasmtime_module_t* module{};
    wasmtime_store_t* store{};
    wasmtime_instance_t instance{};
    ~Wasm() {
        if (store) wasmtime_store_delete(store);
        if (module) wasmtime_module_delete(module);
        if (engine) wasm_engine_delete(engine);
    }
    explicit Wasm(const std::string& bytes, const SourceSpan& s) {
        auto config = wasm_config_new();
        wasmtime_config_consume_fuel_set(config, true);
        engine = wasm_engine_new_with_config(config);
        if (!engine) fail(s, "cannot create WASM engine");
        auto e = wasmtime_module_new(engine, reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), &module);
        if (e) {
            auto message = error_message(e);
            wasm_engine_delete(engine);
            engine = nullptr;
            fail(s, "invalid WASM module: " + message);
        }
    }
    void start(const SourceSpan& s) {
        store = wasmtime_store_new(engine, nullptr, nullptr);
        if (!store) fail(s, "cannot create WASM store");
        wasmtime_store_limiter(store, 64 * 1024 * 1024, 10000, 1, 4, 1);
        auto context = wasmtime_store_context(store);
        checked(wasmtime_context_set_fuel(context, 1000000), s);
        wasm_trap_t* trap = nullptr;
        auto e = wasmtime_instance_new(context, module, nullptr, 0, &instance, &trap);
        if (e) checked(e, s);
        trapped(trap, s);
    }
};
std::string wasm_type(wasm_valkind_t t, const SourceSpan& s) {
    if (t == WASM_I32 || t == WASM_I64) return "int";
    if (t == WASM_F32 || t == WASM_F64) return "float";
    fail(s, "WASM export uses an unsupported reference/vector parameter or result");
}
std::uint32_t native_type_v1(const std::string& t, const SourceSpan& s) {
    if (t == "void") return HUA_VOID;
    if (t == "nil") return HUA_NIL;
    if (t == "bool") return HUA_BOOL;
    if (t == "int") return HUA_INT;
    if (t == "float") return HUA_FLOAT;
    if (t == "string") return HUA_STRING;
    fail(s, "native ABI v1 supports only nil/bool/int/float/string/void signatures");
}
bool native_v2_base(const std::string& t) {
    return t == "nil" || t == "bool" || t == "int" || t == "float" || t == "string" ||
           t == "Bytes" || t == "NativeHandle";
}
bool native_v2_value_type(const std::string& t) {
    if (native_v2_base(t)) return true;
    if (t.size() > 1 && t.ends_with('?')) {
        auto inner = t.substr(0, t.size() - 1);
        return inner != "nil" && native_v2_base(inner);
    }
    return false;
}
void validate_native_v2_type(const std::string& t, bool is_result, const SourceSpan& s) {
    if (is_result && t == "void") return;
    if (native_v2_value_type(t)) return;
    if (is_result) {
        auto args = type_arguments(t, "Result");
        if (args.size() == 2 && args[1] == "std.error.Value" &&
            (args[0] == "void" || native_v2_value_type(args[0]))) {
            return;
        }
    }
    fail(s, "unsupported native ABI v2 type `" + t + "`", "E7003");
}
struct NativeCell {
    Value value;
};
struct NativeCall {
    std::deque<NativeCell> values;
    std::string failure;
    ExternalRegistry::HandleTable* handles{};
    std::uint32_t module_token{};
    hua_value put(Value value) {
        values.push_back({std::move(value)});
        return reinterpret_cast<hua_value>(&values.back());
    }
    Value* get(hua_value h) {
        for (auto& c : values)
            if (reinterpret_cast<hua_value>(&c) == h) return &c.value;
        failure = "invalid or expired native value handle";
        return nullptr;
    }
};
NativeCall& ctx(hua_env e) { return *reinterpret_cast<NativeCall*>(e); }
hua_value n_nil(hua_env e) { return ctx(e).put({}); }
hua_value n_bool(hua_env e, int b) { return ctx(e).put(Value(b != 0)); }
hua_value n_int(hua_env e, std::int64_t x) { return ctx(e).put(Value(x)); }
hua_value n_float(hua_env e, double x) {
    if (!std::isfinite(x)) {
        ctx(e).failure = "native returned a non-finite float";
        return nullptr;
    }
    return ctx(e).put(Value(x));
}
bool string_utf8(std::string_view s) {
    for (std::size_t i = 0; i < s.size();) {
        auto b = static_cast<unsigned char>(s[i++]);
        if (b < 128) continue;
        unsigned n;
        std::uint32_t value, min;
        if (b >= 194 && b <= 223) { n = 1; value = b & 31; min = 128; }
        else if (b >= 224 && b <= 239) { n = 2; value = b & 15; min = 2048; }
        else if (b >= 240 && b <= 244) { n = 3; value = b & 7; min = 65536; }
        else return false;
        if (n > s.size() - i) return false;
        while (n--) {
            auto c = static_cast<unsigned char>(s[i++]);
            if ((c & 192) != 128) return false;
            value = (value << 6) | (c & 63);
        }
        if (value < min || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}
hua_value n_string(hua_env e, const char* p, std::size_t n) {
    if (n > 16 * 1024 * 1024 || (!p && n)) {
        ctx(e).failure = "invalid native string length/pointer";
        return nullptr;
    }
    std::string text(p ? p : "", n);
    if (!string_utf8(text)) {
        ctx(e).failure = "native string is not valid UTF-8";
        return nullptr;
    }
    return ctx(e).put(Value(std::move(text)));
}
hua_value n_bytes(hua_env e, const std::uint8_t* p, std::size_t n) {
    if (n > 16 * 1024 * 1024 || (!p && n)) {
        ctx(e).failure = "invalid native bytes length/pointer";
        return nullptr;
    }
    auto data = std::make_shared<std::string>(p ? reinterpret_cast<const char*>(p) : "", n);
    return ctx(e).put(Value(BytesValue{std::move(data)}));
}
hua_value n_ok_value(hua_env e, hua_value payload) {
    auto* p = ctx(e).get(payload);
    if (!p) return nullptr;
    ResultValue r;
    r.ok = true;
    r.payload = result_box(*p);
    r.writable = true;
    return ctx(e).put(Value(std::move(r)));
}
hua_value n_ok_void(hua_env e) {
    ResultValue r;
    r.ok = true;
    r.success_type = "void";
    r.error_type = "std.error.Value";
    r.writable = true;
    return ctx(e).put(Value(std::move(r)));
}
hua_value n_err_value(hua_env e, const char* domain, const char* code, const char* message,
                      const char* native_domain, int has_native_code, std::int64_t native_code) {
    std::string d = (domain && *domain) ? domain : "native";
    std::string c = (code && *code) ? code : "error";
    std::string m = message ? message : "";
    std::string nd = native_domain ? native_domain : "";
    if (!string_utf8(d) || !string_utf8(c) || !string_utf8(m) || !string_utf8(nd)) {
        ctx(e).failure = "native error fields must be valid UTF-8";
        return nullptr;
    }
    auto ed = std::make_shared<ErrorData>();
    ed->domain = std::move(d);
    ed->code = std::move(c);
    ed->message = std::move(m);
    ed->native_domain = std::move(nd);
    if (has_native_code) ed->native_code = native_code;
    ResultValue r;
    r.ok = false;
    r.payload = result_box(Value(ErrorValue{std::move(ed)}));
    r.error_type = "std.error.Value";
    r.writable = true;
    return ctx(e).put(Value(std::move(r)));
}
hua_value n_error(hua_env e, const char* p) {
    ctx(e).failure = p ? p : "native function failed";
    return nullptr;
}
std::uint32_t n_kind(hua_env e, hua_value h) {
    auto p = ctx(e).get(h);
    if (!p) return HUA_VOID;
    auto k = value_kind(*p);
    if (k <= 4) return static_cast<std::uint32_t>(k);
    if (k == 12) return HUA_BYTES;
    if (k == 25) return HUA_HANDLE;
    return HUA_VOID;
}
template <class T>
int get_scalar(hua_env e, hua_value h, T* out) {
    auto p = ctx(e).get(h);
    if (p && out)
        if (auto v = hua::get_if<T>(&p->data)) {
            *out = *v;
            return 1;
        }
    ctx(e).failure = "native argument type mismatch";
    return 0;
}
int n_as_int(hua_env e, hua_value h, std::int64_t* p) { return get_scalar(e, h, p); }
int n_as_float(hua_env e, hua_value h, double* p) { return get_scalar(e, h, p); }
int n_as_bool(hua_env e, hua_value h, int* out) {
    bool b{};
    if (!out || !get_scalar(e, h, &b)) return 0;
    *out = b ? 1 : 0;
    return 1;
}
int n_as_string(hua_env e, hua_value h, const char** out, std::size_t* n) {
    auto p = ctx(e).get(h);
    if (p && out && n)
        if (auto v = hua::string_data(&p->data)) {
            *out = v->data();
            *n = v->size();
            return 1;
        }
    ctx(e).failure = "native argument type mismatch";
    return 0;
}
int n_as_bytes(hua_env e, hua_value h, const std::uint8_t** out, std::size_t* n) {
    auto p = ctx(e).get(h);
    if (p && out && n)
        if (auto v = hua::get_if<BytesValue>(&p->data)) {
            *out = reinterpret_cast<const std::uint8_t*>(v->data->data());
            *n = v->data->size();
            return 1;
        }
    ctx(e).failure = "native argument type mismatch (expected Bytes)";
    return 0;
}
hua_value n_handle_new(hua_env e, std::uint32_t type_tag, void* payload, hua_handle_destructor destructor) {
    auto& c = ctx(e);
    if (!c.handles || type_tag == 0) {
        c.failure = "invalid native handle creation";
        return nullptr;
    }
    auto h = c.handles->create(c.module_token, type_tag, payload, destructor);
    if (h.slot == 0) {
        c.failure = "native handle table capacity exceeded";
        return nullptr;
    }
    return c.put(Value(h));
}
int n_handle_get(hua_env e, hua_value handle, std::uint32_t expected_type_tag, void** out_payload) {
    auto& c = ctx(e);
    auto* p = c.get(handle);
    if (!p) return HUA_HANDLE_INVALID;
    auto* hv = hua::get_if<NativeHandleValue>(&p->data);
    if (!hv || !c.handles) {
        c.failure = "native argument is not a NativeHandle";
        return HUA_HANDLE_INVALID;
    }
    int st = c.handles->get(*hv, c.module_token, expected_type_tag, out_payload);
    if (st == HUA_HANDLE_INVALID) {
        c.failure = "invalid, stale, or wrong-type NativeHandle";
    }
    return st;
}
int n_handle_close(hua_env e, hua_value handle, std::uint32_t expected_type_tag,
                   hua_handle_closer closer, char* err_buf, std::size_t err_cap) {
    auto& c = ctx(e);
    auto* p = c.get(handle);
    if (!p) return HUA_HANDLE_INVALID;
    auto* hv = hua::get_if<NativeHandleValue>(&p->data);
    if (!hv || !c.handles) {
        c.failure = "native argument is not a NativeHandle";
        return HUA_HANDLE_INVALID;
    }
    int st = c.handles->close(*hv, c.module_token, expected_type_tag, closer, err_buf, err_cap);
    if (st == HUA_HANDLE_INVALID) {
        c.failure = "invalid, stale, or wrong-type NativeHandle on close";
    }
    return st;
}
const hua_api_v1 api_v1 = {
    HUA_NATIVE_ABI, sizeof(hua_api_v1),
    n_nil, n_bool, n_int, n_float, n_string, n_error,
    n_kind, n_as_bool, n_as_int, n_as_float, n_as_string
};
const hua_api_v2 api_v2 = {
    HUA_NATIVE_ABI_V2, sizeof(hua_api_v2),
    n_nil, n_bool, n_int, n_float, n_string, n_bytes,
    n_ok_value, n_ok_void, n_err_value, n_error,
    n_kind, n_as_bool, n_as_int, n_as_float, n_as_string, n_as_bytes,
    n_handle_new, n_handle_get, n_handle_close
};
} // namespace

struct ExternalRegistry::Instance {
#ifdef _WIN32
    HMODULE library{};
#else
    void* library{};
#endif
    std::uint32_t abi_version{1};
    std::uint32_t module_token{};
    std::unique_ptr<Wasm> wasm;
    std::unordered_map<std::string, hua_native_function> native_v1;
    std::unordered_map<std::string, hua_native_function_v2> native_v2;
    ~Instance() {
#ifdef _WIN32
        if (library) FreeLibrary(library);
#else
        if (library) dlclose(library);
#endif
    }
};

ExternalRegistry::ExternalRegistry(std::filesystem::path root)
    : root_(std::move(root)), handles_(std::make_unique<HandleTable>()) {}

ExternalRegistry::~ExternalRegistry() {
    reset();
}

bool ExternalRegistry::contains(const std::string& name) const {
    for (const auto& m : modules_)
        for (const auto& e : m.exports)
            if (e.linked_name == name) return true;
    return false;
}
bool ExternalRegistry::module_exists(const std::string& id) const {
    for (const auto& m : modules_)
        if (m.id == id) return true;
    return false;
}
std::vector<ExternalExport> ExternalRegistry::inspect_wasm(const std::string& bytes, const SourceSpan& s) {
    Wasm wasm(bytes, s);
    wasm_importtype_vec_t imports;
    wasmtime_module_imports(wasm.module, &imports);
    bool imported = imports.size != 0;
    wasm_importtype_vec_delete(&imports);
    if (imported) fail(s, "WASM host/module imports (including WASI) are not enabled");
    wasm_exporttype_vec_t exports;
    wasmtime_module_exports(wasm.module, &exports);
    struct Guard {
        wasm_exporttype_vec_t& e;
        ~Guard() { wasm_exporttype_vec_delete(&e); }
    } guard{exports};
    if (exports.size > 4096) fail(s, "too many WASM exports");
    std::vector<ExternalExport> out;
    for (std::size_t i = 0; i < exports.size; ++i) {
        auto type = wasm_exporttype_type(exports.data[i]);
        if (wasm_externtype_kind(type) != WASM_EXTERN_FUNC) continue;
        auto name = wasm_exporttype_name(exports.data[i]);
        ExternalExport e;
        e.name = std::string(name->data, name->size);
        if (!identifier(e.name)) fail(s, "WASM function export is not a Hua identifier");
        auto fn = wasm_externtype_as_functype_const(type);
        auto ps = wasm_functype_params(fn), rs = wasm_functype_results(fn);
        if (ps->size > 32 || rs->size > 1) fail(s, "WASM ABI supports at most 32 arguments and one result");
        for (std::size_t j = 0; j < ps->size; ++j) e.parameters.push_back(wasm_type(wasm_valtype_kind(ps->data[j]), s));
        e.result = rs->size ? wasm_type(wasm_valtype_kind(rs->data[0]), s) : "void";
        out.push_back(std::move(e));
    }
    return out;
}
void ExternalRegistry::add(ExternalModule m) {
    if (module_exists(m.id) || modules_.size() >= 128 || m.exports.size() > 4096)
        fail(m.span, "duplicate/oversized external module registry");
    if (m.abi_version != 1 && m.abi_version != 2)
        fail(m.span, "unsupported external module ABI version", "E7002");
    std::filesystem::path rel(std::u8string(m.artifact.begin(), m.artifact.end()));
    if (rel.empty() || rel.is_absolute() || rel.has_root_name() || rel.has_root_directory())
        fail(m.span, "external artifact path must be relative");
    for (const auto& part : rel)
        if (part == "..") fail(m.span, "external artifact path escapes bundle root");
    std::unordered_set<std::string> names;
    for (const auto& e : m.exports) {
        if (!identifier(e.name) || e.linked_name.empty() || contains(e.linked_name) ||
            !names.insert(e.name).second || e.parameters.size() > 32)
            fail(m.span, "invalid/duplicate external function signature");
        if (m.abi_version == 1) {
            native_type_v1(e.result, m.span);
            for (const auto& t : e.parameters)
                if (native_type_v1(t, m.span) == HUA_VOID) fail(m.span, "void parameter is not valid");
        } else {
            validate_native_v2_type(e.result, true, m.span);
            for (const auto& t : e.parameters) validate_native_v2_type(t, false, m.span);
        }
    }
    if (m.kind == ExternalKind::Wasm) {
        auto actual = inspect_wasm(m.wasm, m.span);
        if (actual.size() != m.exports.size()) fail(m.span, "WASM export metadata mismatch");
        for (const auto& e : m.exports) {
            auto found = std::find_if(actual.begin(), actual.end(), [&](const auto& a) { return a.name == e.name; });
            if (found == actual.end() || found->parameters != e.parameters || found->result != e.result)
                fail(m.span, "WASM export signature mismatch");
        }
    }
    modules_.push_back(std::move(m));
}
void ExternalRegistry::reset() {
    // CRITICAL: Destroy all open native handles while DLLs are still loaded, then unload DLLs.
    if (handles_) handles_->destroy_all();
    instances_.clear();
}
void ExternalRegistry::initialize(const std::string& id, const SourceSpan& site) {
    if (instances_.contains(id)) return;
    auto found = std::find_if(modules_.begin(), modules_.end(), [&](const auto& m) { return m.id == id; });
    if (found == modules_.end()) fail(site, "unknown external module");
    const auto& m = *found;
    auto instance = std::make_unique<Instance>();
    instance->abi_version = m.abi_version;
    instance->module_token = next_module_token_++;
    if (m.kind == ExternalKind::Wasm) {
        instance->wasm = std::make_unique<Wasm>(m.wasm, site);
        instance->wasm->start(site);
    } else {
        std::error_code ec;
        std::filesystem::path path;
        if (!m.source_artifact.empty()) {
            path = std::filesystem::canonical(m.source_artifact, ec);
            if (ec) fail(site, "cannot find native library `" + m.artifact + "`", "E7001");
        } else {
            path = std::filesystem::canonical(root_ / std::filesystem::path(std::u8string(m.artifact.begin(), m.artifact.end())), ec);
            if (ec) fail(site, "cannot find native library `" + m.artifact + "`", "E7001");
            auto relative = std::filesystem::relative(path, root_, ec);
            if (ec || relative.empty() || *relative.begin() == "..")
                fail(site, "native library escapes bundle root", "E7001");
        }
#ifdef _WIN32
        instance->library = LoadLibraryExW(path.c_str(), nullptr,
                                           LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
#else
        instance->library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
        if (!instance->library) fail(site, "cannot load native library `" + m.artifact + "`", "E7001");

        if (m.abi_version == 1) {
#ifdef _WIN32
            auto entry = reinterpret_cast<const hua_module_v1* (*)()>(GetProcAddress(instance->library, "hua_module_entry_v1"));
#else
            auto entry = reinterpret_cast<const hua_module_v1* (*)()>(dlsym(instance->library, "hua_module_entry_v1"));
#endif
            if (!entry) fail(site, "native library is missing hua_module_entry_v1", "E7002");
            auto descriptor = entry();
            if (!descriptor || descriptor->abi_version != HUA_NATIVE_ABI || descriptor->struct_size != sizeof(hua_module_v1))
                fail(site, "native ABI version/descriptor size mismatch", "E7002");
            if (descriptor->export_count != m.exports.size() || (descriptor->export_count && !descriptor->exports))
                fail(site, "native export count differs from manifest", "E7003");
            for (const auto& expected : m.exports) {
                const hua_export_v1* actual = nullptr;
                for (std::size_t j = 0; j < descriptor->export_count; ++j)
                    if (descriptor->exports[j].name && expected.name == descriptor->exports[j].name) {
                        if (actual) fail(site, "duplicate native descriptor export");
                        actual = &descriptor->exports[j];
                    }
                if (!actual || !actual->invoke || actual->parameter_count != expected.parameters.size() ||
                    actual->result_type != native_type_v1(expected.result, site) ||
                    (actual->parameter_count && !actual->parameter_types))
                    fail(site, "native export signature differs from manifest");
                for (std::size_t j = 0; j < expected.parameters.size(); ++j)
                    if (actual->parameter_types[j] != native_type_v1(expected.parameters[j], site))
                        fail(site, "native parameter signature differs from manifest");
                instance->native_v1.emplace(expected.name, actual->invoke);
            }
        } else {
#ifdef _WIN32
            auto entry = reinterpret_cast<const hua_module_v2* (*)()>(GetProcAddress(instance->library, "hua_module_entry_v2"));
#else
            auto entry = reinterpret_cast<const hua_module_v2* (*)()>(dlsym(instance->library, "hua_module_entry_v2"));
#endif
            if (!entry) fail(site, "native library is missing hua_module_entry_v2", "E7002");
            auto descriptor = entry();
            if (!descriptor || descriptor->abi_version != HUA_NATIVE_ABI_V2 || descriptor->struct_size != sizeof(hua_module_v2))
                fail(site, "native ABI v2 version/descriptor size mismatch", "E7002");
            if (descriptor->export_count != m.exports.size() || (descriptor->export_count && !descriptor->exports))
                fail(site, "native export count differs from manifest", "E7003");
            for (const auto& expected : m.exports) {
                const hua_export_v2* actual = nullptr;
                for (std::size_t j = 0; j < descriptor->export_count; ++j)
                    if (descriptor->exports[j].name && expected.name == descriptor->exports[j].name) {
                        if (actual) fail(site, "duplicate native descriptor export");
                        actual = &descriptor->exports[j];
                    }
                if (!actual || !actual->invoke || !actual->result_type ||
                    actual->parameter_count != expected.parameters.size() ||
                    expected.result != actual->result_type ||
                    (actual->parameter_count && !actual->parameter_types))
                    fail(site, "native export signature differs from manifest", "E7003");
                for (std::size_t j = 0; j < expected.parameters.size(); ++j)
                    if (!actual->parameter_types[j] || expected.parameters[j] != actual->parameter_types[j])
                        fail(site, "native parameter signature differs from manifest", "E7003");
                instance->native_v2.emplace(expected.name, actual->invoke);
            }
        }
    }
    instances_.emplace(id, std::move(instance));
}
Value ExternalRegistry::call(const std::string& name, const std::vector<Value>& values,
                             const std::vector<bool>& contextual, const SourceSpan& s) {
    const ExternalModule* mod = nullptr;
    const ExternalExport* fn = nullptr;
    for (const auto& m : modules_)
        for (const auto& e : m.exports)
            if (e.linked_name == name) {
                mod = &m;
                fn = &e;
            }
    if (!fn) fail(s, "external function was not registered");
    auto instance = instances_.find(mod->id);
    if (instance == instances_.end()) fail(s, "external module is not initialized", "E4001");
    if (fn->parameters.size() != values.size()) fail(s, "external argument count mismatch", "E4003");
    std::vector<Value> args;
    for (std::size_t i = 0; i < values.size(); ++i)
        args.push_back(enforce_type(values[i], fn->parameters[i], s, i < contextual.size() && contextual[i]));
    Value result;
    if (mod->kind == ExternalKind::Native) {
        NativeCall call;
        call.handles = handles_.get();
        call.module_token = instance->second->module_token;
        std::vector<hua_value> handles;
        for (const auto& a : args) handles.push_back(call.put(a));
        hua_value returned = nullptr;
        if (instance->second->abi_version == 1) {
            returned = instance->second->native_v1.at(fn->name)(
                &api_v1, reinterpret_cast<hua_env>(&call), handles.data(), static_cast<std::uint32_t>(handles.size()));
        } else {
            returned = instance->second->native_v2.at(fn->name)(
                &api_v2, reinterpret_cast<hua_env>(&call), handles.data(), static_cast<std::uint32_t>(handles.size()));
        }
        if (!call.failure.empty()) fail(s, "Native: " + call.failure, "E7004");
        if (fn->result == "void" && !returned) {
            result = Value(VoidValue{});
        } else {
            auto value = call.get(returned);
            if (!call.failure.empty()) fail(s, "Native: " + call.failure, "E7004");
            if (!value) fail(s, "native result is not a valid call handle", "E7004");
            result = *value;
        }
        auto res_args = type_arguments(fn->result, "Result");
        if (res_args.size() == 2) {
            if (!holds<ResultValue>(result.data))
                fail(s, "Native: Result-returning function must return ok_value/ok_void/err_value", "E7004");
            auto& rv = mget<ResultValue>(result.data);
            rv.success_type = res_args[0];
            rv.error_type = res_args[1];
            if (rv.ok && res_args[0] != "void") {
                if (!rv.payload) fail(s, "Native: ok Result missing payload", "E7004");
                *rv.payload = enforce_type(*rv.payload, res_args[0], s);
            } else if (!rv.ok) {
                if (!rv.payload) fail(s, "Native: err Result missing payload", "E7004");
                *rv.payload = enforce_type(*rv.payload, res_args[1], s);
            }
            return result;
        }
        if (fn->result == "void") return Value(VoidValue{});
    } else {
        auto& w = *instance->second->wasm;
        auto context = wasmtime_store_context(w.store);
        wasmtime_extern_t exported{};
        if (!wasmtime_instance_export_get(context, &w.instance, fn->name.data(), fn->name.size(), &exported) ||
            exported.kind != WASMTIME_EXTERN_FUNC)
            fail(s, "missing WASM function export");
        auto signature = wasmtime_func_type(context, &exported.of.func);
        struct Guard {
            wasm_functype_t* p;
            ~Guard() { wasm_functype_delete(p); }
        } guard{signature};
        auto ps = wasm_functype_params(signature), rs = wasm_functype_results(signature);
        std::vector<wasmtime_val_t> raw(args.size());
        for (std::size_t i = 0; i < args.size(); ++i) {
            auto k = wasm_valtype_kind(ps->data[i]);
            auto& v = raw[i];
            if (k == WASM_I32) {
                auto x = as_int(args[i], s);
                if (x < std::numeric_limits<std::int32_t>::min() || x > std::numeric_limits<std::int32_t>::max())
                    fail(s, "WASM i32 argument out of range", "E4002");
                v.kind = WASM_I32;
                v.of.i32 = static_cast<std::int32_t>(x);
            } else if (k == WASM_I64) {
                v.kind = WASM_I64;
                v.of.i64 = as_int(args[i], s);
            } else if (k == WASM_F32) {
                v.kind = WASM_F32;
                v.of.f32 = static_cast<float>(hua::get<double>(args[i].data));
                if (!std::isfinite(v.of.f32)) fail(s, "WASM f32 argument out of range", "E4002");
            } else {
                v.kind = WASM_F64;
                v.of.f64 = hua::get<double>(args[i].data);
            }
        }
        wasmtime_val_t returned{};
        wasm_trap_t* trap = nullptr;
        checked(wasmtime_func_call(context, &exported.of.func, raw.data(), raw.size(), &returned, rs->size, &trap), s);
        trapped(trap, s);
        if (rs->size) {
            if (returned.kind == WASM_I32) result = Value(static_cast<std::int64_t>(returned.of.i32));
            else if (returned.kind == WASM_I64) result = Value(returned.of.i64);
            else if (returned.kind == WASM_F32) result = Value(static_cast<double>(returned.of.f32));
            else result = Value(returned.of.f64);
            if (auto x = hua::get_if<double>(&result.data); x && !std::isfinite(*x))
                fail(s, "WASM returned a non-finite float", "E4002");
        }
    }
    return enforce_type(std::move(result), fn->result, s);
}
} // namespace hua
