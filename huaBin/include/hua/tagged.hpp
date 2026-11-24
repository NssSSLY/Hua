#pragma once
// Tagged<Ts...>: the Value::Data union. Same alternatives, construction rules and access
// API as the std::variant it replaces (hua::get/get_if/holds_alternative/visit below also
// accept every other type and forward to std::), but copy, move, destruction and access
// dispatch through an inline switch instead of std::variant's visitor tables, with the
// trivially copyable leading alternatives (monostate, bool, int64, double) handled by a
// byte copy. Never valueless: a throwing construction leaves the monostate alternative.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
namespace hua {
template<class... Ts> class Tagged;
namespace tagged_detail {
template<class T,class... Ts> constexpr std::size_t index_of(){std::size_t i=0;bool found=false;((found||(std::is_same_v<T,Ts>?(found=true):(++i,false))),...);return found?i:sizeof...(Ts);}
template<class T> constexpr bool trivially(){return std::is_trivially_copyable_v<T>&&std::is_trivially_destructible_v<T>;}
// std::variant's converting-constructor selection: an imaginary F(T_i) per alternative,
// viable only when T_i x[]={std::forward<U>(u)} is well formed (no narrowing); bool only
// accepts bool.
template<class T,class U> concept no_narrowing=requires(U&& u){ std::type_identity_t<T[]>{std::forward<U>(u)}; };
template<std::size_t I,class T> struct Candidate {
    template<class U> requires (no_narrowing<T,U>&&(!std::is_same_v<std::remove_cv_t<T>,bool>||std::is_same_v<std::remove_cvref_t<U>,bool>))
    std::integral_constant<std::size_t,I> operator()(T,U&&) const;
};
template<class Seq,class... Ts> struct Candidates;
template<std::size_t... I,class... Ts> struct Candidates<std::index_sequence<I...>,Ts...>:Candidate<I,Ts>... { using Candidate<I,Ts>::operator()...; };
template<class U,class... Ts> using best=decltype(Candidates<std::index_sequence_for<Ts...>,Ts...>{}(std::declval<U>(),std::declval<U>()));
template<class U,class... Ts> concept selectable=requires{typename best<U,Ts...>;};
}
template<class... Ts> class Tagged {
    static constexpr std::size_t count=sizeof...(Ts);
    static constexpr std::size_t size=std::max({sizeof(Ts)...}),align=std::max({alignof(Ts)...});
    template<std::size_t I> using At=std::tuple_element_t<I,std::tuple<Ts...>>;
    // Leading trivially copyable alternatives are copied as bytes.
    static constexpr std::size_t trivial_prefix=[]{std::size_t n=0;bool go=true;((go=go&&tagged_detail::trivially<Ts>(),n+=go?1:0),...);return n;}();
    alignas(align) unsigned char storage_[size];
    std::uint8_t tag_{};
    template<std::size_t I> At<I>* slot(){return std::launder(reinterpret_cast<At<I>*>(storage_));}
    template<std::size_t I> const At<I>* slot()const{return std::launder(reinterpret_cast<const At<I>*>(storage_));}
    // Calls f(std::integral_constant<I>) for the current alternative.
    template<class F> static decltype(auto) dispatch(std::size_t tag,F&& f){
        return dispatch_from<0>(tag,std::forward<F>(f));
    }
    template<std::size_t B,class F> static decltype(auto) dispatch_from(std::size_t tag,F&& f){
#define HUA_TAGGED_CASE(k) case k: if constexpr(B+k<count) return f(std::integral_constant<std::size_t,B+k>{}); else break;
        switch(tag-B){
            HUA_TAGGED_CASE(0) HUA_TAGGED_CASE(1) HUA_TAGGED_CASE(2) HUA_TAGGED_CASE(3) HUA_TAGGED_CASE(4) HUA_TAGGED_CASE(5) HUA_TAGGED_CASE(6) HUA_TAGGED_CASE(7)
            HUA_TAGGED_CASE(8) HUA_TAGGED_CASE(9) HUA_TAGGED_CASE(10) HUA_TAGGED_CASE(11) HUA_TAGGED_CASE(12) HUA_TAGGED_CASE(13) HUA_TAGGED_CASE(14) HUA_TAGGED_CASE(15)
            default: break;
        }
#undef HUA_TAGGED_CASE
        if constexpr(B+16<count) return dispatch_from<B+16>(tag,std::forward<F>(f));
        else __builtin_unreachable();
    }
    void destroy()noexcept{
        if(tag_<trivial_prefix)return;
        dispatch(tag_,[this](auto i){using T=At<decltype(i)::value>;slot<decltype(i)::value>()->~T();});
    }
    void copy_from(const Tagged& o){
        if(o.tag_<trivial_prefix){std::memcpy(storage_,o.storage_,8<size?8:size);tag_=o.tag_;return;}
        dispatch(o.tag_,[&](auto i){constexpr auto I=decltype(i)::value;::new(static_cast<void*>(storage_)) At<I>(*o.template slot<I>());});
        tag_=o.tag_;
    }
    void move_from(Tagged&& o)noexcept{
        if(o.tag_<trivial_prefix){std::memcpy(storage_,o.storage_,8<size?8:size);tag_=o.tag_;return;}
        dispatch(o.tag_,[&](auto i){constexpr auto I=decltype(i)::value;::new(static_cast<void*>(storage_)) At<I>(std::move(*o.template slot<I>()));});
        tag_=o.tag_;
    }
public:
    static constexpr std::size_t alternatives=count;
    Tagged()noexcept{static_assert(std::is_same_v<At<0>,std::monostate>);}
    Tagged(const Tagged& o){copy_from(o);}
    Tagged(Tagged&& o)noexcept{move_from(std::move(o));}
    template<class U> requires (!std::is_same_v<std::remove_cvref_t<U>,Tagged>&&tagged_detail::selectable<U,Ts...>)
    Tagged(U&& u){constexpr auto I=tagged_detail::best<U,Ts...>::value;::new(static_cast<void*>(storage_)) At<I>(std::forward<U>(u));tag_=I;}
    ~Tagged(){destroy();}
    Tagged& operator=(const Tagged& o){
        if(this==&o)return *this;
        if(o.tag_<trivial_prefix&&tag_<trivial_prefix){std::memcpy(storage_,o.storage_,8<size?8:size);tag_=o.tag_;return *this;}
        if(tag_==o.tag_){dispatch(tag_,[&](auto i){constexpr auto I=decltype(i)::value;*slot<I>()=*o.template slot<I>();});return *this;}
        Tagged copy(o);destroy();tag_=0;move_from(std::move(copy));return *this;
    }
    Tagged& operator=(Tagged&& o)noexcept{
        if(this==&o)return *this;
        if(tag_==o.tag_&&tag_>=trivial_prefix){dispatch(tag_,[&](auto i){constexpr auto I=decltype(i)::value;*slot<I>()=std::move(*o.template slot<I>());});return *this;}
        destroy();tag_=0;move_from(std::move(o));return *this;
    }
    template<class U> requires (!std::is_same_v<std::remove_cvref_t<U>,Tagged>&&tagged_detail::selectable<U,Ts...>)
    Tagged& operator=(U&& u){
        constexpr auto I=tagged_detail::best<U,Ts...>::value;
        if(tag_==I){*slot<I>()=std::forward<U>(u);return *this;}
        At<I> value(std::forward<U>(u));destroy();tag_=0;::new(static_cast<void*>(storage_)) At<I>(std::move(value));tag_=I;return *this;
    }
    template<class T,class... A> T& emplace(A&&... a){return emplace<tagged_detail::index_of<T,Ts...>()>(std::forward<A>(a)...);}
    template<std::size_t I,class... A> At<I>& emplace(A&&... a){
        destroy();tag_=0;::new(static_cast<void*>(storage_)) At<I>(std::forward<A>(a)...);tag_=static_cast<std::uint8_t>(I);return *slot<I>();
    }
    std::size_t index()const noexcept{return tag_;}
    constexpr bool valueless_by_exception()const noexcept{return false;}
    template<std::size_t I> At<I>* get_if_index()noexcept{return tag_==I?slot<I>():nullptr;}
    template<std::size_t I> const At<I>* get_if_index()const noexcept{return tag_==I?slot<I>():nullptr;}
    template<class T> static constexpr std::size_t index_of(){return tagged_detail::index_of<T,Ts...>();}
    template<class F> decltype(auto) visit(F&& f){return dispatch(tag_,[&](auto i)->decltype(auto){return f(*slot<decltype(i)::value>());});}
    template<class F> decltype(auto) visit(F&& f)const{return dispatch(tag_,[&](auto i)->decltype(auto){return f(*slot<decltype(i)::value>());});}
};
template<class> struct is_tagged:std::false_type{};
template<class... Ts> struct is_tagged<Tagged<Ts...>>:std::true_type{};
template<class V> concept tagged_union=is_tagged<std::remove_cvref_t<V>>::value;
// std::variant-compatible access, for Tagged and (forwarded) every other type.
template<class T,class... Ts> T* get_if(Tagged<Ts...>* v)noexcept{return v?v->template get_if_index<Tagged<Ts...>::template index_of<T>()>():nullptr;}
template<class T,class... Ts> const T* get_if(const Tagged<Ts...>* v)noexcept{return v?v->template get_if_index<Tagged<Ts...>::template index_of<T>()>():nullptr;}
template<class T,class V> requires (!tagged_union<std::remove_pointer_t<V>>) auto get_if(V* v)noexcept{return std::get_if<T>(v);}
template<std::size_t I,class V> auto get_if(V* v)noexcept{if constexpr(tagged_union<V>)return v?v->template get_if_index<I>():nullptr;else return std::get_if<I>(v);}
template<class T,class V> decltype(auto) get(V&& v){
    if constexpr(tagged_union<V>){auto p=hua::get_if<T>(&v);if(!p)throw std::bad_variant_access();if constexpr(std::is_rvalue_reference_v<V&&>)return std::move(*p);else return (*p);}
    else return std::get<T>(std::forward<V>(v));
}
template<std::size_t I,class V> decltype(auto) get(V&& v){
    if constexpr(tagged_union<V>){auto p=v.template get_if_index<I>();if(!p)throw std::bad_variant_access();if constexpr(std::is_rvalue_reference_v<V&&>)return std::move(*p);else return (*p);}
    else return std::get<I>(std::forward<V>(v));
}
template<class T,class V> bool holds_alternative(const V& v)noexcept{
    if constexpr(tagged_union<V>)return v.index()==std::remove_cvref_t<V>::template index_of<T>();
    else return std::holds_alternative<T>(v);
}
template<class F,class V> decltype(auto) visit(F&& f,V&& v){
    if constexpr(tagged_union<V>)return std::forward<V>(v).visit(std::forward<F>(f));
    else return std::visit(std::forward<F>(f),std::forward<V>(v));
}
}
