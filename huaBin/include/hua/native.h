#ifndef HUA_NATIVE_H
#define HUA_NATIVE_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#define HUA_NATIVE_ABI 1u
#ifdef _WIN32
#define HUA_MODULE_EXPORT __declspec(dllexport)
#else
#define HUA_MODULE_EXPORT __attribute__((visibility("default")))
#endif
typedef struct hua_env__ *hua_env;
typedef struct hua_value__ *hua_value;
enum hua_kind { HUA_NIL=0, HUA_BOOL=1, HUA_INT=2, HUA_FLOAT=3, HUA_STRING=4, HUA_VOID=5 };
/* All handles and string views expire when the exported call returns. */
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
/* The DLL exports this C symbol; descriptors remain valid until unload. */
HUA_MODULE_EXPORT const hua_module_v1 *hua_module_entry_v1(void);
#ifdef __cplusplus
}
#endif
#endif
