"""BO-11 frozen scale evidence and admission. No automatic formal-run fallback.
Uses existing ordinary successes, real reduced closures and a held-out closure.
Native archive census supplies structural features, never Hua VM speed estimates.
"""
from pathlib import Path
import argparse,json,os,shutil,subprocess,sys,time
from audit_bootstrap_phases import command,digest,save,closure,machine,reduce_events

def load(p):return json.loads(Path(p).read_text(encoding='utf-8'))
def lines(p):return [json.loads(s) for s in Path(p).read_text(encoding='utf-8').splitlines()]
def guard(root,out):
 m=load(out/'manifest.json')
 assert {n:digest(p) for n,p in closure(root/'selfhost/archive_main.hua').items()}==m['compiler_sources']
 for p,sha in m['identities'].items():assert digest(root/p)==sha,p
 for p,sha in m['reused_files'].items():assert digest(root/p)==sha,p
 return m

def freeze(root,out):
 assert not out.exists(),'fresh directory required'
 prior=root/'tests/artifacts/build/bo10-complete-20261018';s=load(prior/'summary.json')
 assert s['status']=='BO-10 complete within Windows x64 Release Bridge OFF scope'
 assert all(c['status']=='passed' for c in s['cases'])
 out.mkdir(parents=True)
 binaries={'tests/artifacts/build/hua.exe':s['native_sha256'],'tests/artifacts/build-a11-profile/hua_memory_probe.exe':s['memory_probe_sha256'],'tests/artifacts/build-a11-profile/hua_performance_probe.exe':s['probe_sha256'],'tests/artifacts/build/bo10-complete-20261018/compiler.huab':s['compiler_sha256']}
 identities={**binaries}
 for p in ['tests/bootstrap_scale_probe.cpp','include/hua/archive.hpp','src/archive.cpp','selfhost/bytecode_verify.hua','selfhost/archive.hua','selfhost/lowering.hua']:
  identities[p]=digest(root/p)
 files=[prior/'summary.json',prior/'manifest.json',prior/'candidate-identity-final.json',prior/'final-audit.json',prior/'memory-compiler.huab']
 cases={}
 for c in s['cases']:
  name=c['case'];inp=load(root/'tests/artifacts/build/bo08-hua-sema-20261016/cases'/name/'result.json');source=Path(inp['input_path'])
  assert digest(source)==c['input_sha256']==inp['input_sha256']
  src=closure(source);files.extend(src.values());files.extend([prior/'cases'/name/'after.huab',prior/'commands'/(name+'-after.stdout'),prior/'commands'/(name+'-after.stderr')])
  for p in (prior/'commands').glob(name+'-*.json'):files.append(p)
  cases[name]={'entry':str(source),'input_sources':{str(p.relative_to(root)):digest(p) for p in src.values()},'ordinary':c['rows']['after'],'counter_diagnostic':c['rows']['diagnostic'],'artifact':str(prior/'cases'/name/'after.huab')}
 for name in ['type_rules','ast_pool']:
  files.append(prior/'memory'/name/'events.jsonl');files.append(prior/'memory'/name/'output.huab')
 sources=closure(root/'selfhost/archive_main.hua')
 for name,p in sources.items():
  q=out/'source'/name;q.parent.mkdir(exist_ok=True);shutil.copy2(p,q)
 reused={str(p.relative_to(root)):digest(p) for p in set(files)}
 for p,sha in binaries.items():assert digest(root/p)==sha
 caches={str(p):digest(root/p) for p in [Path('tests/artifacts/build/CMakeCache.txt'),Path('tests/artifacts/build-a11-profile/CMakeCache.txt')]}
 m={'schema':'hua.bootstrap.scale-audit.v1','created_utc':time.strftime('%Y-%m-%dT%H:%M:%SZ',time.gmtime()),'host':machine(),'git_head':subprocess.check_output(['git','-c','safe.directory=E:/project/hua','rev-parse','HEAD'],cwd=root,text=True).strip(),'identities':identities,'reused_files':reused,'compiler_sources':{n:digest(p) for n,p in sources.items()},'compiler_source_bytes':sum(p.stat().st_size for p in sources.values()),'cases':cases,'build_caches':caches,'protocol':{'training':['type_rules','ast_pool'],'holdout':['parser'],'features':['tokens','ast_nodes','instructions','functions','branches','module_count'],'phase_model':'Compare first-order work per phase and a function-square verifier candidate from code; hold-out error over 25 percent rejects that model. Report extrapolation limits and never treat envelope as statistical confidence.','prediction_error_limit_percent':25,'admission_seconds':400,'formal_limit_seconds':600,'production_vm_steps':500000000,'production_calls':128,'production_gc_interval':65536,'production_report_bytes':16777216,'formal_run_only_after_reliable_prediction':True,'first_failure_stops':True},'full_own_source_run':False}
 save(out/'manifest.json',m);print('Frozen BO-11 identities and 3 ordinary successful inputs',flush=True)

def census(root,out):
 m=guard(root,out);cc=root/'.tools/llvm-mingw-20260922-ucrt-x86_64/bin/clang++.exe';exe=out/'scale-census.exe'
 args=[cc,'-std=c++20','-O3','-DNDEBUG','-municode','-static','-I',root/'include',root/'tests/bootstrap_scale_probe.cpp',root/'tests/artifacts/build/libhua_semantics.a',root/'tests/artifacts/build/libhua_frontend.a','-lpthread',root/'.tools/wasmtime-v49.0.2-x86_64-windows-c-api/lib/wasmtime.dll.lib','-o',exe]
 command(root,out,'build-scale-census',args,60)
 records={}
 for name,c in m['cases'].items():
  _,stdout,_=command(root,out,'census-'+name,[exe,c['artifact']],20);records[name]=json.loads(stdout)
  native=out/(name+'-native.huab');command(root,out,'native-build-'+name,[root/'tests/artifacts/build/hua.exe','build',c['entry'],'-o',native],30)
  _,stdout,_=command(root,out,'census-native-'+name,[exe,native],20);records[name+'-native']=json.loads(stdout)
 _,stdout,_=command(root,out,'census-full-native',[exe,root/'tests/artifacts/build/bo10-complete-20261018/compiler.huab'],30);records['full-native']=json.loads(stdout)
 save(out/'census.json',{'probe_sha256':digest(exe),'rows':records,'native_full_is_structural_proxy_not_vm_execution':True})
 print(json.dumps(records,ensure_ascii=False),flush=True)

def phases(root,out,name):
 m=guard(root,out);assert name=='parser'
 work=out/'phases'/name;work.mkdir(parents=True,exist_ok=False)
 prior=root/'tests/artifacts/build/bo10-complete-20261018'
 args=[root/'tests/artifacts/build-a11-profile/hua_memory_probe.exe','--phase-events',work/'events.jsonl',prior/'memory-compiler.huab',m['cases'][name]['entry'],work/'output.huab']
 row,stdout,stderr=command(root,out,'phase-'+name,args,100)
 assert stdout==(prior/'commands'/(name+'-after.stdout')).read_bytes()
 assert (work/'output.huab').read_bytes()==Path(m['cases'][name]['artifact']).read_bytes()
 ev=lines(work/'events.jsonl');assert ev[-1]['phase']=='heap-release';execution=ev[:-2]
 reduced=reduce_events(execution)
 save(work/'result.json',{'status':'passed','timing':row,'counts':json.loads(stderr.splitlines()[0]),'metrics':json.loads(stderr.splitlines()[1]),'phases':reduced,'same_output_and_artifact':True,'native_allocation_observer':True})
 print(json.dumps({k:{'seconds':v['ns']/1e9,'steps':v['counters']['vm_steps']} for k,v in reduced.items()}),flush=True)

def source_table(path):
 import struct,zlib
 data=Path(path).read_bytes();assert data[:8]==b'HUAB\r\n\x1a\n'
 version,abi,size,crc,flags=struct.unpack_from('<IIQII',data,8)
 assert version in (6,7) and abi==1 and flags==0 and size==len(data)-32 and crc==zlib.crc32(data[32:])
 pos=32;n=struct.unpack_from('<I',data,pos)[0];pos+=4;assert 0<n<=128;rows={}
 def string():
  nonlocal pos
  length=struct.unpack_from('<I',data,pos)[0];pos+=4;assert length<=16777216 and pos+length<=len(data)
  value=data[pos:pos+length];pos+=length;return value
 for _ in range(n):
  name=string().decode('utf-8');body=string();assert name not in rows;rows[name]=body
 return rows

def feature(phase,c,variant):
 if phase in ('execution','codegen','publish'):return 1
 if phase in ('core-verify','emit','encode','bytecode-verify'):return c['instructions']
 if phase=='capabilities' and variant=='interaction':return c['ast_nodes']*c['source_functions']
 return c['ast_nodes']

def estimate(training,features,phase_data,variant,key):
 import math
 assert len(set(training))>=2,'at least two independent real training scales required'
 phases=list(phase_data[training[0]])
 result={}
 for phase in phases:
  if phase=='heap-release':continue
  rates=[phase_data[n][phase][key]/feature(phase,features[n],variant) for n in training]
  assert min(rates)>0
  result[phase]={'rate':math.exp(sum(math.log(r) for r in rates)/len(rates)),'min_rate':min(rates),'max_rate':max(rates),'feature':('instructions' if phase in ('core-verify','emit','encode','bytecode-verify') else 'constant' if phase in ('execution','codegen','publish') else 'ast_nodes*source_functions' if phase=='capabilities' and variant=='interaction' else 'ast_nodes')}
 return result

def phase_values(events):
 if events[-1]['phase']=='heap-release':events=events[:-2]
 return {k:{'steps':v['counters']['vm_steps'],'seconds':v['ns']/1e9,'entries':v['entries'],'copies':v['counters']['copy_struct_fields']} for k,v in reduce_events(events).items()}

def calibrate(root,out):
 m=guard(root,out);census=load(out/'census.json')['rows'];prior=root/'tests/artifacts/build/bo10-complete-20261018';features={n:census[n] for n in m['cases']}
 for n in features:
  a=census[n];b=census[n+'-native']
  for key in ['functions','instructions','opcodes','branches','max_code']:assert a[key]==b[key],(n,key)
 phases={n:phase_values(lines(prior/'memory'/n/'events.jsonl')) for n in ['type_rules','ast_pool']}
 phases['parser']=phase_values(lines(out/'phases/parser/events.jsonl'))
 native=out/'modules-native.huab';command(root,out,'native-build-modules',[root/'tests/artifacts/build/hua.exe','build',root/'selfhost/modules.hua','-o',native],30)
 _,stdout,_=command(root,out,'census-native-modules',[out/'scale-census.exe',native],20);features['modules']=json.loads(stdout)
 # Count only actual imported Hua source syntax, excluding native std declarations.
 # Preserve each parser result and verify all C0 embedded user-source bytes.
 user=closure(root/'selfhost/archive_main.hua');extra=closure(root/'selfhost/modules.hua');assert all(n in user for n in extra)
 table=source_table(prior/'compiler.huab');embedded={Path(n).name:body for n,body in table.items() if not n.startswith('<')}
 assert set(embedded)==set(user)
 source_stats={}
 for name,path in user.items():
  assert embedded[name]==path.read_bytes(),name
  _,stdout,_=command(root,out,'source-ast-'+path.stem,[root/'tests/artifacts/build/hua_parser_oracle.exe',path],20)
  row=json.loads(stdout);assert row['status']=='ok';nodes=row['nodes']
  source_stats[name]={'ast_nodes':len(nodes),'source_functions':sum(n['kind']=='Function' for n in nodes),'depth':max(n['depth'] for n in nodes),'sha256':digest(path)}
 def actual(entry):
  return {'ast_nodes':sum(source_stats[n]['ast_nodes'] for n in closure(entry)),'source_functions':sum(source_stats[n]['source_functions'] for n in closure(entry)),'modules':len(closure(entry)),'source_bytes':sum(f.stat().st_size for f in closure(entry).values())}
 features['full']={**census['full-native'],**actual(root/'selfhost/archive_main.hua'),'source_bytes':m['compiler_source_bytes']}
 features['modules'].update(actual(root/'selfhost/modules.hua'))
 save(out/'source-census.json',{'rows':source_stats,'embedded_c0_matches_all_current_user_sources':True,'native_standard_declarations_excluded_from_source_features':True,'oracle_sha256':digest(root/'tests/artifacts/build/hua_parser_oracle.exe')})
 # The first-order FAMILY and threshold were specified before Parser measurement.
 # Its numerical model is reconstructed after Parser was seen: retrospective only.
 # The interaction candidate gets a genuinely NEW held-out module closure.
 first=estimate(['type_rules','ast_pool'],features,phases,'first-order','steps')
 errors={k:100*(v['rate']*feature(k,features['parser'],'first-order')/phases['parser'][k]['steps']-1) for k,v in first.items()}
 model={}
 for key in ['steps','seconds']:model[key]=estimate(['type_rules','ast_pool','parser'],features,phases,'interaction',key)
 predictions={key:{k:{'central':v['rate']*feature(k,features['modules'],'interaction'),'empirical_min':v['min_rate']*feature(k,features['modules'],'interaction'),'empirical_max':v['max_rate']*feature(k,features['modules'],'interaction')} for k,v in rows.items()} for key,rows in model.items()}
 data={'status':'frozen before new modules VM execution','training':['type_rules','ast_pool','parser'],'holdout':'modules','features':features,'phase_data':phases,'first_order_parser_errors_percent':errors,'first_order_rejected':any(abs(e)>25 for e in errors.values()),'interaction_model':model,'modules_predictions':predictions,'candidate_note':'N*source functions for capabilities is an empirical interaction proxy, not a proven algorithmic complexity or mathematical bound. F*(F+1) metadata table visits are proven from code and listed separately.','error_limit_percent':25,'supervision_seconds':150,'no_formal_run_admitted':True}
 save(out/'prediction-before-modules.json',data)
 print(json.dumps({'first_order_parser_errors_percent':errors,'modules_steps_central':sum(v['central'] for v in predictions['steps'].values()),'modules_phase_seconds_central':sum(v['central'] for v in predictions['seconds'].values()),'features':{n:{k:c[k] for k in ['modules','ast_nodes','source_functions','functions','instructions']} for n,c in features.items()}},ensure_ascii=False),flush=True)

def validate(root,out):
 m=guard(root,out);p=load(out/'prediction-before-modules.json');assert p['status']=='frozen before new modules VM execution'
 work=out/'phases/modules';work.mkdir(parents=True,exist_ok=False);prior=root/'tests/artifacts/build/bo10-complete-20261018';source=root/'selfhost/modules.hua'
 normal,stdout,stderr=command(root,out,'modules-ordinary',[root/'tests/artifacts/build/hua.exe','run',prior/'compiler.huab','--execution-profile','compiler','--report-budget','--',source,work/'ordinary.huab'],150)
 receipt=json.loads(stdout);budget=json.loads(stderr);assert receipt['status']=='ok' and budget['steps']<=500000000 and budget['peak_calls']<=128
 memory,ms,me=command(root,out,'modules-phases',[root/'tests/artifacts/build-a11-profile/hua_memory_probe.exe','--phase-events',work/'events.jsonl',prior/'memory-compiler.huab',source,work/'output.huab'],150)
 assert ms==stdout and (work/'ordinary.huab').read_bytes()==(work/'output.huab').read_bytes()
 ev=lines(work/'events.jsonl');assert ev[-1]['phase']=='heap-release' and all(e['managed_live']==0 for e in ev if e['event']=='end' and e['phase'] in ('execution','heap-release'))
 phase=phase_values(ev)
 _,cs,_=command(root,out,'census-modules-hua',[out/'scale-census.exe',work/'ordinary.huab'],20);actual=json.loads(cs)
 for key in ['modules','ast_nodes','source_functions','functions','instructions','opcodes']:assert actual[key]==p['features']['modules'][key],(key,actual[key],p['features']['modules'][key])
 command(root,out,'reader-modules',[root/'tests/artifacts/build/hua.exe','check',work/'ordinary.huab'],20)
 save(work/'result.json',{'status':'passed','ordinary':normal,'receipt':receipt,'budget':budget,'diagnostic':memory,'metrics':json.loads(me.splitlines()[1]),'phases':phase,'all_artifact_bytes_equal':True,'native_structural_proxy_validated':True,'prediction_identity':digest(out/'prediction-before-modules.json'),'managed_roots_at_session_end':0})
 print('New modules closure completed once per purpose, artifact and reader verified',flush=True)


def decide(*,step_errors,wall_errors,guarded_seconds,projected_steps,correctness,structural_verified,ordinary_timing_verified=False,call_depth_verified=False,report_limit_verified=False):
 import math
 reasons=[]
 values=[guarded_seconds,projected_steps,*step_errors.values(),*wall_errors.values()]
 if any(not math.isfinite(v) for v in values):reasons.append('nonfinite prediction or error')
 if guarded_seconds<0 or projected_steps<0:reasons.append('negative projected work or time')
 if not ordinary_timing_verified:reasons.append('ordinary phase timing prediction not calibrated')
 if not call_depth_verified:reasons.append('full-scale call-depth margin not certified')
 if not report_limit_verified:reasons.append('full-scale report limit not certified')
 if not correctness:reasons.append('correctness regression evidence missing')
 if not structural_verified:reasons.append('source/structural identity not verified')
 if not step_errors or not wall_errors:reasons.append('independent holdout errors missing')
 if any(abs(v)>25 for v in step_errors.values()):reasons.append('phase step model failed 25 percent holdout gate')
 if any(abs(v)>25 for v in wall_errors.values()):reasons.append('phase wall model failed 25 percent holdout gate')
 if guarded_seconds>400:reasons.append('planning envelope exceeds 400 second admission margin')
 if projected_steps>500000000:reasons.append('candidate step estimate exceeds unchanged 500M production budget')
 return {'admitted':not reasons,'reasons':reasons,'formal_attempts_authorized_by_this_audit':1 if not reasons else 0}

def display_features(c):
 return {k:c[k] for k in ['modules','source_bytes','ast_nodes','source_functions','functions','instructions','max_code','branches','opcodes']}

def analyze(root,out):
 m=guard(root,out);p=load(out/'prediction-before-modules.json');v=load(out/'phases/modules/result.json')
 assert v['status']=='passed' and v['prediction_identity']==digest(out/'prediction-before-modules.json')
 full=display_features(p['features']['full']);errors={};forecast={}
 for metric in ['steps','seconds']:
  error={phase:100*(row['central']/v['phases'][phase][metric]-1) for phase,row in p['modules_predictions'][metric].items()}
  errors[metric]=error
  forecast[metric]={phase:{tag:row[key]*feature(phase,full,'interaction') for tag,key in [('central','rate'),('empirical_min','min_rate'),('empirical_max','max_rate')]} for phase,row in p['interaction_model'][metric].items()}
 totals={metric:{key:sum(phase[key] for phase in rows.values()) for key in ['central','empirical_min','empirical_max']} for metric,rows in forecast.items()}
 guarded=1+1.25*totals['seconds']['empirical_max']
 # An optimistic linear transfer from the largest measured real input is a
 # sensitivity calculation only. It is neither a lower bound nor a valid model.
 optimistic={phase:row['steps']/feature(phase,p['features']['modules'],'first-order')*feature(phase,full,'first-order') for phase,row in v['phases'].items()}
 decision=decide(step_errors=errors['steps'],wall_errors=errors['seconds'],guarded_seconds=guarded,projected_steps=totals['steps']['central'],correctness=True,structural_verified=True)
 assert not decision['admitted'],'formal acceptance must be a separate, explicitly reviewed action'
 prior=load(root/'tests/artifacts/build/bo10-complete-20261018/summary.json');cases=[]
 assert len(prior['ctests'])==30 and prior['c04']['cases']==174 and prior['c04']['checks']==2686
 for name,c in m['cases'].items():
  a=c['ordinary']['timing'];d=c['counter_diagnostic'];phase=p['phase_data'][name]
  budget=load(root/'tests/artifacts/build/bo10-complete-20261018/commands'/(name+'-after.stderr'))
  assert budget['steps']==d['counts']['vm_steps'] and budget['peak_calls']==d['metrics']['peak_calls']
  if name=='parser':diagnostic=load(out/'phases/parser/result.json')['timing']
  else:diagnostic=next(x for x in prior['memory'] if x['case']==name)['timing']
  cases.append({'case':name,'features':display_features(p['features'][name]),'ordinary_wall_seconds':a['seconds'],'ordinary_cpu_seconds':a['cpu_seconds'],'ordinary_peak_working_bytes':a['peak_working_bytes'],'ordinary_steps':budget['steps'],'phase_marker_step_delta':sum(r['steps'] for r in phase.values())-budget['steps'],'peak_calls':d['metrics']['peak_calls'],'phase_diagnostic_wall_seconds':diagnostic['seconds'],'phase_observer_wall_overhead_percent':100*(diagnostic['seconds']/a['seconds']-1),'reused_ordinary_success':True,'phases':phase})
 cases.append({'case':'modules','features':{**display_features(p['features']['modules']),'source_bytes':sum(f.stat().st_size for f in closure(root/'selfhost/modules.hua').values())},'ordinary_wall_seconds':v['ordinary']['seconds'],'ordinary_cpu_seconds':v['ordinary']['cpu_seconds'],'ordinary_peak_working_bytes':v['ordinary']['peak_working_bytes'],'ordinary_steps':v['budget']['steps'],'phase_marker_step_delta':sum(r['steps'] for r in v['phases'].values())-v['budget']['steps'],'peak_calls':v['budget']['peak_calls'],'phase_diagnostic_wall_seconds':v['diagnostic']['seconds'],'phase_observer_wall_overhead_percent':100*(v['diagnostic']['seconds']/v['ordinary']['seconds']-1),'reused_ordinary_success':False,'phases':v['phases']})
 # Preserve diagnostic phase counts as observations, including their own GC.
 details={}
 for name,events_path in [('parser',out/'phases/parser/events.jsonl'),('modules',out/'phases/modules/events.jsonl')]:
  ev=lines(events_path);reduced=reduce_events(ev[:-2]);details[name]={}
  assert sum(row['counters']['vm_steps'] for row in reduced.values())==ev[-3]['counters']['vm_steps']
  for phase,row in reduced.items():
   c=row['counters'];details[name][phase]={'seconds':row['ns']/1e9,'gc_seconds_in_same_diagnostic_run':c['gc_nanoseconds']/1e9,'steps':c['vm_steps'],'managed_allocations':c['managed_allocations'],'copied_struct_fields':c['copy_struct_fields'],'type_argument_parses':c['type_argument_parses']}
 meta_visits={name:c['functions']*(c['functions']+1) for name,c in p['features'].items()}
 old_sema=load(root/'tests/artifacts/build/bo08-hua-sema-20261016/summary.json');assert old_sema['current_source_hashes']==m['compiler_sources']
 counts=old_sema['parser_candidate_diagnostic_counts']
 save(out/'prediction-evaluation.json',{'first_order_parser_errors_percent':p['first_order_parser_errors_percent'],'new_holdout_phase_errors_percent':errors,'new_holdout_aggregate_step_error_percent':100*(sum(r['central'] for r in p['modules_predictions']['steps'].values())/sum(r['steps'] for r in v['phases'].values())-1),'new_holdout_aggregate_wall_error_percent':100*(sum(r['central'] for r in p['modules_predictions']['seconds'].values())/sum(r['seconds'] for r in v['phases'].values())-1),'both_models_rejected':True,'full_candidate_forecast_by_phase':forecast,'full_candidate_totals':totals,'planning_guarded_seconds':guarded,'guarded_calculation':'1 second startup allowance + 1.25 * empirical maximum diagnostic phase envelope; NOT a certified ordinary runtime bound','optimistic_linear_steps_sensitivity':sum(optimistic.values()),'optimistic_linear_is_not_lower_bound':True,'forecasts_valid_for_admission':False})
 commands=[load(path) for path in sorted((out/'commands').glob('*.json'))];assert all(c['returncode']==0 and not c['timeout'] for c in commands)
 data={'schema':'hua.bootstrap.admission.v1','status':'BO-11 scale prediction and admission judgment completed; formal run NOT admitted','decision':decision,'first_order_methodology_note':'Initial manifest predeclared node/instruction candidate family and 25 percent threshold; numerical first-order formula reconstructed AFTER Parser observation, so only retrospective transfer check. Only the new Modules prediction was numerically frozen before its VM executions.','feature_provenance_note':'Census native raw source-table fields include std declarations. Displayed/modelled AST nodes, source functions, module count and bytes use actual user-source closure; opcodes/code sizes use native generated-image proxy. Raw pre-Modules source_bytes for Modules included std bytes but was never a model variable; frozen prediction is preserved unchanged.','BO11_prediction_judgment_complete':True,'BO_G4_passed':False,'BO_G5_or_A11_or_C07_own_source_complete':False,'full_own_source_runs':0,'generations':0,'native_sha256':m['identities']['tests/artifacts/build/hua.exe'],'compiler_sha256':m['identities']['tests/artifacts/build/bo10-complete-20261018/compiler.huab'],'compiler_sources':m['compiler_sources'],'full_features':full,'cases':cases,'forecast_totals':totals,'planning_guarded_seconds':guarded,'forecast_warning':'Failed holdout models. These are rejected planning scenarios, not observed full-source results, probability intervals, proven bounds or an ordinary runtime forecast.','holdout_errors_percent':errors,'metadata_table_visit_counts':meta_visits,'metadata_work_provenance':'bytecode_verify.hua:85,213-214,278-279: one full F-row function snapshot/scan per F+1 code chunks. This proves visits, not quadratic total time or dominant hotspot share.','phase_cost_details':details,'historical_same_compiler_semantic_counts':{'source':'tests/artifacts/build/bo08-hua-sema-20261016/summary.json','reused_without_vm_rerun':True,'all_14_source_hashes_match':True,'timings_not_reused':True,'parser_scheduler':old_sema['parser_scheduler'],'parser_selected_counts':{k:counts[k] for k in ['snapshot_bindings','snapshot_fields','compare_json','compare_json_bytes','shape_instantiated_fields','alias_scans','alias_fields']}},'gates':{'G0':'fixed current runtime/C0, unchanged full-source closure, two prediction hypotheses','G1':'observed exclusive phase/steps, known prior ordinary CPU sample, failed out-of-sample growth models; complete full workload attribution still absent','G2_G3':'BO03/04/08-native/09/10 selected local contracts retained, previous affected regressions reused; BO08 Hua copy reduction had no overall speedup claim','G4':'NOT passed: per-phase held-out prediction errors exceed threshold; no reliable full-scale time, step or call-depth margin','G5':'not attempted; independent reader/source-removed generated compiler still requires an admitted formal run'},'regressions_reused':{'source':'tests/artifacts/build/bo10-complete-20261018/summary.json','ctest_count':30,'c04_cases':174,'c04_checks':2686,'production_code_changed_this_task':False},'supervised_commands':len(commands),'supervised_seconds':sum(c['seconds'] for c in commands),'timeouts':0,'failed_workload_commands':0,'audit_preflight_stop':'tests/artifacts/build/bo11-scale-20261018/preflight-stop.json: summary status schema mismatch; stopped before workloads, corrected once','next_work':{'priority_1':'derive semantic work model from per-function/module body visits, actual dirty dependencies, snapshot/clone field volume and shape/path widths; distinguish module partition from richer imported code before selecting a fix','priority_2':'review indexed parameter metadata instead of F*(F+1) complete function snapshots; preserve exact first diagnostic, unreachable instruction validation, globals/parameters checks, HUAB and public budgets','priority_3':'measure ordinary phase timing with a bounded low-overhead observer and explicit perturbation control only if time prediction still needed; memory-allocation instrumented timings are not production phase times','reopen_admission_only_after':'targeted correctness and genuinely new holdout phase error <=25%, conservatively certified <=400s and <=500M steps, call/report limits accounted; never increase production budgets'},'scope_limits':['Windows x64 Release Bridge OFF only','No production/compiler code changed; no full regression suite rerun','AST/source depths do not certify VM peak call depth at full scale','Full native archive supplies structural proxy, not Hua VM execution or self-compilation evidence','Native and Hua small-image names/types differ; equality asserted only exact structural fields and same-Hua archive bytes across ordinary/diagnostic, not native/Hua full bytes','No commit, push, install or PATH change']}
 save(out/'summary.json',data);print(json.dumps({'status':data['status'],'decision':decision,'case_steps':{c['case']:c['ordinary_steps'] for c in cases},'forecast_totals':totals,'guarded_diagnostic_seconds':guarded,'optimistic_linear_sensitivity_steps':sum(optimistic.values()),'commands':len(commands),'supervised_seconds':data['supervised_seconds']},ensure_ascii=False),flush=True)


def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('action',choices=['freeze','census','phases','calibrate','validate','analyze']);p.add_argument('--out',required=True);p.add_argument('--case',default='parser');a=p.parse_args();root=Path(__file__).resolve().parents[2];out=Path(a.out).resolve()
 if a.action=='freeze':freeze(root,out)
 elif a.action=='census':census(root,out)
 elif a.action=='phases':phases(root,out,a.case)
 elif a.action=='calibrate':calibrate(root,out)
 elif a.action=='validate':validate(root,out)
 else:analyze(root,out)
if __name__=='__main__':main()
