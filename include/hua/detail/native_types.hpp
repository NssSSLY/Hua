#pragma once

namespace hn {

// The function's cleanup has already run. A failed result conversion is
// attributed to the call site, outside the returning frame.
template<class Convert> auto complete_return_conversion(Convert& convert){
    auto& state = current_state();
    const auto depth=state.depth;
    const auto* site=state.sites[depth-1];
    state.depth=depth-1;
    auto value=convert(*site);
    state.depth=depth;
    return value;
}

template<class T> struct NumericStorage {
    using Rep=T;
    static Rep read(T value){return value;}
    static T make(Rep value){return value;}
};
template<ExactName Name,class T> struct NumericStorage<Exact<Name,T>> {
    using Rep=T;
    static Rep read(Exact<Name,T> value){return value.value;}
    static Exact<Name,T> make(Rep value){return {value};}
};
template<class To,bool Widening=false,class FromType>
To numeric_coerce(const FromType& source,const SourceSpan& site,bool contextual){
    using S=typename NumericStorage<FromType>::Rep;
    using T=typename NumericStorage<To>::Rep;
    const auto x=NumericStorage<FromType>::read(source);
    if(contextual||Widening){
        if constexpr(std::is_integral_v<S>&&std::is_integral_v<T>){
            if(std::in_range<T>(x))return NumericStorage<To>::make(static_cast<T>(x));
        }else if constexpr(std::is_floating_point_v<T>){
            const auto v=static_cast<T>(x);
            if(std::isfinite(v))return NumericStorage<To>::make(v);
        }
    }
    return from_value<To>(hua::convert_numeric(to_value(source),TypeName<To>::get(),site,contextual));
}

template<char Op,ExactName Name,class Rep>
Exact<Name,Rep> exact_arithmetic(Exact<Name,Rep> left,Exact<Name,Rep> right,const SourceSpan& site){
    const auto x=left.value,y=right.value;
    Rep result{};
    bool invalid{};
    if constexpr(std::is_floating_point_v<Rep>){
        const F a=x,b=y;
        const auto n=Op=='+'?a+b:Op=='-'?a-b:Op=='*'?a*b:a/b;
        result=static_cast<Rep>(n);
        invalid=!std::isfinite(result);
    } else if constexpr(Op=='+')invalid=__builtin_add_overflow(x,y,&result);
    else if constexpr(Op=='-')invalid=__builtin_sub_overflow(x,y,&result);
    else invalid=__builtin_mul_overflow(x,y,&result);
    if(invalid)[[unlikely]]{
        const char op[]{Op,0};
        return general_binary<Exact<Name,Rep>>(op,left,right,site);
    }
    return {result};
}

struct ArrayContext {
    bool literal{};
    std::initializer_list<ArrayContext> elements;
};
inline Value contextual_array(Value value,const ArrayContext& context){
    if(const auto view=hua::cget_if<hua::SliceValue>(&value.data);view&&context.elements.size()==view->length&&view->storage){
        auto sequence=*view;
        auto items=hua::managed<std::vector<Value>>();
        items->reserve(sequence.length);
        sequence.contextual.clear();
        std::size_t i=0;
        for(const auto& item:context.elements){
            items->push_back(contextual_array(hua::sequence_read(sequence,i++),item));
            sequence.contextual.push_back(item.literal);
        }
        sequence.storage=std::move(items);sequence.start=0;
        return Value(std::move(sequence));
    }
    return value;
}
template<class To,class S> To sequence_coerce(const S& source,const SourceSpan& site,const ArrayContext& context){
    auto raw=to_value(source);
    if(context.literal)raw=contextual_array(std::move(raw),context);
    return from_value<To>(hua::enforce_type(std::move(raw),TypeName<To>::get(),site,context.literal));
}
template<class T> T read_element(const Slice<T>& slice,I index,const SourceSpan& site){
    T value=at(slice,index,site);
    if(!slice.writable)value=capture_value(std::move(value));
    return value;
}
template<class T,std::size_t N> Fixed<T,N> read_element(const Slice<Fixed<T,N>>& slice,I index,const SourceSpan& site){
    auto value=at(slice,index,site);
    value.view.writable=value.view.writable&&slice.writable;
    return value;
}
template<class T,std::size_t N> Slice<T> nested_fixed_view(const Slice<Fixed<T,N>>& slice,I index,const SourceSpan& site){
    auto view=sequence_view(at(slice,index,site));
    view.writable=view.writable&&slice.writable;
    return view;
}
template<class T,std::size_t N> void check_fixed_extent(I length,const SourceSpan& site){
    if(length!=static_cast<I>(N))[[unlikely]]fail(site,"fixed array length mismatch","E4003");
    // A nested packed shape can exceed the storage cap at legal extents.
    if constexpr(N!=0&&requires(T v){v.view;}){
        static const auto width=hua::packed_width(TypeName<T>::get());
        if(width&&N>134217728/(*width?*width:1))[[unlikely]]
            fail(site,"fixed array storage exceeds 128 MiB","E4099");
    }
}
template<class T,std::size_t N> Fixed<T,N> make_fixed_literal(std::vector<T> values,const SourceSpan& site){
    check_fixed_extent<T,N>(static_cast<I>(values.size()),site);
    return Fixed<T,N>(Slice<T>::of(std::move(values)));
}
template<class T,std::size_t N> Fixed<T,N> make_fixed(const Slice<T>& source,const SourceSpan& site){
    check_fixed_extent<T,N>(source.length,site);
    auto view=Slice<T>::of(source.length?
        std::vector<T>(source.storage->begin()+source.start,source.storage->begin()+source.start+source.length):
        std::vector<T>{});
    view.writable=source.writable;
    return Fixed<T,N>(std::move(view));
}

} // namespace hn
