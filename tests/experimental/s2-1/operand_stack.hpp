// Experimental S2-1 candidate; not used by the production VM.
// See docs/VM与自举性能改造分析.md#s2-1-results-20261030.
#pragma once
#include "hua/value.hpp"
#include <algorithm>
#include <memory>
#include <type_traits>
#include <utility>
#include <stdexcept>
#include <vector>
namespace hua {
// VM-private storage. Slots never move while occupied. Vacant slots retain only
// capacity, never a language value, owner lease, map key, or resource.
template<class T> class OperandSlots {
    static constexpr std::size_t block_size=32;
public:
    struct Slot { std::optional<T> value;Slot* next{}; };
private:
    std::vector<std::unique_ptr<Slot[]>> blocks_;
    Slot* free_{};
    std::size_t live_{},peak_{};
public:
    template<class U> Slot* acquire(U&& value){
        if(!free_){
            auto block=std::make_unique<Slot[]>(block_size);
            blocks_.push_back(std::move(block));
            auto* first=blocks_.back().get();
            for(std::size_t i=0;i<block_size;++i){first[i].next=free_;free_=&first[i];}
        }
        auto* slot=free_;
        slot->value.emplace(std::forward<U>(value)); // failure leaves the free list unchanged
        free_=slot->next;slot->next=nullptr;++live_;peak_=std::max(peak_,live_);return slot;
    }
    void release(Slot* slot) noexcept {slot->value.reset();slot->next=free_;free_=slot;--live_;}
    std::size_t live()const{return live_;}
    std::size_t peak()const{return peak_;}
    std::size_t capacity()const{return blocks_.size()*block_size;}
};
// Values retain the existing inline ownership. Only assignment targets move
// into stable slots, including targets waiting across nested calls or await.
// Generic Value pooling was measured and withdrawn: extra moves outweighed it.
// Local is a non-owning reference whose enclosing VM frame retains its binding.
template<class Local> class OperandStack {
    using Locations=OperandSlots<ValueLocation>;
public:
    struct LocationRef {typename Locations::Slot* slot;};
    struct Entry {
        using Data=std::variant<Value,LocationRef,Local>;
        Data data;
        template<class T,class U> Entry(std::in_place_type_t<T> tag,U&& v):data(tag,std::forward<U>(v)){}
    };
private:
    Locations locations_;
    std::vector<Entry> entries_;
    void release(Entry& e)noexcept{
        if(auto p=std::get_if<LocationRef>(&e.data))locations_.release(p->slot);
    }
public:
    OperandStack()=default;
    OperandStack(const OperandStack&)=delete;
    OperandStack& operator=(const OperandStack&)=delete;
    ~OperandStack(){clear();}
    std::size_t size()const{return entries_.size();}
    bool empty()const{return entries_.empty();}
    Entry& back(){return entries_.back();}
    Entry& operator[](std::size_t n){return entries_[n];}
    void emplace_back(const Value& v){entries_.emplace_back(std::in_place_type<Value>,v);}
    void emplace_back(Value&& v){entries_.emplace_back(std::in_place_type<Value>,std::move(v));}
    void emplace_back(ValueLocation&& v){
        auto* slot=locations_.acquire(std::move(v));
        try{entries_.emplace_back(std::in_place_type<LocationRef>,LocationRef{slot});}
        catch(...){locations_.release(slot);throw;}
    }
    void emplace_back(Local local){entries_.emplace_back(std::in_place_type<Local>,local);}
    template<class T> requires (std::is_same_v<T,bool>||std::is_same_v<T,std::int64_t>||std::is_same_v<T,double>)
    void emplace_back(std::in_place_type_t<Value>,T value){entries_.emplace_back(std::in_place_type<Value>,value);}
    bool is_value(const Entry& e)const{return std::holds_alternative<Value>(e.data);}
    Value* value_if(Entry& e){return std::get_if<Value>(&e.data);}
    Value& value(Entry& e){return std::get<Value>(e.data);}
    ValueLocation* location_if(Entry& e){auto p=std::get_if<LocationRef>(&e.data);return p?&*p->slot->value:nullptr;}
    Local* local_if(Entry& e){return std::get_if<Local>(&e.data);}
    Value take_value(){auto result=std::move(value(back()));pop_back();return result;}
    void replace_back(Value&& v){release(back());back().data=std::move(v);}
    void pop_back()noexcept{release(back());entries_.pop_back();}
    void resize(std::size_t n){if(n>size())throw std::logic_error("operand stack cannot grow through resize");while(size()>n)pop_back();}
    void clear()noexcept{while(!empty())pop_back();}
    std::size_t value_live()const{std::size_t n=0;for(const auto& e:entries_)n+=std::holds_alternative<Value>(e.data)&&std::get<Value>(e.data).data.index()>3;return n;}
    std::size_t location_live()const{return locations_.live();}
    std::size_t location_capacity()const{return locations_.capacity();}
};
}
