#include "hua/value.hpp"
#include "hua/enum_value.hpp"
#include <utility>
#include <algorithm>
#include <stdexcept>
namespace hua {
namespace {
std::vector<std::string> sorted_names(std::vector<std::string> names){
    std::sort(names.begin(),names.end());names.erase(std::unique(names.begin(),names.end()),names.end());return names;
}
}
RecordLayout::RecordLayout(std::vector<std::string> fields,std::string name,std::map<std::string,std::string> types):names(sorted_names(std::move(fields))),nominal_name(std::move(name)),annotations(std::move(types)){}
std::size_t RecordLayout::index(const std::string& name)const{
    auto found=std::lower_bound(names.begin(),names.end(),name);
    return found!=names.end()&&*found==name?static_cast<std::size_t>(found-names.begin()):missing;
}
std::shared_ptr<const RecordLayout> record_layout(const Node& declaration){
    std::vector<std::string> names;names.reserve(declaration.children.size());
    for(const auto& field:declaration.children)names.push_back(field->text);
    std::map<std::string,std::string> types;for(const auto& field:declaration.children)if(!field->children.empty())types.emplace(field->text,type_name(*field->children[0]));
    return std::make_shared<const RecordLayout>(std::move(names),declared_name(declaration),std::move(types));
}
RecordFields::iterator RecordFields::find(const std::string& name){
    detach();
    if(auto* f=flat()){
        f->copy_depth=0;auto i=f->layout->index(name);return value_at(i)?iterator(this,i):end();}
    return iterator(this,std::get<Tree>(storage_).find(name));
}
RecordFields::const_iterator RecordFields::find(const std::string& name)const{
    if(auto* f=flat()){auto i=f->layout->index(name);return value_at(i)?const_iterator(this,i):end();}
    return const_iterator(this,std::get<Tree>(storage_).find(name));
}
Value& RecordFields::at(const std::string& name){auto item=find(name);if(item==end())throw std::out_of_range("unknown record field");return item->second;}
const Value& RecordFields::at(const std::string& name)const{auto item=find(name);if(item==end())throw std::out_of_range("unknown record field");return item->second;}
std::pair<RecordFields::iterator,bool> RecordFields::emplace(const std::string& name,Value value){
    detach();
    if(auto* f=flat()){
        f->copy_depth=0;
        auto i=f->layout->index(name);if(i==RecordLayout::missing)throw std::out_of_range("field is absent from record layout");
        if(f->slots.empty())f->slots.resize(f->layout->names.size());
        if(f->slots[i])return {iterator(this,i),false};
        f->slots[i].emplace(std::move(value));++f->count;return {iterator(this,i),true};
    }
    auto [item,inserted]=std::get<Tree>(storage_).emplace(name,std::move(value));return {iterator(this,item),inserted};
}
RecordFields::iterator RecordFields::erase(iterator where){
    detach();
    if(auto* f=flat()){
        f->copy_depth=0;auto i=where.position_;f->slots.at(i).reset();--f->count;return iterator(this,i+1);}
    return iterator(this,std::get<Tree>(storage_).erase(where.tree_));
}
void RecordFields::detach(){
    auto p=std::get_if<std::shared_ptr<Flat>>(&storage_);
    if(!p||p->use_count()==1){if(auto f=flat())f->cycle_free=false;return;}
    auto result=transform([](const Value& v){return copy_value(v);});
    storage_=std::move(result.storage_);
}
void RecordFields::normalize_owned(bool readonly,unsigned depth,bool resource_alias){
    detach();
    auto normalize=[&](Value& value){if(readonly)value=read_only(std::move(value));value=copy_value(std::move(value),false,depth+1,resource_alias);};
    if(auto f=flat()){for(auto& slot:f->slots)if(slot)normalize(*slot);f->copy_depth=0;}
    else for(auto& [_,value]:std::get<Tree>(storage_))normalize(value);
}
namespace {
bool scalar_type(const std::string& type){return type=="int"||type=="float"||type=="bool"||type=="string"||numeric_spec(type).has_value();}
bool acyclic_field(const Value& value){
    if(value_kind(value)<=4||std::holds_alternative<NumericValue>(value.data)||std::holds_alternative<BytesValue>(value.data)||std::holds_alternative<BufferValue>(value.data)||std::holds_alternative<RngValue>(value.data))return true;
    if(auto p=std::get_if<MapValue>(&value.data))return scalar_type(p->data->item_type);
    if(auto p=std::get_if<ListValue>(&value.data))return scalar_type(p->data->element_type);
    if(auto p=std::get_if<DequeValue>(&value.data))return scalar_type(p->data->element_type);
    if(auto p=std::get_if<SliceValue>(&value.data))return !p->fixed&&scalar_type(p->element_type);
    return false;
}
unsigned copy_depth(const Value& v,unsigned depth){
    if(depth>128)runtime_error({},"value nesting limit exceeded","E4099");
    if(std::holds_alternative<VoidValue>(v.data))runtime_error({},"void has no copyable value","E4003");
    unsigned result=0;
    if(auto p=std::get_if<PlainRecord>(&v.data))result=p->layout->depth;
    else if(auto p=std::get_if<StructValue>(&v.data)){
        require_complete_enum(*p->data);
        std::as_const(p->data->fields).each_value([&](const Value& x){result=std::max(result,1+copy_depth(x,depth+1));});
    }else if(auto p=std::get_if<ResultValue>(&v.data);p&&p->payload)result=1+copy_depth(*p->payload,depth+1);
    else if(auto p=std::get_if<MultiValue>(&v.data))for(const auto& x:p->data->values)result=std::max(result,1+copy_depth(x,depth+1));
    else if(auto p=std::get_if<SliceValue>(&v.data);p&&p->fixed)for(std::size_t i=0;i<p->length;++i)result=std::max(result,1+copy_depth(sequence_copy_source(*p,i),depth+1));
    if(depth+result>128)runtime_error({},"value nesting limit exceeded","E4099");
    return result;
}
}
RecordFields RecordFields::snapshot(unsigned depth)const{
    auto f=flat();
    if(!f||f->exposed)return transform([&](const Value& v){return copy_value(v,false,depth+1);});
    if(!f->copy_depth.load(std::memory_order_relaxed)){unsigned height=1;for(const auto& slot:f->slots)if(slot)height=std::max(height,1+copy_depth(*slot,depth+1));f->copy_depth=height;}
    if(depth+f->copy_depth.load(std::memory_order_relaxed)>128)runtime_error({},"value nesting limit exceeded","E4099");
    // Nested record/Result/fixed-array members publish live value aliases when
    // read; sharing their outer table would immediately detach it again. Keep
    // those tables private and share their eligible leaf record bodies instead.
    if(f->copy_depth.load(std::memory_order_relaxed)>1)
        return transform([&](const Value& v){return copy_value(v,false,depth+1);});
    // A private field table is scanned inline, just like the old representation.
    // Only actual shared snapshots need a separate graph vertex (otherwise their
    // outgoing shared_ptr edges would be counted once per wrapper).
    // Ordinary records keep their field body inline. Promote only when an
    // actual snapshot first shares it; no mutable field reference has escaped.
    if(auto inline_body=std::get_if<Flat>(&storage_)){
        auto body=std::make_shared<Flat>(std::move(*inline_body));
        storage_=std::move(body);f=flat();
    }
    // A table containing only scalars or scalar-typed container handles cannot
    // lead back into a record graph. Public container objects remain tracked;
    // this private backing body needs neither a vertex nor duplicate edges.
    if(!f->cycle_free&&!f->tracked.load(std::memory_order_relaxed)){
        bool safe=true;for(const auto& slot:f->slots)if(slot&&!acyclic_field(*slot)){safe=false;break;}
        f->cycle_free=safe;
    }
    if(!f->cycle_free&&!f->tracked.load(std::memory_order_relaxed)){
        managed_heap().track(std::get<std::shared_ptr<Flat>>(storage_),true);
        f->tracked.store(true,std::memory_order_release);
    }
    RecordFields result;result.storage_=std::get<std::shared_ptr<Flat>>(storage_);return result;
}

}
