#pragma once
// Map storage (V4; D3 2026-11-08: iteration stays in ascending key order). Entries live in
// individually allocated nodes, so references to a stored Value stay valid until that
// entry is erased, as with the std::map it replaces. Lookup is an open-addressing index of
// positions into the order vector. The order vector is ascending whenever sorted_ is set;
// an insert below the current maximum clears it, and the next begin() sorts in place
// (positions move then, so iterators taken before that begin() are stale).
// Requires Value to be complete.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <stdexcept>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>
namespace hua {
class MapEntries {
public:
    using value_type=std::pair<MapKey,Value>;
private:
    struct Node { value_type kv; std::uint64_t hash; };
    mutable std::vector<Node*> order_;        // nullptr marks an erased entry
    mutable std::vector<std::uint32_t> index_; // 0 empty, deleted marker, else position+1
    mutable std::size_t live_{},dead_{},used_{}; // used_: non-empty index slots (live+deleted)
    mutable bool sorted_{true};
    mutable Node* max_{}; // greatest key while sorted_ (the last live node)
    static constexpr std::uint32_t deleted=0xffffffffu;
    static std::uint64_t mix(std::uint64_t x){x^=x>>33;x*=0xff51afd7ed558ccdULL;x^=x>>33;x*=0xc4ceb9fe1a85ec53ULL;x^=x>>33;return x;}
    static std::uint64_t hash_of(const MapKey& k){
        switch(k.index()){
        case 0:return mix(hua::get<0>(k)?0x9e3779b97f4a7c15ULL:0x6a09e667f3bcc909ULL);
        case 1:return mix(static_cast<std::uint64_t>(hua::get<1>(k))+0x243f6a8885a308d3ULL);
        default:{const auto& s=hua::get<2>(k);std::uint64_t h=0xcbf29ce484222325ULL^s.size();
            const auto* p=reinterpret_cast<const unsigned char*>(s.data());std::size_t n=s.size();
            while(n>=8){std::uint64_t w;std::memcpy(&w,p,8);h=(h^mix(w))*0x100000001b3ULL;p+=8;n-=8;}
            std::uint64_t w=0;std::memcpy(&w,p,n);return mix(h^w^(static_cast<std::uint64_t>(n)<<56));}
        }
    }
    // Index slot holding `k`, or the slot where it would be inserted (first deleted or empty).
    std::size_t probe(const MapKey& k,std::uint64_t h,bool& found)const{
        const std::size_t mask=index_.size()-1;std::size_t i=h&mask,first_free=static_cast<std::size_t>(-1);
        for(;;i=(i+1)&mask){
            auto slot=index_[i];
            if(!slot){found=false;return first_free!=static_cast<std::size_t>(-1)?first_free:i;}
            if(slot==deleted){if(first_free==static_cast<std::size_t>(-1))first_free=i;continue;}
            const Node* n=order_[slot-1];
            if(n->hash==h&&n->kv.first==k){found=true;return i;}
        }
    }
    // Compaction moves positions, so only erase (never insert) asks for it: an insert
    // during iteration keeps the iterator's position meaningful.
    void rebuild(std::size_t capacity,bool compact=false)const{
        if(compact&&dead_){std::size_t w=0;for(auto* n:order_)if(n)order_[w++]=n;order_.resize(w);dead_=0;}
        index_.assign(capacity,0);used_=live_;const std::size_t mask=capacity-1;
        for(std::size_t pos=0;pos<order_.size();++pos)if(const Node* n=order_[pos]){std::size_t i=n->hash&mask;while(index_[i])i=(i+1)&mask;index_[i]=static_cast<std::uint32_t>(pos+1);}
    }
    void reserve_one(){
        if(index_.empty()){rebuild(8);return;}
        if((used_+1)*4>index_.size()*3){std::size_t c=index_.size();while((live_+1)*2>c)c*=2;rebuild(c);} // live share ≤1/2 after rebuild
    }
    void release()noexcept{for(auto* n:order_)delete n;}
    void ensure_sorted()const{
        if(sorted_)return;
        std::size_t w=0;for(auto* n:order_)if(n)order_[w++]=n;order_.resize(w);dead_=0;
        std::sort(order_.begin(),order_.end(),[](const Node* a,const Node* b){return a->kv.first<b->kv.first;});
        rebuild(index_.size());sorted_=true;max_=order_.empty()?nullptr:order_.back();
    }
public:
    template<class Ptr,class Ref,class Owner> class basic_iterator {
        friend class MapEntries;Owner* owner_{};std::size_t pos_{};
        void skip(){while(pos_<owner_->order_.size()&&!owner_->order_[pos_])++pos_;}
    public:
        using iterator_category=std::bidirectional_iterator_tag;using value_type=MapEntries::value_type;using difference_type=std::ptrdiff_t;using pointer=Ptr;using reference=Ref;
        basic_iterator()=default;
        basic_iterator(Owner* owner,std::size_t pos):owner_(owner),pos_(pos){skip();}
        template<class P,class R,class O> basic_iterator(const basic_iterator<P,R,O>& o):owner_(o.owner_),pos_(o.pos_){}
        Ref operator*()const{return owner_->order_[pos_]->kv;}
        Ptr operator->()const{return &owner_->order_[pos_]->kv;}
        basic_iterator& operator++(){++pos_;skip();return *this;}
        basic_iterator operator++(int){auto t=*this;++*this;return t;}
        basic_iterator& operator--(){do --pos_;while(!owner_->order_[pos_]);return *this;}
        basic_iterator operator--(int){auto t=*this;--*this;return t;}
        template<class P,class R,class O> bool operator==(const basic_iterator<P,R,O>& o)const{return pos_==o.pos_;}
        template<class P,class R,class O> friend class basic_iterator;
    };
    using iterator=basic_iterator<value_type*,value_type&,MapEntries>;
    using const_iterator=basic_iterator<const value_type*,const value_type&,const MapEntries>;
    MapEntries()=default;
    MapEntries(const MapEntries& o){
        order_.reserve(o.live_);
        try{for(std::size_t p=0;p<o.order_.size();++p)if(const Node* n=o.order_[p]){order_.push_back(new Node{n->kv,n->hash});++live_;}}catch(...){release();throw;}
        if(live_){std::size_t c=8;while(live_*2>c)c*=2;rebuild(c);}
        sorted_=o.sorted_;max_=sorted_&&!order_.empty()?order_.back():nullptr;
    }
    MapEntries(MapEntries&& o)noexcept:order_(std::move(o.order_)),index_(std::move(o.index_)),live_(o.live_),dead_(o.dead_),used_(o.used_),sorted_(o.sorted_),max_(o.max_){o.order_.clear();o.index_.clear();o.live_=o.dead_=o.used_=0;o.sorted_=true;o.max_=nullptr;}
    MapEntries& operator=(MapEntries o)noexcept{swap(o);return *this;}
    ~MapEntries(){release();}
    void swap(MapEntries& o)noexcept{order_.swap(o.order_);index_.swap(o.index_);std::swap(live_,o.live_);std::swap(dead_,o.dead_);std::swap(used_,o.used_);std::swap(sorted_,o.sorted_);std::swap(max_,o.max_);}
    std::size_t size()const{return live_;}
    bool empty()const{return !live_;}
    void clear(){release();order_.clear();index_.clear();live_=dead_=used_=0;sorted_=true;max_=nullptr;}
    void reserve(std::size_t n){if(n>live_){order_.reserve(n+order_.size()-live_);std::size_t c=index_.empty()?8:index_.size();while(n*2>c)c*=2;if(c>index_.size())rebuild(c);}}
    iterator begin(){ensure_sorted();return {this,0};}
    iterator end(){return {this,order_.size()};}
    const_iterator begin()const{ensure_sorted();return {this,0};}
    const_iterator end()const{return {this,order_.size()};}
    iterator find(const MapKey& k){if(!live_)return end();bool f;auto i=probe(k,hash_of(k),f);return f?iterator{this,index_[i]-1u}:end();}
    const_iterator find(const MapKey& k)const{if(!live_)return end();bool f;auto i=probe(k,hash_of(k),f);return f?const_iterator{this,index_[i]-1u}:end();}
    bool contains(const MapKey& k)const{return find(k)!=end();}
    template<class V> std::pair<iterator,bool> emplace(MapKey k,V&& v){
        reserve_one();const auto h=hash_of(k);bool f;auto i=probe(k,h,f);
        if(f)return {iterator{this,index_[i]-1u},false};
        if(!index_[i])++used_;
        const bool ascending=!max_||max_->kv.first<k;
        order_.push_back(new Node{value_type{std::move(k),Value(std::forward<V>(v))},h});++live_;
        if(sorted_){if(ascending)max_=order_.back();else{sorted_=false;max_=nullptr;}}
        index_[i]=static_cast<std::uint32_t>(order_.size());return {iterator{this,order_.size()-1},true};
    }
    template<class V> std::pair<iterator,bool> insert_or_assign(MapKey k,V&& v){
        if(live_){bool f;auto i=probe(k,hash_of(k),f);if(f){auto& slot=order_[index_[i]-1]->kv.second;slot=std::forward<V>(v);return {iterator{this,index_[i]-1u},false};}}
        return emplace(std::move(k),std::forward<V>(v));
    }
    // The hint is ignored: emplace keeps the order itself.
    template<class V> iterator emplace_hint(const_iterator,MapKey k,V&& v){return emplace(std::move(k),std::forward<V>(v)).first;}
    Value& at(const MapKey& k){auto it=find(k);if(it==end())throw std::out_of_range("map key");return it->second;}
    const Value& at(const MapKey& k)const{auto it=find(k);if(it==end())throw std::out_of_range("map key");return it->second;}
    Value& operator[](const MapKey& k){return emplace(k,Value{}).first->second;}
    iterator erase(const_iterator it){
        const std::size_t pos=it.pos_;Node* n=order_[pos];bool f;auto i=probe(n->kv.first,n->hash,f);
        index_[i]=deleted;order_[pos]=nullptr;--live_;++dead_;
        if(n==max_){max_=nullptr;for(std::size_t p=pos;p-->0;)if(order_[p]){max_=order_[p];break;}}
        delete n;
        iterator next{this,pos+1};
        if(dead_>16&&dead_>live_){ // compact; positions shift, so recompute the successor
            std::size_t before=0;for(std::size_t p=0;p<pos;++p)if(order_[p])++before;
            rebuild(index_.size(),true);return iterator{this,before};}
        return next;
    }
    iterator erase(iterator it){return erase(const_iterator(it));}
    std::size_t erase(const MapKey& k){auto it=find(k);if(it==end())return 0;erase(it);return 1;}
};
}
