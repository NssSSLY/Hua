#include "hua/native.h"
#include <limits>
#include <string>
static hua_value add(const hua_api_v1* api,hua_env env,const hua_value* args,uint32_t) {
    int64_t a,b;if(!api->as_int(env,args[0],&a)||!api->as_int(env,args[1],&b))return nullptr;
    if((b>0&&a>std::numeric_limits<int64_t>::max()-b)||(b<0&&a<std::numeric_limits<int64_t>::min()-b))return api->error(env,"native integer overflow");
    return api->integer(env,a+b);
}
static hua_value greeting(const hua_api_v1* api,hua_env env,const hua_value* args,uint32_t) {
    const char* data;size_t length;if(!api->as_string(env,args[0],&data,&length))return nullptr;
    std::string text="hello ";text.append(data,length);return api->string(env,text.data(),text.size());
}
static hua_value invert(const hua_api_v1* api,hua_env env,const hua_value* args,uint32_t) {
    int b;if(!api->as_bool(env,args[0],&b))return nullptr;return api->boolean(env,!b);
}
static hua_value twice(const hua_api_v1* api,hua_env env,const hua_value* args,uint32_t) {
    double x;if(!api->as_float(env,args[0],&x))return nullptr;return api->floating(env,x*2);
}
static const uint32_t ints[]={HUA_INT,HUA_INT},strings[]={HUA_STRING},bools[]={HUA_BOOL},floats[]={HUA_FLOAT};
static const hua_export_v1 exports[]={
    {"add",2,ints,HUA_INT,add},{"greeting",1,strings,HUA_STRING,greeting},
    {"invert",1,bools,HUA_BOOL,invert},{"twice",1,floats,HUA_FLOAT,twice}
};
extern "C" HUA_MODULE_EXPORT const hua_module_v1* hua_module_entry_v1(void) {
    static const hua_module_v1 module={HUA_NATIVE_ABI,sizeof(hua_module_v1),4,exports};return &module;
}
