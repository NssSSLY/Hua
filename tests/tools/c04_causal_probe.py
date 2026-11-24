"""Build and run a Native C04 causal revisit probe on the frozen 14-module input.

Outputs:
- Exact phase breakdown (wall ns) for the native compiler on the frozen input.
- Per-visit causal JSONL record for every function visit in capabilities.analyze:
  triggering callee summary channel diffs, call-site read usage & eager instantiation counts,
  caller output summary/fact diffs, loop pass counts, and wall/work/helper costs.
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "tests" / "artifacts" / "c04-revisit-20261124"


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def build_instrumented_capabilities(src_text: str) -> str:
    """Transform a copy of capabilities.hua to emit per-visit causal telemetry to stderr/file."""
    text = src_text
    # Add std.time and std.fs imports
    text = text.replace(
        "import std.json as json\n",
        "import std.json as json\nimport std.fs as pfs\nimport std.os as pos\n",
        1,
    )

    # Add probe_site to Value so we can track which call-site results are actually read and which fields
    text = text.replace(
        "    readonly_fields bool\n    positions []Value }",
        "    readonly_fields bool\n    positions []Value\n    probe_site string }",
        1,
    )
    text = text.replace(
        'fn value(type string, writable bool) Value { return Value{root:view(type,writable),fields:map[string]View{},prefixes:map[string]map[string]bool{},prefix:"",readonly_fields:false,positions:[]} }',
        'fn value(type string, writable bool) Value { return Value{root:view(type,writable),fields:map[string]View{},prefixes:map[string]map[string]bool{},prefix:"",readonly_fields:false,positions:[],probe_site:""} }',
        1,
    )
    text = text.replace(
        'var out=Value{root:instantiate_view_at(site,source.root,actual),fields:map[string]View{},prefixes:clone(logical.prefixes),prefix:"",readonly_fields:false,positions:clone(source.positions)}',
        'var out=Value{root:instantiate_view_at(site,source.root,actual),fields:map[string]View{},prefixes:clone(logical.prefixes),prefix:"",readonly_fields:false,positions:clone(source.positions),probe_site:""}',
        1,
    )

    # Add probe counters & causal state to State
    probe_state_fields = """    probing bool
    work int
    error string
    p_invoke int
    p_inst_at int
    p_inst_fields int
    p_inst_views int
    p_inst_origins int
    p_put int
    p_merge int
    p_merge_fields int
    p_alias int
    p_alias_scanned int
    p_field_defaults int
    p_fd_entries int
    p_loop_enters int
    p_loop_iters int
    p_max_loop_passes int
    p_select int
    p_site_reads map[string]map[string]bool
    p_site_info map[string]string
    p_triggers map[string]map[string]string
    p_logs List<string> }"""
    text = text.replace(
        "    probing bool\n    work int\n    error string }",
        probe_state_fields,
        1,
    )

    # Initialize probe fields in State constructor
    text = text.replace(
        'probing:true,work:0,error:""}',
        'probing:true,work:0,error:"",p_invoke:0,p_inst_at:0,p_inst_fields:0,p_inst_views:0,p_inst_origins:0,p_put:0,p_merge:0,p_merge_fields:0,p_alias:0,p_alias_scanned:0,p_field_defaults:0,p_fd_entries:0,p_loop_enters:0,p_loop_iters:0,p_max_loop_passes:0,p_select:0,p_site_reads:map[string]map[string]bool{},p_site_info:map[string]string{},p_triggers:map[string]map[string]string{},p_logs:list.from_slice(_p_empty_str())}',
        1,
    )

    # Track dynamic reads on Value.probe_site
    # In select_root / field_fact: we don't have `s`, so let's pass `s` only where easy, OR track in expression_body/invoke/statement where `s` IS available!
    # Look where `s` is available in capabilities.hua:
    # - expression_body (Member/Index/Slice -> base.probe_site + path! Assignment -> right.probe_site! Call -> args[i].probe_site!)
    # - invoke (s is mut State! We can record site_info, summary.writes, summary.required, summary.returned.fields, inst_views, inst_origins, and tag result.probe_site!)
    # - statement (Let/Var/Const -> input.probe_site! Return -> items[i].probe_site! While/For -> loop_enters, loop_iters, max_loop_passes!)
    # - field_defaults (s is mut State!)

    # Instrument field_defaults
    text = text.replace(
        "fn field_defaults(s mut State, base View, level int) map[string]View {\n",
        "fn field_defaults(s mut State, base View, level int) map[string]View {\n    s.p_field_defaults+=1\n",
        1,
    )
    text = text.replace(
        "    return result\n}\nfn seeded(",
        "    s.p_fd_entries+=len(result)\n    return result\n}\nfn seeded(",
        1,
    )

    # Instrument invoke: count calls, instantiate_at fields, instantiate_view_at origins, tag result.probe_site, record site metadata
    old_invoke_end = """    var result=instantiate_at(site,summary.returned,actual)
    result.root.type_name=f.result
    return ok(result)"""
    new_invoke_end = """    s.p_invoke+=1
    let p_logical_ret=materialized(summary.returned)
    s.p_inst_at+=1
    s.p_inst_fields+=len(p_logical_ret.fields)
    var p_iv=1+len(p_logical_ret.fields)+len(summary.writes)
    var p_io=len(summary.returned.root.origins)
    for unused_k, pf in p_logical_ret.fields { p_io+=len(pf.origins) }
    for unused_k, pf in summary.writes { p_io+=len(pf.origins) }
    s.p_inst_views+=p_iv
    s.p_inst_origins+=p_io
    var result=instantiate_at(site,summary.returned,actual)
    result.root.type_name=f.result
    let p_call_tag=f.target+"@"+str(n.id.index)
    result.probe_site=p_call_tag
    if !has(s.p_site_info,p_call_tag) {
        s.p_site_info[p_call_tag]=f.target+"|rf="+str(len(p_logical_ret.fields))+"|rp="+str(len(summary.returned.positions))+"|w="+str(len(summary.writes))+"|req="+str(len(summary.required))+"|unk="+str(summary.unknown)+"|io="+str(p_io)
    }
    return ok(result)"""
    assert old_invoke_end in text, "old_invoke_end not found"
    text = text.replace(old_invoke_end, new_invoke_end, 1)

    # Track probe_site reads in expression_body:
    # 1) Member / Index / Slice
    old_member = """    if k=="Member" || k=="Index" || k=="Slice" { let base=expression(s,e,n.children[0])?"""
    new_member = """    if k=="Member" || k=="Index" || k=="Slice" { let base=expression(s,e,n.children[0])?
        s.p_select+=1
        if base.probe_site!="" {
            if k=="Member" { _p_note_read(s,base.probe_site,"field:#@"+text) }
            else { _p_note_read(s,base.probe_site,"field:#@[]") }
        }"""
    assert old_member in text, "old_member not found"
    text = text.replace(old_member, new_member, 1)

    # Preserve probe_site across Propagate & Call builtins (unwrap/unwrap_or/ok/err/copied)
    old_prop = """    if k=="Propagate" { var out=expression(s,e,n.children[0])?"""
    new_prop = """    if k=="Propagate" { var out=expression(s,e,n.children[0])?
        if out.probe_site!="" { _p_note_read(s,out.probe_site,"propagate") }"""
    assert old_prop in text, "old_prop not found"
    text = text.replace(old_prop, new_prop, 1)

    # Track Call arguments using a call result
    old_call_args = """        for i in 1..len(n.children) { list.append(items,expression(s,e,n.children[i])?)? }
        let args=list.snapshot(items)"""
    new_call_args = """        for i in 1..len(n.children) { let p_arg=expression(s,e,n.children[i])?
            if p_arg.probe_site!="" { _p_note_read(s,p_arg.probe_site,"arg:"+callee.root.type_name+"#"+str(i-1)) }
            list.append(items,p_arg)? }
        let args=list.snapshot(items)"""
    assert old_call_args in text, "old_call_args not found"
    text = text.replace(old_call_args, new_call_args, 1)

    # Track Return / ExpressionStatement / Let / Assignment / While / For
    old_ret = """        for child in n.children { list.append(values,expression(s,e,child)?)? }"""
    new_ret = """        for child in n.children { let p_rv=expression(s,e,child)?
            if p_rv.probe_site!="" { _p_note_read(s,p_rv.probe_site,"return") }
            list.append(values,p_rv)? }"""
    assert old_ret in text, "old_ret not found"
    text = text.replace(old_ret, new_ret, 1)

    old_expr_stmt = """    if k=="ExpressionStatement" { expression(s,e,n.children[0])?
        return ok() }"""
    new_expr_stmt = """    if k=="ExpressionStatement" { let p_ev=expression(s,e,n.children[0])?
        if p_ev.probe_site!="" { _p_note_read(s,p_ev.probe_site,"DISCARDED") }
        return ok() }"""
    assert old_expr_stmt in text, "old_expr_stmt not found"
    text = text.replace(old_expr_stmt, new_expr_stmt, 1)

    old_let = """    if k=="Let" || k=="Var" || k=="Const" { var input=expression(s,e,n.children[len(n.children)-1])?"""
    new_let = """    if k=="Let" || k=="Var" || k=="Const" { var input=expression(s,e,n.children[len(n.children)-1])?
        if input.probe_site!="" { _p_note_read(s,input.probe_site,"bind:"+declaration(s.declarations,e.unit.id,n)) }"""
    assert old_let in text, "old_let not found"
    text = text.replace(old_let, new_let, 1)

    old_assign_stmt = """        if k=="Assignment" { let right=expression(s,e,n.children[1])?
            if text=="=" { assign(s.refs,e,left,right)? } }"""
    new_assign_stmt = """        if k=="Assignment" { let right=expression(s,e,n.children[1])?
            if right.probe_site!="" { _p_note_read(s,right.probe_site,"assign") }
            if text=="=" { assign(s.refs,e,left,right)? } }"""
    assert old_assign_stmt in text, "old_assign_stmt not found"
    text = text.replace(old_assign_stmt, new_assign_stmt, 1)

    # Track loop passes in While/For
    old_loop = """        var changed=true
        var passes=0
        while changed && passes<32 {
            passes+=1"""
    new_loop = """        s.p_loop_enters+=1
        var changed=true
        var passes=0
        while changed && passes<32 {
            passes+=1
            s.p_loop_iters+=1
            if passes>s.p_max_loop_passes { s.p_max_loop_passes=passes }"""
    assert old_loop in text, "old_loop not found"
    text = text.replace(old_loop, new_loop, 1)

    # Instrument analyze() loop to record per-visit causal table + flush to file specified by HUA_C04_PROBE_OUT
    old_analyze_loop = """        for name in order { let f=unwrap(functions[name])
            if !has(s.dirty,name) { continue }
            delete(s.dirty,name)
            delete(s.errors,name)
            var e=environment(unwrap(s.units[f.module]),name,globals)
            for p in f.parameters { store(e,p.target,parameter_seeded(s,p.type_name,p.writable,p.target))
                if p.writable { e.visible[p.target]=true } }
            let n=get(e.unit.tree.pool,ast.NodeId{pool:e.unit.tree.pool.identity,index:f.node})
            statement(s,e,n.children[1])?
            record_dependencies(s,e)
            let next=summary(e)
            if !has(s.summaries,name) || !same_summary(next,unwrap(s.summaries[name])) { changed=true
                s.summaries[name]=next
                if has(s.dependents,name) { for caller, unused in unwrap(s.dependents[name]) { s.dirty[caller]=true } } }
        } }"""
    new_analyze_loop = """        for name in order { let f=unwrap(functions[name])
            if !has(s.dirty,name) { continue }
            delete(s.dirty,name)
            let p_prev_err=unwrap_or(s.errors[name],"")
            delete(s.errors,name)
            let p_had_prev=has(s.summaries,name)
            var p_prev_sum=Summary{returned:value("",false),unknown:false,saw_return:false,writes:map[string]View{},required:map[string]bool{}}
            if p_had_prev { p_prev_sum=unwrap(s.summaries[name]) }
            let p_trig=unwrap_or(s.p_triggers[name],map[string]string{})
            delete(s.p_triggers,name)
            s.p_site_reads=map[string]map[string]bool{}
            s.p_site_info=map[string]string{}
            let p_w0=s.work
            let p_inv0=s.p_invoke
            let p_ia0=s.p_inst_at
            let p_if0=s.p_inst_fields
            let p_iv0=s.p_inst_views
            let p_io0=s.p_inst_origins
            let p_fd0=s.p_field_defaults
            let p_fde0=s.p_fd_entries
            let p_le0=s.p_loop_enters
            let p_li0=s.p_loop_iters
            let p_sel0=s.p_select
            s.p_max_loop_passes=0
            var e=environment(unwrap(s.units[f.module]),name,globals)
            for p in f.parameters { store(e,p.target,parameter_seeded(s,p.type_name,p.writable,p.target))
                if p.writable { e.visible[p.target]=true } }
            let n=get(e.unit.tree.pool,ast.NodeId{pool:e.unit.tree.pool.identity,index:f.node})
            statement(s,e,n.children[1])?
            record_dependencies(s,e)
            let next=summary(e)
            let p_sum_changed=!p_had_prev || !same_summary(next,p_prev_sum)
            let p_diff=_p_diff_summary(p_had_prev,p_prev_sum,next)
            let p_err_changed=unwrap_or(s.errors[name],"")!=p_prev_err
            let p_row=_p_visit_json(passes,true,name,f.module,0,s.work-p_w0,s.p_invoke-p_inv0,s.p_inst_at-p_ia0,s.p_inst_fields-p_if0,s.p_inst_views-p_iv0,s.p_inst_origins-p_io0,s.p_field_defaults-p_fd0,s.p_fd_entries-p_fde0,s.p_loop_enters-p_le0,s.p_loop_iters-p_li0,s.p_max_loop_passes,s.p_select-p_sel0,p_sum_changed,p_err_changed,p_diff,p_trig,s.p_site_info,s.p_site_reads)
            list.append(s.p_logs,p_row)?
            if p_sum_changed { changed=true
                s.summaries[name]=next
                if has(s.dependents,name) { for caller, unused in unwrap(s.dependents[name]) { s.dirty[caller]=true
                    var tr=map[string]string{}
                    if has(s.p_triggers,caller) { tr=clone(unwrap(s.p_triggers[caller])) }
                    tr[name]=p_diff
                    s.p_triggers[caller]=tr } } }
        } }"""
    assert old_analyze_loop in text, "old_analyze_loop not found"
    text = text.replace(old_analyze_loop, new_analyze_loop, 1)

    # Flush probe logs right before returning Model
    old_ret_model = "    return ok(Model{facts:s.facts,passes:passes,work:s.work,error:s.error})"
    new_ret_model = """    let p_out_opt=pos.getenv("HUA_C04_PROBE_OUT")?
    let p_out=unwrap_or(p_out_opt,"")
    if p_out!="" {
        pfs.write_text(p_out,strings.join(list.snapshot(s.p_logs),"\\n")+"\\n")?
    }
    return ok(Model{facts:s.facts,passes:passes,work:s.work,error:s.error})"""
    assert old_ret_model in text, "old_ret_model not found"
    text = text.replace(old_ret_model, new_ret_model, 1)

    # Append helper functions at end of capabilities.hua
    helpers = """
fn _p_empty_str() []string { var x []string=[]
    return x }
fn _p_note_read(s mut State, site string, what string) {
    var m=map[string]bool{}
    if has(s.p_site_reads,site) { m=unwrap(s.p_site_reads[site]) }
    else { s.p_site_reads[site]=m }
    m[what]=true
}
fn _p_diff_view_kind(a View, b View) string {
    var flags=a.writable!=b.writable || a.readonly_children!=b.readonly_children || a.deep_owned!=b.deep_owned || a.record_copy!=b.record_copy || a.type_name!=b.type_name
    var origs=a.origins!=b.origins
    if flags && origs { return "flags+orig" }
    if flags { return "flags" }
    if origs { return "orig("+str(len(a.origins))+"->"+str(len(b.origins))+")" }
    return "eq"
}
fn _p_diff_summary(had_prev bool, prev Summary, next Summary) string {
    if !had_prev {
        let nr=materialized(next.returned)
        return "INIT|rf="+str(len(nr.fields))+"|rp="+str(len(next.returned.positions))+"|w="+str(len(next.writes))+"|req="+str(len(next.required))
    }
    var empty []string=[]
    var parts=list.from_slice(empty)
    if prev.unknown!=next.unknown { unwrap(list.append(parts,"unk:"+str(prev.unknown)+"->"+str(next.unknown))) }
    if prev.saw_return!=next.saw_return { unwrap(list.append(parts,"saw:"+str(prev.saw_return)+"->"+str(next.saw_return))) }
    if !same_view(prev.returned.root,next.returned.root) {
        unwrap(list.append(parts,"ret.root:"+_p_diff_view_kind(prev.returned.root,next.returned.root)))
    }
    let pr=materialized(prev.returned)
    let nr=materialized(next.returned)
    var rf_add=0
    var rf_del=0
    var rf_chg=0
    var rf_flag_chg=0
    var rf_orig_only=0
    var rf_sample=""
    for k, v in nr.fields {
        if !has(pr.fields,k) { rf_add+=1
            if rf_sample=="" { rf_sample="+"+k }
        } else if !same_view(unwrap(pr.fields[k]),v) {
            rf_chg+=1
            let dk=_p_diff_view_kind(unwrap(pr.fields[k]),v)
            if strings.starts_with(dk,"orig") { rf_orig_only+=1 } else { rf_flag_chg+=1 }
            if rf_sample=="" { rf_sample="~"+k+":"+dk }
        }
    }
    for k, unused_v in pr.fields { if !has(nr.fields,k) { rf_del+=1 } }
    if rf_add>0 || rf_del>0 || rf_chg>0 {
        unwrap(list.append(parts,"ret.fields:+"+str(rf_add)+"/-"+str(rf_del)+"/~"+str(rf_chg)+"(flg="+str(rf_flag_chg)+",orig="+str(rf_orig_only)+")["+rf_sample+"]"))
    }
    if len(prev.returned.positions)!=len(next.returned.positions) {
        unwrap(list.append(parts,"ret.pos_len:"+str(len(prev.returned.positions))+"->"+str(len(next.returned.positions))))
    } else {
        var pos_chg=0
        for i in 0..len(next.returned.positions) {
            if !same_value(prev.returned.positions[i],next.returned.positions[i]) { pos_chg+=1 }
        }
        if pos_chg>0 { unwrap(list.append(parts,"ret.pos:~"+str(pos_chg))) }
    }
    var w_add=0
    var w_del=0
    var w_chg=0
    var w_flag_chg=0
    var w_orig_only=0
    var w_sample=""
    for k, v in next.writes {
        if !has(prev.writes,k) { w_add+=1
            if w_sample=="" { w_sample="+"+k }
        } else if !same_view(unwrap(prev.writes[k]),v) {
            w_chg+=1
            let dk=_p_diff_view_kind(unwrap(prev.writes[k]),v)
            if strings.starts_with(dk,"orig") { w_orig_only+=1 } else { w_flag_chg+=1 }
            if w_sample=="" { w_sample="~"+k+":"+dk }
        }
    }
    for k, unused_v in prev.writes { if !has(next.writes,k) { w_del+=1 } }
    if w_add>0 || w_del>0 || w_chg>0 {
        unwrap(list.append(parts,"writes:+"+str(w_add)+"/-"+str(w_del)+"/~"+str(w_chg)+"(flg="+str(w_flag_chg)+",orig="+str(w_orig_only)+")["+w_sample+"]"))
    }
    var req_add=0
    var req_del=0
    var req_chg=0
    for k, v in next.required {
        if !has(prev.required,k) { req_add+=1 }
        else if unwrap(prev.required[k])!=v { req_chg+=1 }
    }
    for k, unused_v in prev.required { if !has(next.required,k) { req_del+=1 } }
    if req_add>0 || req_del>0 || req_chg>0 {
        unwrap(list.append(parts,"req:+"+str(req_add)+"/-"+str(req_del)+"/~"+str(req_chg)))
    }
    if list.len(parts)==0 { return "NONE" }
    return strings.join(list.snapshot(parts),"; ")
}
struct _PVisit { pass int
    probing bool
    target string
    module string
    ns int
    work int
    invokes int
    inst_at int
    inst_fields int
    inst_views int
    inst_origins int
    field_defaults int
    fd_entries int
    loop_enters int
    loop_iters int
    max_loop_passes int
    selects int
    sum_changed bool
    err_changed bool
    out_diff string
    triggers map[string]string
    sites map[string]string
    reads map[string]map[string]bool }
fn _p_visit_json(pass int, probing bool, target string, module string, ns int, work int, invokes int, inst_at int, inst_fields int, inst_views int, inst_origins int, field_defaults int, fd_entries int, loop_enters int, loop_iters int, max_loop_passes int, selects int, sum_changed bool, err_changed bool, out_diff string, triggers map[string]string, sites map[string]string, reads map[string]map[string]bool) string {
    return unwrap(json.encode(_PVisit{pass:pass,probing:probing,target:target,module:module,ns:ns,work:work,invokes:invokes,inst_at:inst_at,inst_fields:inst_fields,inst_views:inst_views,inst_origins:inst_origins,field_defaults:field_defaults,fd_entries:fd_entries,loop_enters:loop_enters,loop_iters:loop_iters,max_loop_passes:max_loop_passes,selects:selects,sum_changed:sum_changed,err_changed:err_changed,out_diff:out_diff,triggers:triggers,sites:sites,reads:reads}))
}
"""
    return text + helpers


def main():
    frozen = OUT / "frozen"
    assert frozen.exists(), "Run c04_baseline_freeze.py first"
    probe_dir = OUT / "causal-probe"
    if probe_dir.exists():
        shutil.rmtree(probe_dir)
    probe_src = probe_dir / "src"
    probe_src.mkdir(parents=True)
    for p in frozen.glob("*.hua"):
        shutil.copyfile(p, probe_src / p.name)

    cap_path = probe_src / "capabilities.hua"
    cap_path.write_text(build_instrumented_capabilities(cap_path.read_text(encoding="utf-8")), encoding="utf-8", newline="\n")

    # Also instrument phase timings in archive_main.hua (just C04 vs total)
    env = os.environ.copy()
    env["HUA_HOME"] = str(ROOT / "huaBin")
    env["HUA_RELEASE_CACHE"] = str(OUT / "release-cache")

    exe = probe_dir / "probe-archive.exe"
    print("[1/2] Building native causal probe via hua build --release...", flush=True)
    t0 = time.perf_counter()
    p = subprocess.run(
        [str(ROOT / "huaBin/bin/hua.exe"), "build", "--release", str(probe_src / "archive_main.hua"), "-o", str(exe)],
        cwd=str(ROOT),
        env=env,
        capture_output=True,
        text=True,
        encoding="utf-8",
        timeout=600,
    )
    print(f"Build rc={p.returncode} time={time.perf_counter()-t0:.2f}s", flush=True)
    if p.returncode != 0:
        print("STDOUT:", p.stdout[-2000:])
        print("STDERR:", p.stderr[-2000:])
        raise SystemExit(1)

    probe_jsonl = probe_dir / "visits.jsonl"
    out_huab = probe_dir / "probe.huab"
    env["HUA_C04_PROBE_OUT"] = str(probe_jsonl)
    print("[2/2] Running native causal probe on frozen input...", flush=True)
    t1 = time.perf_counter()
    r = subprocess.run(
        [str(exe), str(frozen / "archive_main.hua"), str(out_huab)],
        cwd=str(ROOT),
        env=env,
        capture_output=True,
        text=True,
        encoding="utf-8",
        timeout=600,
    )
    print(f"Run rc={r.returncode} time={time.perf_counter()-t1:.2f}s", flush=True)
    for line in r.stdout.splitlines():
        if line.startswith("@@") or line.startswith("{"):
            print("  OUT:", line, flush=True)
    if r.returncode != 0:
        print("STDOUT:", r.stdout[-2000:])
        print("STDERR:", r.stderr[-2000:])
        raise SystemExit(1)

    base_summary = json.loads((OUT / "ordinary-baseline/summary.json").read_text(encoding="utf-8"))
    probe_sha = sha256(out_huab)
    assert probe_sha == base_summary["artifact_sha256"], f"Probe changed HUAB! {probe_sha} != {base_summary['artifact_sha256']}"
    print(f"Verified byte-identical HUAB: {probe_sha[:16]}; visits written to {probe_jsonl}", flush=True)


if __name__ == "__main__":
    main()
