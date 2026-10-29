#include "hua/value.hpp"
#include <algorithm>
#include <bit>
#include <utility>
namespace hua {
namespace {
unsigned scalar_tag(const Value& value){
    if(std::holds_alternative<bool>(value.data))return 1;
    if(std::holds_alternative<std::int64_t>(value.data))return 2;
    if(std::holds_alternative<double>(value.data))return 3;
    if(auto p=std::get_if<NumericValue>(&value.data);p&&numeric_spec(p->type)&&p->type!="@integer")return 4;
    return 0;
}
bool encode(const Value& value,const PlainRecordLayout::Field& field,std::array<std::uint64_t,8>& cells,unsigned depth){
    if(field.child){
        PlainRecord child;
        if(auto p=std::get_if<PlainRecord>(&value.data))child=*p;
        else {auto boxed=std::get_if<StructValue>(&value.data);if(!boxed||!pack_plain_record(*boxed,child,depth))return false;}
        if(child.layout!=field.child)return false;
        std::copy_n(child.cells.begin(),child.layout->slots,cells.begin()+field.offset);return true;
    }
    if(scalar_tag(value)!=field.tag||value_type(value)!=field.type)return false;
    if(field.tag==1)cells[field.offset]=std::get<bool>(value.data);
    else if(field.tag==2)cells[field.offset]=std::bit_cast<std::uint64_t>(std::get<std::int64_t>(value.data));
    else if(field.tag==3)cells[field.offset]=std::bit_cast<std::uint64_t>(std::get<double>(value.data));
    else if(field.tag==4)cells[field.offset]=std::visit([](auto x)->std::uint64_t{return std::bit_cast<std::uint64_t>(x);},std::get<NumericValue>(value.data).number);
    else return false;
    return true;
}
bool fill(const StructValue& input,PlainRecord& output,unsigned depth){
    for(const auto& field:output.layout->fields){auto item=std::as_const(input.data->fields).find(field.name);if(item==std::as_const(input.data->fields).end()||!encode(item->second,field,output.cells,depth+1))return false;}
    return true;
}
}
bool pack_plain_record(const StructValue& input,PlainRecord& output,unsigned depth){
    if(depth>128)runtime_error({},"value nesting limit exceeded","E4099");
    if(input.data->enumeration)return false;
    auto shape=input.data->fields.retain_layout();
    if(!shape||shape->nominal_name!=input.data->name||shape->nominal_name.empty()||shape->plain_rejected.load(std::memory_order_relaxed)||input.data->fields.size()!=shape->names.size())return false;
    std::shared_ptr<const PlainRecordLayout> cached;{std::lock_guard lock(shape->plain_mutex);cached=shape->plain_cache.lock();}
    if(cached){
        if(depth+cached->depth>128)runtime_error({},"value nesting limit exceeded","E4099");
        output.layout=std::move(cached);output.writable=true;if(fill(input,output,depth))return true;
    }
    auto layout=std::make_shared<PlainRecordLayout>();layout->shape=shape;layout->name=input.data->name;
    for(const auto& name:shape->names){
        auto found=std::as_const(input.data->fields).find(name);if(found==std::as_const(input.data->fields).end())return false;
        const auto& value=found->second;PlainRecordLayout::Field field;field.name=name;field.type=value_type(value);field.offset=layout->slots;field.tag=scalar_tag(value);
        auto annotation=shape->annotations.find(name);
        if(annotation==shape->annotations.end()||annotation->second!=field.type)return false;
        if(field.tag){++layout->slots;layout->depth=std::max(layout->depth,1u);}
        else{
            PlainRecord child;
            if(auto packed=std::get_if<PlainRecord>(&value.data))child=*packed;
            else{auto record=std::get_if<StructValue>(&value.data);if(!record||!pack_plain_record(*record,child,depth+1)){
                shape->plain_rejected.store(true,std::memory_order_relaxed);return false;}}
            field.child=child.layout;layout->slots+=child.layout->slots;layout->depth=std::max(layout->depth,child.layout->depth+1);
        }
        if(layout->slots>8){shape->plain_rejected.store(true,std::memory_order_relaxed);return false;}
        layout->fields.push_back(std::move(field));
    }
    if(depth+layout->depth>128)runtime_error({},"value nesting limit exceeded","E4099");
    output.layout=layout;output.writable=true;if(!fill(input,output,depth))return false;
    // Weak cache avoids a Layout -> PlainLayout -> Layout ownership cycle.
    {std::lock_guard lock(shape->plain_mutex);shape->plain_cache=layout;}
    return true;
}
std::optional<Value> plain_record_field(const PlainRecord& input,const std::string& name){
    const auto& fields=input.layout->fields;
    auto found=std::lower_bound(fields.begin(),fields.end(),name,[](const auto& f,const auto& n){return f.name<n;});
    if(found==fields.end()||found->name!=name)return {};
    const auto& field=*found;
    if(field.child){PlainRecord child;child.layout=field.child;child.writable=input.writable;std::copy_n(input.cells.begin()+field.offset,child.layout->slots,child.cells.begin());return Value(std::move(child));}
    auto bits=input.cells[field.offset];
    if(field.tag==1)return Value(bits!=0);
    if(field.tag==2)return Value(std::bit_cast<std::int64_t>(bits));
    if(field.tag==3)return Value(std::bit_cast<double>(bits));
    auto spec=numeric_spec(field.type);if(!spec)return {};
    if(spec->category=='u')return Value(NumericValue{field.type,bits});
    if(spec->category=='i')return Value(NumericValue{field.type,std::bit_cast<std::int64_t>(bits)});
    return Value(NumericValue{field.type,std::bit_cast<double>(bits)});
}
StructValue* materialize_record(Value& value){
    if(auto input=std::get_if<PlainRecord>(&value.data)){
        // Publish only after the complete boxed record is ready. The old inline
        // snapshot remains usable if allocation or field construction throws.
        auto data=managed<StructData>();data->name=input->layout->name;data->fields=RecordFields(input->layout->shape);
        for(const auto& field:input->layout->fields)data->fields.emplace(field.name,*plain_record_field(*input,field.name));
        bool writable=input->writable;value=Value(StructValue{std::move(data),writable});
    }
    return std::get_if<StructValue>(&value.data);
}
void retain_record_alias(Value& value){
    // Values stored in shared containers may be observed through both readonly
    // and writable aliases. Materialize before exposing either view, so later
    // writes observe the original object rather than a newly captured snapshot.
    if(std::holds_alternative<PlainRecord>(value.data))materialize_record(value);
}
}
