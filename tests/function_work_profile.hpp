#pragma once
#include "hua/performance.hpp"
#include <array>
#include <unordered_map>
#include <string>
#include <ostream>
#include <iomanip>
#include <stdexcept>
namespace hua::test_work {
using Values=std::array<std::uint64_t,6>;
inline Values snapshot(performance::Counters& c){return {c.vm_steps.load(),c.managed_allocations.load(),c.copy_struct_fields.load(),c.deep_clone_nodes.load(),c.type_argument_parses.load(),c.gc_nanoseconds.load()};}
struct Row {std::string name,file;Values exclusive{};};
struct Session;
struct Instruction;
inline thread_local Session* active{};
inline thread_local Instruction* top{};
struct Session {
 performance::Counters& counters;std::unordered_map<const void*,Row> rows;
 explicit Session(performance::Counters& c):counters(c){if(active||top)throw std::runtime_error("nested work session");active=this;}
 ~Session(){active=nullptr;}
 void write(std::ostream& out){
  out<<"{\"schema\":\"hua.function.work.v1\",\"boundary\":\"opcode owner including outgoing call setup; recursive dispatch excluded from parent\",\"rows\":[";bool first=true;
  for(const auto& [_,r]:rows){if(!first)out<<',';first=false;out<<"{\"name\":"<<std::quoted(r.name)<<",\"file\":"<<std::quoted(r.file)<<",\"work\":[";for(std::size_t i=0;i<r.exclusive.size();++i){if(i)out<<',';out<<r.exclusive[i];}out<<"]}";}
  out<<"]}";
 }
};
struct Instruction {
 Session* session{};Instruction* parent{};Row* row{};Values before{},children{};
 Instruction(const void* code,const std::string& name,const std::string& file):session(active){
  if(!session)return;
  auto [i,created]=session->rows.try_emplace(code);row=&i->second;
  if(created){row->name=name;row->file=file;}
  before=snapshot(session->counters);parent=top;top=this;
 }
 ~Instruction(){
  if(!session)return;auto after=snapshot(session->counters);
  for(std::size_t i=0;i<before.size();++i){auto total=after[i]-before[i];row->exclusive[i]+=total-children[i];if(parent)parent->children[i]+=total;}
  top=parent;
 }
};
}
