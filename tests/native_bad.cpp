#include "hua/native.h"
static hua_value fn(const hua_api_v1* api,hua_env e,const hua_value*,uint32_t) {
#if BAD_KIND == 5
    return api->string(e,"\xc0\xaf",2);
#elif BAD_KIND == 3
    return (hua_value)(uintptr_t)123;
#elif BAD_KIND == 4
    return api->floating(e,2.5);
#else
    return api->integer(e,7);
#endif
}
extern "C" HUA_MODULE_EXPORT const hua_module_v1* hua_module_entry_v1(void) {
    static const hua_export_v1 exports[]={
#if BAD_KIND == 2
        {"value",0,0,HUA_FLOAT,fn}
#else
        {"value",0,0,HUA_INT,fn}
#endif
    };
#if BAD_KIND == 1
    static const hua_module_v1 module={99,sizeof(hua_module_v1),1,exports};
#else
    static const hua_module_v1 module={1,sizeof(hua_module_v1),1,exports};
#endif
    return &module;
}
