#define PY_SSIZE_T_CLEAN
#define NOMINMAX
#include "hua/native.h"
#include "python_config.hpp"
#include <Python.h>
#include <cmath>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
namespace {
constexpr std::size_t max_text = 16 * 1024 * 1024, max_handles = 4096, max_nodes = 1000000;
using Id = std::int64_t;
struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct Ref {
    PyObject *p{};
    explicit Ref(PyObject *value = nullptr) : p(value) {}
    ~Ref() { Py_XDECREF(p); }
    Ref(const Ref &) = delete;
    Ref &operator=(const Ref &) = delete;
    PyObject *release() {
        auto value = p;
        p = nullptr;
        return value;
    }
};
struct State {
    std::mutex mutex;
    bool initialized{}, closed{};
    std::filesystem::path home;
    std::thread::id owner;
    PyThreadState *saved{};
    PyObject *loads{}, *dumps{};
    Id next{1};
    std::map<Id, PyObject *> objects;
};
// The DLL stays resident; no Python finalization runs from a DLL loader-lock destructor.
State &state() {
    static auto *value = new State;
    return *value;
}
std::string quote(const std::string &s) {
    std::string out = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c < 32) {
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else
            out += static_cast<char>(c);
    }
    return out + '"';
}
std::string error(const std::string &text) {
    return "{\"ok\":false,\"error\":" + quote(text) + "}";
}
std::string python_error() {
    PyObject *t = nullptr, *v = nullptr, *tb = nullptr;
    PyErr_Fetch(&t, &v, &tb);
    PyErr_NormalizeException(&t, &v, &tb);
    Ref type(t), value(v), trace(tb);
    std::string name = "PythonError", reason = "Python operation failed";
    if (t) {
        Ref n(PyObject_GetAttrString(t, "__name__"));
        if (n.p) {
            auto c = PyUnicode_AsUTF8(n.p);
            if (c)
                name = c;
        }
        PyErr_Clear();
    }
    if (v) {
        Ref text(PyObject_Str(v));
        if (text.p) {
            Ref encoded(PyUnicode_AsEncodedString(text.p, "utf-8", "backslashreplace"));
            if (encoded.p)
                reason.assign(PyBytes_AS_STRING(encoded.p),
                              static_cast<std::size_t>(PyBytes_GET_SIZE(encoded.p)));
        }
        PyErr_Clear();
    }
    return name + ": " + reason;
}
PyObject *checked(PyObject *value) {
    if (!value)
        throw Failure(python_error());
    return value;
}
void checked(int status) {
    if (status < 0)
        throw Failure(python_error());
}
std::string string(PyObject *value) {
    if (!PyUnicode_Check(value))
        throw Failure("expected a Python string");
    Py_ssize_t size;
    const char *text = PyUnicode_AsUTF8AndSize(value, &size);
    if (!text)
        throw Failure(python_error());
    if (size > static_cast<Py_ssize_t>(max_text))
        throw Failure("string exceeds 16 MiB");
    return {text, static_cast<std::size_t>(size)};
}
std::string field_string(PyObject *doc, const char *name) {
    auto value = PyDict_GetItemString(doc, name);
    if (!value)
        throw Failure(std::string("missing request field: ") + name);
    return string(value);
}
Id id(PyObject *value) {
    if (!value || !PyLong_CheckExact(value))
        throw Failure("object handle must be an integer");
    auto n = PyLong_AsLongLong(value);
    if (PyErr_Occurred())
        throw Failure(python_error());
    if (n <= 0)
        throw Failure("object handle must be positive");
    return n;
}
PyObject *object(Id handle) {
    auto found = state().objects.find(handle);
    if (found == state().objects.end())
        throw Failure("invalid or released Python object handle");
    return found->second;
}
PyObject *retain(PyObject *owned) {
    Ref value(checked(owned));
    auto &s = state();
    if (s.objects.size() >= max_handles || s.next == std::numeric_limits<Id>::max())
        throw Failure("Python object handle limit exceeded (4096)");
    auto handle = s.next;
    Ref result(checked(PyLong_FromLongLong(handle)));
    s.objects.emplace(handle, value.p);
    ++s.next;
    value.release();
    return result.release();
}
void validate_json(PyObject *value, std::size_t depth, std::size_t &nodes,
                   std::set<PyObject *> &active) {
    if (depth > 64 || ++nodes > max_nodes)
        throw Failure("JSON depth/node limit exceeded");
    if (value == Py_None || PyBool_Check(value))
        return;
    if (PyLong_CheckExact(value)) {
        PyLong_AsLongLong(value);
        if (PyErr_Occurred())
            throw Failure(python_error());
        return;
    }
    if (PyFloat_CheckExact(value)) {
        if (!std::isfinite(PyFloat_AS_DOUBLE(value)))
            throw Failure("JSON number must be finite");
        return;
    }
    if (PyUnicode_CheckExact(value)) {
        string(value);
        return;
    }
    bool list = PyList_CheckExact(value), tuple = PyTuple_CheckExact(value),
         dict = PyDict_CheckExact(value);
    if (!list && !tuple && !dict)
        throw Failure("Python object is not JSON-compatible; call an explicit conversion method");
    if (!active.insert(value).second)
        throw Failure("cyclic Python value cannot convert to JSON");
    if (dict) {
        PyObject *key, *item;
        Py_ssize_t at = 0;
        while (PyDict_Next(value, &at, &key, &item)) {
            if (!PyUnicode_CheckExact(key))
                throw Failure("JSON object keys must be strings");
            string(key);
            validate_json(item, depth + 1, nodes, active);
        }
    } else {
        auto size = list ? PyList_GET_SIZE(value) : PyTuple_GET_SIZE(value);
        for (Py_ssize_t i = 0; i < size; ++i)
            validate_json(list ? PyList_GET_ITEM(value, i) : PyTuple_GET_ITEM(value, i), depth + 1,
                          nodes, active);
    }
    active.erase(value);
}
void validate_json(PyObject *value) {
    std::size_t nodes = 0;
    std::set<PyObject *> active;
    validate_json(value, 0, nodes, active);
}
std::string encode(PyObject *value) {
    Ref args(checked(PyTuple_Pack(1, value))), kwargs(checked(PyDict_New()));
    checked(PyDict_SetItemString(kwargs.p, "allow_nan", Py_False));
    Ref encoded(checked(PyObject_Call(state().dumps, args.p, kwargs.p)));
    return string(encoded.p);
}
void clear() {
    auto &s = state();
    for (auto [_, value] : s.objects)
        Py_DECREF(value);
    s.objects.clear();
    Py_CLEAR(s.loads);
    Py_CLEAR(s.dumps);
}
struct Gil {
    bool active{true};
    Gil() {
        PyEval_RestoreThread(state().saved);
        state().saved = nullptr;
    }
    ~Gil() {
        if (active)
            state().saved = PyEval_SaveThread();
    }
};
std::filesystem::path path(const std::string &text) {
    if (text.find('\0') != std::string::npos)
        throw Failure("path contains NUL");
    return std::filesystem::absolute(std::filesystem::path(std::u8string(text.begin(), text.end())))
        .lexically_normal();
}
bool module_name(const std::string &name) {
    bool first = true;
    if (name.empty())
        return false;
    for (char c : name) {
        if (c == '.') {
            if (first)
                return false;
            first = true;
            continue;
        }
        bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        if (!alpha && (first || c < '0' || c > '9'))
            return false;
        first = false;
    }
    return !first;
}
std::string initialize(const std::string &requested) {
    auto &s = state();
    if (s.closed)
        return error(
            "Python bridge is closed; initialization cannot be repeated after shutdown/failure");
    try {
        auto home = path(requested.empty() ? HUA_PYTHON_DEFAULT_HOME : requested);
#ifdef _WIN32
        auto executable = home / "python.exe", encodings = home / "Lib" / "encodings";
#else
        auto executable = home / "bin" / "python3.12",
             encodings = home / "lib" / "python3.12" / "encodings";
#endif
        if (!std::filesystem::is_regular_file(executable) ||
            !std::filesystem::is_directory(encodings))
            return error("Python home must contain a complete CPython 3.12 installation");
        if (s.initialized) {
            if (s.owner != std::this_thread::get_id())
                return error("Python bridge calls must use the initialization thread");
            if (home != s.home)
                return error("Python home is already fixed for this process");
            return "{\"ok\":true,\"value\":true}";
        }
        if (Py_IsInitialized())
            return error("Python is already initialized outside this bridge");
        PyConfig config;
        PyConfig_InitIsolatedConfig(&config);
        config.site_import = 1;
        config.user_site_directory = 0;
        config.install_signal_handlers = 0;
        config.parse_argv = 0;
        config.write_bytecode = 0;
        auto status = PyConfig_SetString(&config, &config.home, home.wstring().c_str());
        if (!PyStatus_Exception(status))
            status =
                PyConfig_SetString(&config, &config.program_name, executable.wstring().c_str());
        if (!PyStatus_Exception(status))
            status = PyConfig_SetString(&config, &config.executable, executable.wstring().c_str());
        if (!PyStatus_Exception(status))
            status = PyConfig_SetString(&config, &config.stdio_encoding, L"utf-8");
        if (!PyStatus_Exception(status))
            status = PyConfig_SetString(&config, &config.stdio_errors, L"backslashreplace");
        if (!PyStatus_Exception(status))
            status = Py_InitializeFromConfig(&config);
        std::string reason =
            PyStatus_Exception(status)
                ? (status.err_msg ? status.err_msg : "Python initialization failed")
                : "";
        PyConfig_Clear(&config);
        if (!reason.empty()) {
            s.closed = true;
            return error(reason);
        }
        s.initialized = true;
        s.home = home;
        s.owner = std::this_thread::get_id();
        try {
            Ref json(checked(PyImport_ImportModule("json")));
            s.loads = checked(PyObject_GetAttrString(json.p, "loads"));
            s.dumps = checked(PyObject_GetAttrString(json.p, "dumps"));
        } catch (const std::exception &e) {
            std::string message = e.what();
            clear();
            Py_FinalizeEx();
            s.initialized = false;
            s.closed = true;
            return error(message);
        }
        s.saved = PyEval_SaveThread();
        return "{\"ok\":true,\"value\":true}";
    } catch (const std::filesystem::filesystem_error &) {
        return error("Python home path cannot be inspected");
    } catch (const std::exception &e) {
        return error(e.what());
    }
}
PyObject *dispatch(PyObject *doc, const std::string &op) {
    auto &s = state();
    if (op == "version")
        return checked(PyUnicode_FromString(Py_GetVersion()));
    if (op == "count")
        return checked(PyLong_FromSize_t(s.objects.size()));
    if (op == "shutdown")
        return Py_NewRef(Py_True);
    if (op == "add_path") {
        auto directory = path(field_string(doc, "path"));
        if (!std::filesystem::is_directory(directory))
            throw Failure("Python search path is not an existing directory");
        auto text = directory.u8string();
        Ref entry(checked(PyUnicode_DecodeUTF8(reinterpret_cast<const char *>(text.data()),
                                               static_cast<Py_ssize_t>(text.size()), "strict")));
        auto paths = PySys_GetObject("path");
        if (!paths || !PyList_Check(paths))
            throw Failure("Python sys.path is unavailable");
        auto found = PySequence_Contains(paths, entry.p);
        checked(found);
        if (!found)
            checked(PyList_Insert(paths, 0, entry.p));
        return Py_NewRef(Py_True);
    }
    if (op == "import") {
        auto name = field_string(doc, "name");
        if (!module_name(name))
            throw Failure("invalid absolute Python module name");
        return retain(PyImport_ImportModule(name.c_str()));
    }
    if (op == "from_json") {
        auto value = PyDict_GetItemString(doc, "value");
        if (!value)
            throw Failure("missing request field: value");
        return retain(Py_NewRef(value));
    }
    if (op == "release") {
        auto handle = id(PyDict_GetItemString(doc, "target"));
        auto found = s.objects.find(handle);
        if (found == s.objects.end())
            return Py_NewRef(Py_False);
        auto value = found->second;
        s.objects.erase(found);
        Py_DECREF(value);
        return Py_NewRef(Py_True);
    }
    if (op == "getattr" || op == "call" || op == "to_json" || op == "to_string") {
        auto value = object(id(PyDict_GetItemString(doc, "target")));
        if (op == "getattr") {
            auto name = field_string(doc, "name");
            if (name.empty() || name.find('\0') != std::string::npos)
                throw Failure("invalid Python attribute name");
            Ref attribute(checked(
                PyUnicode_DecodeUTF8(name.data(), static_cast<Py_ssize_t>(name.size()), "strict")));
            return retain(PyObject_GetAttr(value, attribute.p));
        }
        if (op == "to_json") {
            validate_json(value);
            return Py_NewRef(value);
        }
        if (op == "to_string")
            return checked(PyObject_Str(value));
        auto positional = PyDict_GetItemString(doc, "args"),
             keywords = PyDict_GetItemString(doc, "kwargs");
        if (!positional || !PyList_CheckExact(positional) || !keywords ||
            !PyDict_CheckExact(keywords))
            throw Failure("call requires a handle list and keyword handle object");
        auto size = PyList_GET_SIZE(positional);
        if (size > 1024 || PyDict_Size(keywords) > 1024)
            throw Failure("Python call argument limit exceeded (1024)");
        if (!PyCallable_Check(value))
            throw Failure("Python target is not callable");
        Ref args(checked(PyTuple_New(size))), kwargs(checked(PyDict_New()));
        for (Py_ssize_t i = 0; i < size; ++i) {
            auto item = object(id(PyList_GET_ITEM(positional, i)));
            PyTuple_SET_ITEM(args.p, i, Py_NewRef(item));
        }
        PyObject *key, *item;
        Py_ssize_t at = 0;
        while (PyDict_Next(keywords, &at, &key, &item))
            checked(PyDict_SetItem(kwargs.p, key, object(id(item))));
        return retain(PyObject_Call(value, args.p, kwargs.p));
    }
    throw Failure("unknown Python bridge operation");
}
std::string request(const std::string &message) {
    auto &s = state();
    if (!s.initialized || s.closed)
        return error("Python bridge is not initialized or has been shut down");
    if (s.owner != std::this_thread::get_id())
        return error("Python bridge calls must use the initialization thread");
    Gil gil;
    bool closing = false;
    std::string reply;
    try {
        {
            Ref text(checked(PyUnicode_DecodeUTF8(
                message.data(), static_cast<Py_ssize_t>(message.size()), "strict")));
            Ref doc(checked(PyObject_CallOneArg(s.loads, text.p)));
            if (!PyDict_CheckExact(doc.p))
                throw Failure("Python bridge request must be an object");
            validate_json(doc.p);
            auto op = field_string(doc.p, "op");
            Ref value(checked(dispatch(doc.p, op))), response(checked(PyDict_New()));
            checked(PyDict_SetItemString(response.p, "ok", Py_True));
            checked(PyDict_SetItemString(response.p, "value", value.p));
            reply = encode(response.p);
            closing = op == "shutdown";
        }
        if (closing) {
            clear();
            gil.active = false;
            auto status = Py_FinalizeEx();
            s.initialized = false;
            s.closed = true;
            return status < 0 ? error("Python finalization failed") : reply;
        }
        return reply;
    } catch (const std::filesystem::filesystem_error &) {
        return error("Python search path cannot be inspected");
    } catch (const std::exception &e) {
        PyErr_Clear();
        return error(e.what());
    }
}
hua_value initialize_export(const hua_api_v1 *api, hua_env env, const hua_value *args, uint32_t) {
    const char *text;
    std::size_t length;
    if (!api->as_string(env, args[0], &text, &length))
        return nullptr;
    std::lock_guard lock(state().mutex);
    auto response = initialize(std::string(text, length));
    if (response.size() > max_text)
        response = error("response exceeds 16 MiB");
    return api->string(env, response.data(), response.size());
}
hua_value request_export(const hua_api_v1 *api, hua_env env, const hua_value *args, uint32_t) {
    const char *text;
    std::size_t length;
    if (!api->as_string(env, args[0], &text, &length))
        return nullptr;
    std::lock_guard lock(state().mutex);
    auto response =
        length > max_text ? error("request exceeds 16 MiB") : request(std::string(text, length));
    if (response.size() > max_text)
        response = error("response exceeds 16 MiB");
    return api->string(env, response.data(), response.size());
}
const uint32_t strings[] = {HUA_STRING};
const hua_export_v1 exports[] = {{"initialize", 1, strings, HUA_STRING, initialize_export},
                                 {"request", 1, strings, HUA_STRING, request_export}};
} // namespace
extern "C" HUA_MODULE_EXPORT const hua_module_v1 *hua_module_entry_v1(void) {
#ifdef _WIN32
    HMODULE pinned{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(&hua_module_entry_v1), &pinned))
        return nullptr;
#else
    static void *pinned = nullptr;
    if (!pinned) {
        Dl_info info{};
        if (!dladdr(reinterpret_cast<void *>(&hua_module_entry_v1), &info))
            return nullptr;
        pinned = dlopen(info.dli_fname, RTLD_NOW | RTLD_LOCAL);
        if (!pinned)
            return nullptr;
    }
#endif
    static const hua_module_v1 module = {HUA_NATIVE_ABI, sizeof(hua_module_v1), 2, exports};
    return &module;
}
