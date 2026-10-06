#include "hua/simd.hpp"
#include "hua/stdlib.hpp"
#include "hua/tasks.hpp"
#include <bit>
#include <cmath>
#ifdef __SSE2__
#include <emmintrin.h>
#endif
namespace hua {namespace {
template<class T> T scalar(std::string_view op,T a,T b){return op=="add"?a+b:op=="sub"?a-b:a*b;}
double real(const Value& value,const SourceSpan& span){return std::get<double>(convert_numeric(value,"float",span,true).data);}
template<class T> void store(std::string& out,std::size_t index,T value){
    using Bits=std::conditional_t<sizeof(T)==4,std::uint32_t,std::uint64_t>;auto bits=std::bit_cast<Bits>(value);
    for(std::size_t i=0;i<sizeof(T);++i)out[index*sizeof(T)+i]=static_cast<char>((bits>>(8*i))&255);
}
template<class T> Value apply(std::string_view operation,const SliceValue& a,const SliceValue& b,const SourceSpan& span,RuntimeContext* context){
    auto bytes=managed<std::string>(a.length*sizeof(T),'\0');constexpr auto lanes=sizeof(T)==4?4u:2u;
    for(std::size_t i=0;i<a.length;i+=lanes){
        task_checkpoint(context,span);T left[lanes]{},right[lanes]{},result[lanes]{};auto count=std::min<std::size_t>(lanes,a.length-i);
        for(std::size_t j=0;j<count;++j){left[j]=static_cast<T>(real(sequence_read(a,i+j),span));right[j]=static_cast<T>(real(sequence_read(b,i+j),span));}
#ifdef __SSE2__
        if constexpr(sizeof(T)==4){auto x=_mm_loadu_ps(left),y=_mm_loadu_ps(right);auto z=operation=="add"?_mm_add_ps(x,y):operation=="sub"?_mm_sub_ps(x,y):_mm_mul_ps(x,y);_mm_storeu_ps(result,z);}
        else{auto x=_mm_loadu_pd(left),y=_mm_loadu_pd(right);auto z=operation=="add"?_mm_add_pd(x,y):operation=="sub"?_mm_sub_pd(x,y):_mm_mul_pd(x,y);_mm_storeu_pd(result,z);}
#else
        for(std::size_t j=0;j<count;++j)result[j]=scalar(operation,left[j],right[j]);
#endif
        for(std::size_t j=0;j<count;++j){if(!std::isfinite(result[j]))return standard_result(Value(std::string("SIMD floating overflow")),false,"[]"+a.element_type);store(*bytes,i+j,result[j]);}
    }
    SliceValue out;out.length=a.length;out.element_type=a.element_type;out.packed=bytes;out.writable=true;
    return standard_result(Value(out),true,"[]"+a.element_type);
}
}
Value simd_standard(std::string_view operation,const std::vector<Value>& args,const SourceSpan& span,RuntimeContext* context){
    if(operation=="backend"){
#ifdef __SSE2__
        return Value(std::string("sse2"));
#else
        return Value(std::string("scalar"));
#endif
    }
    auto a=std::get_if<SliceValue>(&args[0].data),b=std::get_if<SliceValue>(&args[1].data);
    if(!a||!b||a->element_type!=b->element_type||(a->element_type!="f32"&&a->element_type!="f64"&&a->element_type!="float"))runtime_error(span,"SIMD requires matching f32/f64/float arrays","E4003");
    if(a->length!=b->length)return standard_result(Value(std::string("SIMD length mismatch")),false,"[]"+a->element_type);
    if(a->length>1000000)runtime_error(span,"SIMD item limit exceeded","E4099");
    return a->element_type=="f32"?apply<float>(operation,*a,*b,span,context):apply<double>(operation,*a,*b,span,context);
}
}
