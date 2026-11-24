#pragma once
// Diagnostic-only host semantic analysis. Ordinary builds have no meters or hooks.
#ifdef HUA_SEMA_PROFILE
#include <array>
#include <algorithm>
#include <type_traits>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <map>
#include <ostream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>
namespace hua::sema_profile {
enum class Area { other, binary_eager, binary_logic, branch_snapshot, loop_snapshot, scope_restore, global_snapshot, function_reset, summary_snapshot, summary_store, join, fields, alias, exit, returns, apply, closure, call_actuals, unknown, count };
enum class Kind { info, origin, symbol, count };
enum class Phase { setup, summaries, checking, count };
constexpr std::size_t areas=std::size_t(Area::count), metrics=6, phases=std::size_t(Phase::count);
using Counts=std::array<std::array<std::uint64_t,metrics>,areas>;
struct FunctionRow {std::string name;unsigned pass{};bool summary{},return_changed{},effect_changed{};double seconds{};Counts copies{};std::map<std::pair<std::string,unsigned>,std::uint64_t> reads;};
struct PassRow {unsigned pass{},changed_returns{},changed_effects{},functions{};bool changed{};};
struct Counters {Counts copies{};std::array<Counts,phases> phase_copies{};std::array<std::uint64_t,areas> area_calls{};std::vector<FunctionRow> functions;std::vector<PassRow> passes;std::map<std::string,unsigned> versions;unsigned mutating_passes{},return_passes{},summary_fallbacks{};std::uint64_t joins{},alias_scans{},alias_symbols{},alias_views{},exits{},loop_passes{},field_requests{},field_hits{},field_builds{},max_views{},max_origins{};std::array<std::size_t,3> layouts{};};
inline thread_local Counters* active=nullptr;
inline thread_local Area area=Area::other;
inline thread_local Phase phase=Phase::setup;
inline thread_local FunctionRow* function=nullptr;
inline void copy(Kind kind,bool assignment)noexcept{if(active){auto k=2*std::size_t(kind)+assignment;++active->copies[std::size_t(area)][k];++active->phase_copies[std::size_t(phase)][std::size_t(area)][k];}}
template<Kind kind>struct CopyMeter {
 CopyMeter()=default;CopyMeter(const CopyMeter&)noexcept{copy(kind,false);}CopyMeter& operator=(const CopyMeter&)noexcept{copy(kind,true);return *this;}
 CopyMeter(CopyMeter&&)noexcept=default;CopyMeter& operator=(CopyMeter&&)noexcept=default;
 bool operator==(const CopyMeter&)const noexcept{return true;}
};
struct AreaScope {Area old;explicit AreaScope(Area next):old(area){area=next;if(active)++active->area_calls[std::size_t(next)];}~AreaScope(){area=old;}};
struct PhaseScope {Phase old;explicit PhaseScope(Phase next):old(phase){phase=next;}~PhaseScope(){phase=old;}};
struct Scope {Counters* old;Area old_area;Phase old_phase;FunctionRow* old_function;explicit Scope(Counters& value):old(active),old_area(area),old_phase(phase),old_function(function){active=&value;area=Area::other;phase=Phase::setup;function=nullptr;}~Scope(){active=old;area=old_area;phase=old_phase;function=old_function;}};
template<class T>T snapshot(const T& value,Area context){AreaScope guard(context);return value;}
template<class T>void restore(T& destination,const T& value,Area context){AreaScope guard(context);destination=value;}
struct FunctionScope {
 using Clock=std::chrono::steady_clock;FunctionRow row;FunctionRow* previous;Counts before{};Clock::time_point start;
 FunctionScope(const std::string& name,unsigned pass,bool summary):previous(function),start(Clock::now()){row.name=name;row.pass=pass;row.summary=summary;function=&row;if(active)before=active->copies;}
 ~FunctionScope(){if(active){row.seconds=std::chrono::duration<double>(Clock::now()-start).count();for(std::size_t a=0;a<areas;++a)for(std::size_t k=0;k<metrics;++k)row.copies[a][k]=active->copies[a][k]-before[a][k];active->functions.push_back(std::move(row));}function=previous;}
};
inline void dependency(const std::string& name){if(active&&function)++function->reads[{name,active->versions[name]}];}
inline void summary_change(const std::string& name,bool returns,bool effects){if(active&&function){function->return_changed=returns;function->effect_changed=effects;if(returns||effects)++active->versions[name];}}
inline void finish_pass(unsigned pass,bool changed){if(active){PassRow row;row.pass=pass;row.changed=changed;for(auto it=active->functions.rbegin();it!=active->functions.rend()&&it->summary&&it->pass==pass;++it){++row.functions;row.changed_returns+=it->return_changed;row.changed_effects+=it->effect_changed;}active->passes.push_back(row);}}
inline void dimensions(std::size_t views,std::size_t origins){if(active){active->max_views=std::max(active->max_views,std::uint64_t(views));active->max_origins=std::max(active->max_origins,std::uint64_t(origins));}}
inline void counts(std::ostream& out,const Counts& values){
 const char* names[]={"other","binary_eager","binary_logic","branch_snapshot","loop_snapshot","scope_restore","global_snapshot","function_reset","summary_snapshot","summary_store","join","fields","alias","exit","returns","apply","closure","call_actuals","unknown"};
 out<<'{';bool first=true;for(std::size_t a=0;a<areas;++a){bool nonzero=false;for(auto v:values[a])nonzero|=v!=0;if(!nonzero)continue;if(!first)out<<',';first=false;out<<std::quoted(names[a])<<": [";for(std::size_t k=0;k<metrics;++k){if(k)out<<',';out<<values[a][k];}out<<']';}out<<'}';
}
inline void write(std::ostream& out,const Counters& c){
 out<<std::setprecision(12)<<"{\"schema\":\"hua.sema.scale.v1\",\"metrics\":[\"info_ctor\",\"info_assign\",\"origin_ctor\",\"origin_assign\",\"symbol_ctor\",\"symbol_assign\"],\"copies\":";counts(out,c.copies);
 out<<",\"phases\":[";for(std::size_t p=0;p<phases;++p){if(p)out<<',';counts(out,c.phase_copies[p]);}out<<"],\"area_calls\":[";for(std::size_t a=0;a<areas;++a){if(a)out<<',';out<<c.area_calls[a];}out<<"],\"passes\":[";
 for(std::size_t i=0;i<c.passes.size();++i){if(i)out<<',';const auto& p=c.passes[i];out<<"{\"pass\":"<<p.pass<<",\"functions\":"<<p.functions<<",\"changed_returns\":"<<p.changed_returns<<",\"changed_effects\":"<<p.changed_effects<<",\"changed\":"<<(p.changed?"true":"false")<<'}';}
 out<<"],\"mutating_passes\":"<<c.mutating_passes<<",\"return_passes\":"<<c.return_passes<<",\"summary_fallbacks\":"<<c.summary_fallbacks<<",\"joins\":"<<c.joins<<",\"alias_scans\":"<<c.alias_scans<<",\"alias_symbols\":"<<c.alias_symbols<<",\"alias_views\":"<<c.alias_views<<",\"exits\":"<<c.exits<<",\"loop_passes\":"<<c.loop_passes<<",\"field_requests\":"<<c.field_requests<<",\"field_hits\":"<<c.field_hits<<",\"field_builds\":"<<c.field_builds<<",\"max_views\":"<<c.max_views<<",\"max_origins\":"<<c.max_origins<<",\"layouts\":["<<c.layouts[0]<<','<<c.layouts[1]<<','<<c.layouts[2]<<"],\"functions\":[";
 for(std::size_t i=0;i<c.functions.size();++i){if(i)out<<',';const auto& f=c.functions[i];out<<"{\"name\":"<<std::quoted(f.name)<<",\"pass\":"<<f.pass<<",\"summary\":"<<(f.summary?"true":"false")<<",\"return_changed\":"<<(f.return_changed?"true":"false")<<",\"effect_changed\":"<<(f.effect_changed?"true":"false")<<",\"seconds\":"<<f.seconds<<",\"copies\":";counts(out,f.copies);out<<",\"reads\":[";bool first=true;for(const auto& [key,n]:f.reads){if(!first)out<<',';first=false;out<<'['<<std::quoted(key.first)<<','<<key.second<<','<<n<<']';}out<<"]}";}out<<"]}";
}
}
#define HUA_SEMA_MEMBER(kind) [[no_unique_address]] ::hua::sema_profile::CopyMeter<::hua::sema_profile::Kind::kind> copy_meter_{};
#define HUA_SEMA_AREA(name) ::hua::sema_profile::AreaScope hua_sema_area(::hua::sema_profile::Area::name)
#define HUA_SEMA_PHASE(name) ::hua::sema_profile::PhaseScope hua_sema_phase(::hua::sema_profile::Phase::name)
#define HUA_SEMA_SNAPSHOT(value,name) ::hua::sema_profile::snapshot(value,::hua::sema_profile::Area::name)
#define HUA_SEMA_RESTORE(destination,value,name) ::hua::sema_profile::restore(destination,value,::hua::sema_profile::Area::name)
#define HUA_SEMA_BINARY(value,op) ::hua::sema_profile::snapshot(value,(op=="&&"||op=="||")?::hua::sema_profile::Area::binary_logic : ::hua::sema_profile::Area::binary_eager)
#define HUA_SEMA_COUNT(name) do{if(::hua::sema_profile::active)++::hua::sema_profile::active->name;}while(false)
#define HUA_SEMA_ADD(name,value) do{if(::hua::sema_profile::active)::hua::sema_profile::active->name+=(value);}while(false)
#define HUA_SEMA_FUNCTION(name,pass,summary) ::hua::sema_profile::FunctionScope hua_sema_function(name,pass,summary)
#else
#define HUA_SEMA_MEMBER(kind)
#define HUA_SEMA_AREA(name) ((void)0)
#define HUA_SEMA_PHASE(name) ((void)0)
#define HUA_SEMA_SNAPSHOT(value,name) (value)
#define HUA_SEMA_RESTORE(destination,value,name) ((destination)=(value))
#define HUA_SEMA_BINARY(value,op) (value)
#define HUA_SEMA_COUNT(name) ((void)0)
#define HUA_SEMA_ADD(name,value) ((void)0)
#define HUA_SEMA_FUNCTION(name,pass,summary) ((void)0)
#endif
