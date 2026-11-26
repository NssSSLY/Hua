#ifndef HUA_NATIVE_H
#define HUA_NATIVE_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#define HUA_NATIVE_ABI 1u
#define HUA_NATIVE_ABI_V2 2u
#ifdef _WIN32
#define HUA_MODULE_EXPORT __declspec(dllexport)
#else
#define HUA_MODULE_EXPORT __attribute__((visibility("default")))
#endif
typedef struct hua_env__ *hua_env;
typedef struct hua_value__ *hua_value;
enum hua_kind {
    HUA_NIL = 0,
    HUA_BOOL = 1,
    HUA_INT = 2,
    HUA_FLOAT = 3,
    HUA_STRING = 4,
    HUA_VOID = 5,
    HUA_BYTES = 6,
    HUA_HANDLE = 7
};
enum hua_handle_status {
    HUA_HANDLE_OK = 0,
    HUA_HANDLE_CLOSED = 1,
    HUA_HANDLE_POISONED = 2,
    HUA_HANDLE_INVALID = 3
};
typedef void (*hua_handle_destructor)(void *payload);
typedef int (*hua_handle_closer)(void *payload, char *err_buf, size_t err_cap);
/* All value handles, string views, and byte views expire when the exported call returns. */
typedef struct hua_api_v1 {
    uint32_t abi_version, struct_size;
    hua_value (*nil)(hua_env);
    hua_value (*boolean)(hua_env, int);
    hua_value (*integer)(hua_env, int64_t);
    hua_value (*floating)(hua_env, double);
    hua_value (*string)(hua_env, const char *, size_t);
    hua_value (*error)(hua_env, const char *);
    uint32_t (*kind)(hua_env, hua_value);
    int (*as_bool)(hua_env, hua_value, int *);
    int (*as_int)(hua_env, hua_value, int64_t *);
    int (*as_float)(hua_env, hua_value, double *);
    int (*as_string)(hua_env, hua_value, const char **, size_t *);
} hua_api_v1;
typedef hua_value (*hua_native_function)(const hua_api_v1 *, hua_env, const hua_value *, uint32_t);
typedef struct hua_export_v1 {
    const char *name;
    uint32_t parameter_count;
    const uint32_t *parameter_types;
    uint32_t result_type;
    hua_native_function invoke;
} hua_export_v1;
typedef struct hua_module_v1 {
    uint32_t abi_version, struct_size, export_count;
    const hua_export_v1 *exports;
} hua_module_v1;
/* Synchronous Native ABI v2: borrowed input Bytes, host-copied return Bytes,
 * host-owned generational NativeHandle control blocks, and Result<T, std.error.Value>. */
typedef struct hua_api_v2 {
    uint32_t abi_version, struct_size;
    hua_value (*nil)(hua_env);
    hua_value (*boolean)(hua_env, int);
    hua_value (*integer)(hua_env, int64_t);
    hua_value (*floating)(hua_env, double);
    hua_value (*string)(hua_env, const char *, size_t);
    hua_value (*bytes)(hua_env, const uint8_t *, size_t);
    hua_value (*ok_value)(hua_env, hua_value);
    hua_value (*ok_void)(hua_env);
    hua_value (*err_value)(hua_env, const char *domain, const char *code, const char *message,
                           const char *native_domain, int has_native_code, int64_t native_code);
    hua_value (*abort_error)(hua_env, const char *message);
    uint32_t (*kind)(hua_env, hua_value);
    int (*as_bool)(hua_env, hua_value, int *);
    int (*as_int)(hua_env, hua_value, int64_t *);
    int (*as_float)(hua_env, hua_value, double *);
    int (*as_string)(hua_env, hua_value, const char **, size_t *);
    int (*as_bytes)(hua_env, hua_value, const uint8_t **, size_t *);
    hua_value (*handle_new)(hua_env, uint32_t type_tag, void *payload, hua_handle_destructor destructor);
    int (*handle_get)(hua_env, hua_value handle, uint32_t expected_type_tag, void **out_payload);
    int (*handle_close)(hua_env, hua_value handle, uint32_t expected_type_tag,
                        hua_handle_closer closer, char *err_buf, size_t err_cap);
} hua_api_v2;
typedef hua_value (*hua_native_function_v2)(const hua_api_v2 *, hua_env, const hua_value *, uint32_t);
typedef struct hua_export_v2 {
    const char *name;
    uint32_t parameter_count;
    const char *const *parameter_types;
    const char *result_type;
    hua_native_function_v2 invoke;
} hua_export_v2;
typedef struct hua_module_v2 {
    uint32_t abi_version, struct_size, export_count;
    const hua_export_v2 *exports;
} hua_module_v2;
/* The DLL exports the matching C symbol; descriptors remain valid until unload. */
HUA_MODULE_EXPORT const hua_module_v1 *hua_module_entry_v1(void);
HUA_MODULE_EXPORT const hua_module_v2 *hua_module_entry_v2(void);
#ifdef __cplusplus
}
#endif
#endif
