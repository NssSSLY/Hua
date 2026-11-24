#pragma once

#include "hua/detail/text_format.hpp"

namespace hn {
inline void check_text_bound(const Str& value,const SourceSpan& site){
    if(value.text().size()>hua::text_format::max_text)[[unlikely]]
        fail(site,"standard library text exceeds 16 MiB","E4099");
}
inline Result<Str,Str> text_slice(const Str& input,I first,I last,const SourceSpan& site){
    check_text_bound(input,site);
    const auto& text=input.text();
    if(first<0||last<first||static_cast<std::uint64_t>(last)>text.size())
        return err(Str("string slice bounds are invalid"),site);
    const auto view=std::string_view(text).substr(static_cast<std::size_t>(first),static_cast<std::size_t>(last-first));
    if(!hua::text_utf8_error(view).empty())
        return err(Str("string slice cuts a UTF-8 character"),site);
    return ok(Str(std::string(view)));
}
inline I text_last_index(const Str& text,const Str& pattern,const SourceSpan& site){
    check_text_bound(text,site);check_text_bound(pattern,site);
    if(pattern.text().empty())return static_cast<I>(text.text().size());
    const auto at=text.text().rfind(pattern.text());
    return at==std::string::npos?-1:static_cast<I>(at);
}
inline I text_count(const Str& text,const Str& pattern,const SourceSpan& site){
    check_text_bound(text,site);check_text_bound(pattern,site);
    const auto& s=text.text();const auto& key=pattern.text();
    if(key.empty())return static_cast<I>(hua::text_format::scalars(s)+1);
    I count=0;
    for(auto at=s.find(key);at!=std::string::npos;at=s.find(key,at+key.size()))++count;
    return count;
}
inline Str text_trim(const Str& text,const SourceSpan& site){
    check_text_bound(text,site);
    constexpr std::string_view blank=" \t\r\n\v\f";
    const auto& s=text.text();
    const auto begin=s.find_first_not_of(blank);
    if(begin==std::string::npos)return Str{};
    const auto end=s.find_last_not_of(blank)+1;
    return begin==0&&end==s.size()?text:Str(s.substr(begin,end-begin));
}
template<bool Upper> Str text_ascii_case(const Str& text,const SourceSpan& site){
    check_text_bound(text,site);
    auto out=text.text();
    for(char& c:out){
        if constexpr(Upper){if(c>='a'&&c<='z')c-=32;}
        else if(c>='A'&&c<='Z')c+=32;
    }
    return Str(std::move(out));
}
inline Result<Str,Str> text_template(const Str& text,const Map<Str,Str>& values,const SourceSpan& site){
    check_text_bound(text,site);
    std::string out,problem;
    const auto resolve=[&](std::string_view key)->const std::string*{
        if(const auto v=values.data->find_text(key))return &v->text();
        problem="FORMAT_ARGS: missing template key "+std::string(key);
        return nullptr;
    };
    if(!hua::text_format::expand_placeholders(text.text(),out,problem,site,resolve))
        return {false,Str{},Str(std::move(problem)),traced({},site,"Result error created here")};
    if(out.size()>hua::text_format::max_text)[[unlikely]]fail(site,"template text exceeds 16 MiB","E4099");
    return {true,Str(std::move(out)),Str{}};
}

// Synchronous algorithms use typed elements with the same invocation order and
// work limits as callback_stdlib.cpp. Only the callback's own call is charged.
struct CallbackWork {
    const SourceSpan& site;
    std::size_t count{};
    const char* message{"callback work limit exceeded (32000000)"};
    void tick(std::size_t n=1){
        const auto before=count;
        count+=n;
        if(count>32000000)[[unlikely]]fail(site,message,"E4099");
        if((before>>10)!=(count>>10))hua::task_checkpoint(state.context,site);
    }
};
template<class T> auto element(const Slice<T>& input,I i){
    return T((*input.storage)[static_cast<std::size_t>(input.start+i)]);
}
template<class T,class Fn> auto alg_map(const Slice<T>& xs,Fn fn,const SourceSpan& site){
    using R=std::invoke_result_t<Fn,T>;
    std::vector<R> out;out.reserve(static_cast<std::size_t>(xs.length));
    CallbackWork work{site};
    for(I i=0;i<xs.length;++i){work.tick();out.push_back(fn(element(xs,i)));}
    return Slice<R>::of(std::move(out));
}
template<class T,class A,class Fn> A alg_fold(const Slice<T>& xs,A acc,Fn fn,const SourceSpan& site){
    CallbackWork work{site};
    for(I i=0;i<xs.length;++i){work.tick();acc=fn(std::move(acc),element(xs,i));}
    return acc;
}
template<class T,class U,class Fn> auto alg_zip_with(const Slice<T>& xs,const Slice<U>& ys,Fn fn,const SourceSpan& site){
    using R=std::invoke_result_t<Fn,T,U>;
    const auto n=std::min(xs.length,ys.length);
    std::vector<R> out;out.reserve(static_cast<std::size_t>(n));
    CallbackWork work{site};
    for(I i=0;i<n;++i){work.tick();out.push_back(fn(element(xs,i),element(ys,i)));}
    return Slice<R>::of(std::move(out));
}
template<class T,class Fn> auto alg_filter(const Slice<T>& xs,Fn fn,const SourceSpan& site){
    std::vector<T> out;
    CallbackWork work{site};
    for(I i=0;i<xs.length;++i){
        work.tick();auto item=element(xs,i);
        if(fn(item))out.push_back(std::move(item));
    }
    return Slice<T>::of(std::move(out));
}
template<class T,class Fn> auto alg_partition(const Slice<T>& xs,Fn fn,const SourceSpan& site){
    std::vector<T> yes,no;
    CallbackWork work{site};
    for(I i=0;i<xs.length;++i){
        work.tick();auto item=element(xs,i);
        (fn(item)?yes:no).push_back(std::move(item));
    }
    return std::tuple{Slice<T>::of(std::move(yes)),Slice<T>::of(std::move(no))};
}
template<class T,class Fn> I alg_find_index(const Slice<T>& xs,Fn fn,const SourceSpan& site){
    CallbackWork work{site};
    for(I i=0;i<xs.length;++i){work.tick();if(fn(element(xs,i)))return i;}
    return -1;
}
template<class T,class Fn> bool alg_any(const Slice<T>& xs,Fn fn,const SourceSpan& site){
    return alg_find_index(xs,fn,site)>=0;
}
template<class T,class Fn> bool alg_all(const Slice<T>& xs,Fn fn,const SourceSpan& site){
    return alg_find_index(xs,[&](T value){return !fn(std::move(value));},site)<0;
}
template<class T,class Fn> Opt<T> alg_find(const Slice<T>& xs,Fn fn,const SourceSpan& site){
    CallbackWork work{site};
    for(I i=0;i<xs.length;++i){work.tick();auto item=element(xs,i);if(fn(item))return item;}
    return std::nullopt;
}
inline std::vector<std::size_t> indices(std::size_t count){
    std::vector<std::size_t> out(count);
    for(std::size_t i=0;i<count;++i)out[i]=i;
    return out;
}
template<class T> std::vector<T> snapshot(const Slice<T>& input){
    if(!input.length)return {};
    auto first=input.storage->begin()+input.start;
    return {first,first+input.length};
}
template<class T> Slice<T> permute(std::vector<T>& items,const std::vector<std::size_t>& order){
    std::vector<T> out;out.reserve(items.size());
    for(auto i:order)out.push_back(std::move(items[i]));
    return Slice<T>::of(std::move(out));
}
template<class Less> std::vector<std::size_t> stable_merge_order(std::size_t size,Less less){
    auto order=indices(size);
    std::vector<std::size_t> scratch(size);
    for(std::size_t width=1;width<size;width*=2){
        for(std::size_t start=0;start<size;start+=2*width){
            const auto middle=std::min(start+width,size),end=std::min(start+2*width,size);
            auto left=start,right=middle;
            for(auto out=start;out<end;++out){
                bool take_right=left>=middle;
                if(left<middle&&right<end)take_right=less(order[right],order[left]);
                scratch[out]=take_right?order[right++]:order[left++];
            }
        }
        order.swap(scratch);
    }
    return order;
}
template<class T,class Fn> Slice<T> alg_sorted_by(const Slice<T>& xs,Fn fn,const SourceSpan& site){
    auto items=snapshot(xs);
    CallbackWork work{site};
    const auto order=stable_merge_order(items.size(),[&](auto a,auto b){
        work.tick();
        return fn(T(items[a]),T(items[b]));
    });
    return permute(items,order);
}
template<class T> int compare_callback_keys(const T& left,const T& right,CallbackWork& work){
    work.tick();
    return left<right?-1:right<left?1:0;
}
inline int compare_callback_keys(const Str& left,const Str& right,CallbackWork& work){
    work.tick();
    const auto& a=left.text();const auto& b=right.text();
    const auto n=std::min(a.size(),b.size());
    work.tick(n/64);
    const auto order=n?std::memcmp(a.data(),b.data(),n):0;
    if(order)return order<0?-1:1;
    return a.size()<b.size()?-1:a.size()>b.size()?1:0;
}
template<class Key> Key valid_callback_key(Key key,const SourceSpan& site){
    if constexpr(std::is_same_v<Key,F>)
        if(!std::isfinite(key))fail(site,"sort keys require finite float values","E4003");
    return key;
}
template<class T,class Fn> auto callback_keys(const std::vector<T>& items,Fn fn,CallbackWork& work){
    using Key=std::invoke_result_t<Fn,T>;
    std::vector<Key> keys;keys.reserve(items.size());
    for(const auto& item:items){
        work.tick();keys.push_back(valid_callback_key(fn(T(item)),work.site));
    }
    return keys;
}
template<class T,class Fn> Slice<T> alg_sorted_by_key(const Slice<T>& xs,Fn fn,const SourceSpan& site){
    auto items=snapshot(xs);auto order=indices(items.size());
    CallbackWork work{site};
    const auto keys=callback_keys(items,fn,work);
    std::stable_sort(order.begin(),order.end(),[&](auto a,auto b){return compare_callback_keys(keys[a],keys[b],work)<0;});
    return permute(items,order);
}
template<bool Min,class T,class Fn> Result<T,Str> alg_extreme(const Slice<T>& xs,Fn fn,const SourceSpan& site){
    const auto items=snapshot(xs);
    CallbackWork work{site};
    const auto keys=callback_keys(items,fn,work);
    if(items.empty()){
        auto msg=static_text(Min?"ALG_EMPTY: min_by_key requires non-empty input":"ALG_EMPTY: max_by_key requires non-empty input");
        return Result<T,Str>{false,T{},std::move(msg),traced({},site,"Result error created here")};
    }
    std::size_t best=0;
    for(std::size_t i=1;i<keys.size();++i)
        if((Min?1:-1)*compare_callback_keys(keys[i],keys[best],work)<0)best=i;
    return Result<T,Str>{true,items[best],Str{}};
}
template<class T,class Fn> auto alg_min_by_key(const Slice<T>& xs,Fn fn,const SourceSpan& site){
    return alg_extreme<true>(xs,fn,site);
}
template<class T,class Fn> auto alg_max_by_key(const Slice<T>& xs,Fn fn,const SourceSpan& site){
    return alg_extreme<false>(xs,fn,site);
}

template<class T> void check_list_revision(const List<T>& items,std::uint64_t revision,const SourceSpan& site){
    if(items.data->revision!=revision)[[unlikely]]fail(site,"list modified during callback","E4003");
}
template<class T,class Fn> I list_retain(List<T> items,Fn fn,const SourceSpan& site){
    require_writable(items,site);
    CallbackWork work{site};
    const auto count=items.data->size(),revision=items.data->revision;
    std::vector<unsigned char> keep(count);
    for(std::size_t i=0;i<count;++i){
        work.tick();
        keep[i]=fn(T((*items.data)[i]));
        check_list_revision(items,revision,site);
    }
    std::vector<T> out;out.reserve(count);
    for(std::size_t i=0;i<count;++i)if(keep[i])out.push_back(std::move((*items.data)[i]));
    const auto removed=count-out.size();
    items.data->swap(out);
    ++items.data->revision;
    return static_cast<I>(removed);
}
template<class T> void commit_list_order(List<T> list,const std::vector<std::size_t>& order){
    std::vector<T> sorted;sorted.reserve(order.size());
    for(auto i:order)sorted.push_back(std::move((*list.data)[i]));
    list.data->swap(sorted);
    ++list.data->revision;
}
template<class T,class Fn> void list_sort_by(List<T> list,Fn fn,const SourceSpan& site){
    require_writable(list,site);
    CallbackWork work{site};
    const auto revision=list.data->revision;
    const auto order=stable_merge_order(list.data->size(),[&](auto a,auto b){
        work.tick();
        const bool less=fn(T((*list.data)[a]),T((*list.data)[b]));
        check_list_revision(list,revision,site);
        return less;
    });
    commit_list_order(list,order);
}
template<class T,class Fn> void list_sort_by_key(List<T> list,Fn fn,const SourceSpan& site){
    require_writable(list,site);
    CallbackWork work{site};
    const auto revision=list.data->revision,count=list.data->size();
    using Key=std::invoke_result_t<Fn,T>;
    std::vector<Key> keys;keys.reserve(count);
    for(std::size_t i=0;i<count;++i){
        work.tick();
        keys.push_back(valid_callback_key(fn(T((*list.data)[i])),site));
        check_list_revision(list,revision,site);
    }
    auto order=indices(count);
    std::stable_sort(order.begin(),order.end(),[&](auto a,auto b){
        return compare_callback_keys(keys[a],keys[b],work)<0;
    });
    commit_list_order(list,order);
}
template<class T> bool equal_list_items(const T& a,const T& b,CallbackWork& work){
    if constexpr(std::is_same_v<T,Str>){work.tick(a.text().size()/64);return a==b;}
    else if constexpr(std::is_arithmetic_v<T>)return a==b;
    else return general_binary<bool>("==",a,b,work.site);
}
template<class T> I list_dedup(List<T> items,const SourceSpan& site){
    require_writable(items,site);
    CallbackWork work{site,0,"collection work limit exceeded (32000000)"};
    ++items.data->revision;
    const auto count=items.data->size();
    std::vector<unsigned char> keep(count,1);
    std::size_t last=0;
    for(std::size_t i=1;i<count;++i){
        work.tick();
        if(equal_list_items<T>((*items.data)[i],(*items.data)[last],work))keep[i]=0;
        else last=i;
    }
    std::size_t kept=0;
    for(std::size_t i=0;i<count;++i)if(keep[i]){
        if(i!=kept)(*items.data)[kept]=std::move((*items.data)[i]);
        ++kept;
    }
    items.data->resize(kept);
    return static_cast<I>(count-kept);
}
template<class T> void list_sort(List<T> items,const SourceSpan& site){
    require_writable(items,site);
    CallbackWork work{site,0,"collection work limit exceeded (32000000)"};
    ++items.data->revision;
    auto order=indices(items.data->size());
    std::stable_sort(order.begin(),order.end(),[&](auto a,auto b){
        return compare_callback_keys((*items.data)[a],(*items.data)[b],work)<0;
    });
    std::vector<T> sorted;sorted.reserve(order.size());
    for(auto i:order)sorted.push_back(std::move((*items.data)[i]));
    items.data->swap(sorted);
}
template<class K,class V> auto map_entries(const Map<K,V>& map,const SourceSpan& site){
    CallbackWork work{site,0,"collection work limit exceeded (32000000)"};
    std::vector<K> keys;std::vector<V> values;
    keys.reserve(map.data->size());values.reserve(map.data->size());
    for(const auto& [key,value]:*map.data){
        work.tick();keys.push_back(key);values.push_back(value);
    }
    return std::tuple{Slice<K>::of(std::move(keys)),Slice<V>::of(std::move(values))};
}
template<class K,class V> I map_merge(Map<K,V> dest,const Map<K,V>& source,const SourceSpan& site){
    require_writable(dest,site,"cannot mutate a read-only map");
    if(dest.data.same(source.data))return 0;
    CallbackWork work{site,0,"collection work limit exceeded (32000000)"};
    std::size_t added=0;
    for(const auto& [key,value]:*source.data){work.tick();if(!dest.data->find(key))++added;}
    if(added>1000000-dest.data->size())[[unlikely]]fail(site,"map entry limit exceeded","E4099");
    for(const auto& [key,value]:*source.data)dest.data->insert_or_assign(key,value);
    return static_cast<I>(added);
}
template<class K,class V,class Fn> V map_update(Map<K,V> map,const K& key,V fallback,Fn fn,const SourceSpan& site){
    require_writable(map,site,"cannot mutate a read-only map");
    auto current=map.data->find(key);
    auto next=fn(current?V(current->second):std::move(fallback));
    put(map,key,next,site);
    return next;
}

template<class T> Result<T,Str> collection_error(const char* message,const SourceSpan& site){
    return {false,T{},static_text(message),traced({},site,"Result error created here")};
}
template<class T> Result<T,Str> collection_ok(T item){return {true,std::move(item),Str{}};}
template<class T> Deque<T> deque_from_slice(const Slice<T>& xs,const SourceSpan& site){
    if(xs.length>1000000)[[unlikely]]fail(site,"deque item limit exceeded","E4099");
    Deque<T> out{Ref<std::deque<T>>::make()};
    for(I i=0;i<xs.length;++i)out.data->push_back(clone(element(xs,i)));
    return out;
}
template<class T> I deque_len(const Deque<T>& q){return static_cast<I>(q.data->size());}
template<class T> void deque_clear(Deque<T> q){q.data->clear();}
template<class T> Slice<T> deque_snapshot(const Deque<T>& q){
    return Slice<T>::of(std::vector<T>(q.data->begin(),q.data->end()));
}
template<class T> Result<T,Str> deque_get(const Deque<T>& q,I index,const SourceSpan& site){
    if(static_cast<std::uint64_t>(index)>=q.data->size())
        return collection_error<T>("deque index out of bounds",site);
    return collection_ok(T((*q.data)[static_cast<std::size_t>(index)]));
}
template<bool Back,class T> Result<I,Str> deque_push(Deque<T> q,T item,const SourceSpan& site){
    if(q.data->size()>=1000000)[[unlikely]]return collection_error<I>("deque item limit exceeded",site);
    if constexpr(Back)q.data->push_back(std::move(item));
    else q.data->push_front(std::move(item));
    return collection_ok(deque_len(q));
}
template<bool Back,bool Pop,class T> Result<T,Str> deque_end(const Deque<T>& q,const SourceSpan& site){
    if(q.data->empty())[[unlikely]]return collection_error<T>("DEQUE_EMPTY: deque is empty",site);
    auto item=Back?q.data->back():q.data->front();
    if constexpr(Pop){
        if constexpr(Back)q.data->pop_back();
        else q.data->pop_front();
    }
    return collection_ok(T(std::move(item)));
}
template<class T> void heap_sift_down(std::vector<T>& data,std::size_t i){
    auto item=std::move(data[i]);
    while(2*i+1<data.size()){
        auto child=2*i+1;
        if(child+1<data.size()&&data[child+1]<data[child])++child;
        if(!(data[child]<item))break;
        data[i]=std::move(data[child]);i=child;
    }
    data[i]=std::move(item);
}
template<class T> void heap_heapify(List<T> heap,const SourceSpan& site){
    require_writable(heap,site);
    auto& data=*heap.data;
    ++data.revision;
    for(auto i=data.size()/2;i-->0;)heap_sift_down<T>(data,i);
}
template<class T> Result<I,Str> heap_push(List<T> heap,T item,const SourceSpan& site){
    require_writable(heap,site);
    auto& data=*heap.data;
    ++data.revision;
    if(data.size()>=1000000)[[unlikely]]return collection_error<I>("heap item limit exceeded",site);
    auto i=data.size();data.emplace_back();
    for(;i>0;){
        const auto parent=(i-1)/2;
        if(!(item<data[parent]))break;
        data[i]=std::move(data[parent]);i=parent;
    }
    data[i]=std::move(item);
    return collection_ok(static_cast<I>(data.size()));
}
template<bool Pop,class T> Result<T,Str> heap_end(List<T> heap,const SourceSpan& site){
    auto& data=*heap.data;
    if constexpr(Pop){require_writable(heap,site);++data.revision;}
    if(data.empty())[[unlikely]]return collection_error<T>("HEAP_EMPTY: heap is empty",site);
    T first=data.front();
    if constexpr(Pop){
        if(data.size()>1)data.front()=std::move(data.back());
        data.pop_back();
        if(!data.empty())heap_sift_down<T>(data,0);
    }
    return collection_ok(std::move(first));
}
template<bool Largest,class T> Result<Slice<T>,Str> heap_top(const Slice<T>& xs,I count,const SourceSpan& site){
    if(count<0)fail(site,"heap top-k requires k >= 0","E4003");
    auto items=snapshot(xs);
    const auto take=std::min<std::size_t>(static_cast<std::size_t>(count),items.size());
    std::partial_sort(items.begin(),items.begin()+take,items.end(),[](const T& a,const T& b){
        if constexpr(Largest)return b<a;
        else return a<b;
    });
    items.resize(take);
    return collection_ok(Slice<T>::of(std::move(items)));
}
template<class T,class Fn> Result<I,Str> heap_push_by_key(List<T> list,T item,Fn fn,const SourceSpan& site){
    require_writable(list,site);
    auto& values=*list.data;
    if(values.size()>=1000000)return collection_error<I>("heap item limit exceeded",site);
    const auto revision=values.revision;
    CallbackWork work{site};
    const auto key=valid_callback_key(fn(T(item)),site);
    check_list_revision(list,revision,site);
    std::array<std::size_t,32> path{};std::size_t count=0;
    for(auto i=values.size();i>0;){
        const auto parent=(i-1)/2;work.tick();
        const auto above=valid_callback_key(fn(T(values[parent])),site);
        check_list_revision(list,revision,site);
        if(compare_callback_keys(key,above,work)>=0)break;
        path[count++]=parent;i=parent;
    }
    auto hole=values.size();values.emplace_back();
    for(std::size_t i=0;i<count;++i){const auto parent=path[i];values[hole]=std::move(values[parent]);hole=parent;}
    values[hole]=std::move(item);++values.revision;
    return collection_ok(static_cast<I>(values.size()));
}
template<class T,class Fn> Result<T,Str> heap_pop_by_key(List<T> list,Fn fn,const SourceSpan& site){
    require_writable(list,site);
    auto& values=*list.data;
    if(values.empty())return collection_error<T>("HEAP_EMPTY: heap is empty",site);
    const auto n=values.size()-1,revision=values.revision;
    CallbackWork work{site};
    std::array<std::size_t,32> path{};std::size_t count=0;
    auto key_at=[&](std::size_t index){
        auto key=valid_callback_key(fn(T(values[index])),site);
        check_list_revision(list,revision,site);
        return key;
    };
    if(n){
        const auto key=key_at(n);
        for(std::size_t i=0;2*i+1<n;){
            auto child=2*i+1;work.tick();
            auto best=key_at(child);
            if(child+1<n){
                auto other=key_at(child+1);
                if(compare_callback_keys(other,best,work)<0){++child;best=std::move(other);}
            }
            if(compare_callback_keys(best,key,work)>=0)break;
            path[count++]=child;i=child;
        }
    }
    T top=values.front();
    auto last=std::move(values[n]);values.pop_back();
    if(n){
        std::size_t hole=0;
        for(std::size_t i=0;i<count;++i){const auto child=path[i];values[hole]=std::move(values[child]);hole=child;}
        values[hole]=std::move(last);
    }
    ++values.revision;
    return collection_ok(std::move(top));
}
template<class K> Map<K,bool> set_from_slice(const Slice<K>& xs,const SourceSpan& site){
    auto result=new_map<K,bool>();
    CallbackWork work{site,0,"collection work limit exceeded (32000000)"};
    for(I i=0;i<xs.length;++i){work.tick();put(result,element(xs,i),true,site);}
    return result;
}
enum class SetOperation { Union,Intersection,Difference,SymmetricDifference };
template<SetOperation Op,class K> Map<K,bool> set_combine(const Map<K,bool>& left,const Map<K,bool>& right,const SourceSpan& site){
    auto result=new_map<K,bool>();
    CallbackWork work{site,0,"collection work limit exceeded (32000000)"};
    for(const auto& [key,value]:left.data->items){
        work.tick();
        if constexpr(Op==SetOperation::Union)put(result,key,true,site);
        else if((Op==SetOperation::Intersection)==has(right,key))put(result,key,true,site);
    }
    if constexpr(Op==SetOperation::Union||Op==SetOperation::SymmetricDifference)
        for(const auto& [key,value]:right.data->items){work.tick();if(!has(left,key))put(result,key,true,site);}
    return result;
}
template<class K> bool set_is_subset(const Map<K,bool>& left,const Map<K,bool>& right,const SourceSpan& site){
    if(left.data->size()>right.data->size())return false;
    CallbackWork work{site,0,"collection work limit exceeded (32000000)"};
    for(const auto& [key,value]:*left.data){work.tick();if(!has(right,key))return false;}
    return true;
}
template<class K> bool set_is_disjoint(const Map<K,bool>& left,const Map<K,bool>& right,const SourceSpan& site){
    const auto& small=left.data->size()<=right.data->size()?left:right;
    const auto& large=left.data->size()<=right.data->size()?right:left;
    CallbackWork work{site,0,"collection work limit exceeded (32000000)"};
    for(const auto& [key,value]:*small.data){work.tick();if(has(large,key))return false;}
    return true;
}

inline Rng random_new(I seed){
    auto state=hua::managed<hua::RngState>();
    hua::random_engine::seed(*state,seed);
    return {std::move(state),true};
}
inline hua::RngState& rng_state(const Rng& rng,const SourceSpan& site){
    if(!rng.data)[[unlikely]]fail(site,"expected std.random.Rng","E4003");
    if(!rng.writable)[[unlikely]]fail(site,"standard mutation requires a writable argument","E4007");
    return *rng.data;
}
inline I random_int(const Rng& rng,I lo,I hi,const SourceSpan& site){
    auto& state=rng_state(rng,site);
    if(lo>=hi)[[unlikely]]fail(site,"random.int requires lo < hi","E4003");
    using U=std::uint64_t;
    return static_cast<I>(static_cast<U>(lo)+hua::random_engine::bounded(state,static_cast<U>(hi)-static_cast<U>(lo)));
}
inline F random_float(const Rng& rng,const SourceSpan& site){
    return static_cast<F>(hua::random_engine::next(rng_state(rng,site))>>11)*0x1.0p-53;
}
inline bool random_bool(const Rng& rng,const SourceSpan& site){
    return hua::random_engine::next(rng_state(rng,site))>>63!=0;
}
template<class T> void random_shuffle(const Rng& rng,List<T> items,const SourceSpan& site){
    require_writable(items,site);
    auto& state=rng_state(rng,site);
    ++items.data->revision;
    for(auto i=items.data->size();i>1;--i){
        const auto j=hua::random_engine::bounded(state,i);
        std::iter_swap(items.data->begin()+i-1,items.data->begin()+static_cast<std::size_t>(j));
    }
}
template<class T> Result<T,Str> random_choice(const Rng& rng,const Slice<T>& xs,const SourceSpan& site){
    auto& state=rng_state(rng,site);
    if(!xs.length)return collection_error<T>("RANDOM_EMPTY: choice requires non-empty input",site);
    return collection_ok(element(xs,static_cast<I>(hua::random_engine::bounded(state,xs.length))));
}
template<class T> Result<Slice<T>,Str> random_sample(const Rng& rng,const Slice<T>& xs,I k,const SourceSpan& site){
    auto& state=rng_state(rng,site);
    if(k<0)[[unlikely]]fail(site,"random.sample requires k >= 0","E4003");
    if(k>xs.length)return collection_error<Slice<T>>("RANDOM_RANGE: sample size exceeds input length",site);
    auto index=indices(static_cast<std::size_t>(xs.length));
    std::vector<T> out;out.reserve(static_cast<std::size_t>(k));
    for(std::size_t i=0;i<static_cast<std::size_t>(k);++i){
        const auto j=i+hua::random_engine::bounded(state,index.size()-i);
        std::swap(index[i],index[j]);out.push_back(element(xs,static_cast<I>(index[i])));
    }
    return collection_ok(Slice<T>::of(std::move(out)));
}

}
