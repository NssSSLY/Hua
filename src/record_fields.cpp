#include "hua/value.hpp"
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
    if(auto* f=std::get_if<Flat>(&storage_)){auto i=f->layout->index(name);return value_at(i)?iterator(this,i):end();}
    return iterator(this,std::get<Tree>(storage_).find(name));
}
RecordFields::const_iterator RecordFields::find(const std::string& name)const{
    if(auto* f=std::get_if<Flat>(&storage_)){auto i=f->layout->index(name);return value_at(i)?const_iterator(this,i):end();}
    return const_iterator(this,std::get<Tree>(storage_).find(name));
}
Value& RecordFields::at(const std::string& name){auto item=find(name);if(item==end())throw std::out_of_range("unknown record field");return item->second;}
const Value& RecordFields::at(const std::string& name)const{auto item=find(name);if(item==end())throw std::out_of_range("unknown record field");return item->second;}
std::pair<RecordFields::iterator,bool> RecordFields::emplace(const std::string& name,Value value){
    if(auto* f=std::get_if<Flat>(&storage_)){
        auto i=f->layout->index(name);if(i==RecordLayout::missing)throw std::out_of_range("field is absent from record layout");
        if(f->slots.empty())f->slots.resize(f->layout->names.size());
        if(f->slots[i])return {iterator(this,i),false};
        f->slots[i].emplace(std::move(value));++f->count;return {iterator(this,i),true};
    }
    auto [item,inserted]=std::get<Tree>(storage_).emplace(name,std::move(value));return {iterator(this,item),inserted};
}
RecordFields::iterator RecordFields::erase(iterator where){
    if(auto* f=std::get_if<Flat>(&storage_)){auto i=where.position_;f->slots.at(i).reset();--f->count;return iterator(this,i+1);}
    return iterator(this,std::get<Tree>(storage_).erase(where.tree_));
}
}
