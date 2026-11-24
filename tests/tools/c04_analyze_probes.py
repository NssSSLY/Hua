"""Join VM exact step probe + Native causal probe to produce C04 upper-bound & top-3 causal tables."""
import collections
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests" / "tools"))
from audit_bootstrap_phases import reduce_events  # noqa: E402
OUT = ROOT / "tests" / "artifacts" / "c04-revisit-20261124"


def short_fn(target: str) -> str:
    if "#" in target:
        mod, fn = target.split("#", 1)
        return f"{Path(mod).stem}.{fn}"
    return target or "<global>"


def main():
    base = json.loads((OUT / "ordinary-baseline/summary.json").read_text(encoding="utf-8"))
    vm_report = json.loads((OUT / "vm-probe/report.json").read_text(encoding="utf-8"))
    vm_events = [json.loads(line) for line in (OUT / "vm-probe/events.jsonl").read_text(encoding="utf-8").splitlines() if line.strip()]
    causal_path = OUT / "causal-probe/visits.jsonl"
    causal_rows = [json.loads(line) for line in causal_path.read_text(encoding="utf-8").splitlines() if line.strip()] if causal_path.exists() else []

    total_vm_steps = vm_report["steps"]
    c04_steps = vm_report["helper_work"]["analyze"]["inclusive"]
    print(f"=== ORDINARY BASELINE ===")
    print(f"Native: wall={base['native']['wall_seconds']:.3f}s cpu={base['native']['cpu_seconds']:.3f}s peak={base['native']['peak_rss_mib']}MiB")
    print(f"VM:     wall={base['vm']['wall_seconds']:.3f}s cpu={base['vm']['cpu_seconds']:.3f}s peak={base['vm']['peak_rss_mib']}MiB steps={base['vm_budget']['steps']:,}")
    print(f"C04 VM steps: {c04_steps:,} / {total_vm_steps:,} ({100.0 * c04_steps / total_vm_steps:.2f}%)")
    print(f"Actual passes: {vm_report['volumes'].get('actual-passes')} | Logical work: {vm_report['volumes'].get('logical-work')}")

    print(f"\n=== VM PHASE EXCLUSIVE BREAKDOWN (OBSERVED) ===")
    ph = reduce_events(vm_events)
    tot_ph_steps = sum(v["counters"]["vm_steps"] for v in ph.values())
    tot_ph_ns = sum(v["ns"] for v in ph.values())
    for k, v in sorted(ph.items(), key=lambda x: -x[1]["ns"]):
        st = v["counters"]["vm_steps"]
        ns = v["ns"]
        print(f"  {k:18s} steps={st:13,} ({100.0*st/max(1,tot_ph_steps):5.2f}%) obs_wall={ns/1e9:7.2f}s ({100.0*ns/max(1,tot_ph_ns):5.2f}%)")

    # Partition VM target_visits
    visits = vm_report["target_visits"]
    global_steps = sum(v["steps"] for v in visits if not v["target"])
    fn_visits = [v for v in visits if v["target"]]
    first_visit_steps = sum(v["steps"] for v in fn_visits if v["ordinal"] == 1)
    revisit_steps = sum(v["steps"] for v in fn_visits if v["ordinal"] > 1)
    final_nonprobing_steps = sum(v["steps"] for v in fn_visits if not v["probing"])
    other_c04_steps = c04_steps - global_steps - first_visit_steps - revisit_steps

    print(f"\n=== C04 VM STEP PARTITION ===")
    print(f"Total C04:          {c04_steps:13,} (100.00%)")
    print(f"Global bodies:      {global_steps:13,} ({100.0 * global_steps / c04_steps:6.2f}%)")
    print(f"Visit 1 (all fns):  {first_visit_steps:13,} ({100.0 * first_visit_steps / c04_steps:6.2f}%) [{sum(1 for v in fn_visits if v['ordinal']==1)} visits]")
    print(f"Revisits (ord>=2):  {revisit_steps:13,} ({100.0 * revisit_steps / c04_steps:6.2f}%) [{sum(1 for v in fn_visits if v['ordinal']>1)} visits]")
    print(f"  (non-probing fin):{final_nonprobing_steps:13,} ({100.0 * final_nonprobing_steps / c04_steps:6.2f}%)")
    print(f"Setup/summary/etc:  {other_c04_steps:13,} ({100.0 * other_c04_steps / c04_steps:6.2f}%)")
    if revisit_steps > 0:
        ub_no_revisits = c04_steps / (c04_steps - revisit_steps)
        print(f"UPPER BOUND if ALL revisits cost 0: C04 speedup = {ub_no_revisits:.2f}x (remaining {100.0*(c04_steps-revisit_steps)/c04_steps:.1f}%)")

    # Top helpers by exclusive VM steps
    print(f"\n=== TOP 20 HELPERS BY EXCLUSIVE VM STEPS ===")
    helpers_sorted = sorted(vm_report["helper_work"].items(), key=lambda kv: kv[1]["exclusive"], reverse=True)
    for name, w in helpers_sorted[:20]:
        print(f"  {name:24s} calls={w['calls']:9,} excl={w['exclusive']:12,} ({100.0*w['exclusive']/c04_steps:5.2f}%) incl={w['inclusive']:12,} ({100.0*w['inclusive']/c04_steps:5.2f}%)")

    # Group VM visits by function
    by_fn = collections.defaultdict(list)
    for v in fn_visits:
        by_fn[v["target"]].append(v)

    # Align causal rows with probing fn_visits
    probing_vm_visits = [v for v in fn_visits if v["probing"]]
    aligned = len(causal_rows) == len(probing_vm_visits)
    print(f"\nCausal rows={len(causal_rows)}, probing VM fn visits={len(probing_vm_visits)}, aligned={aligned}")
    causal_by_key = {}
    if aligned:
        for vm_v, c_v in zip(probing_vm_visits, causal_rows):
            assert vm_v["target"] == c_v["target"], (vm_v["target"], c_v["target"])
            causal_by_key[(vm_v["target"], vm_v["ordinal"])] = c_v

        noop_revisit_steps = 0
        noop_revisit_count = 0
        chg_revisit_steps = 0
        chg_revisit_count = 0
        noop_revisit_ns = 0
        total_causal_ns = 0
        first_causal_ns = 0
        revisit_causal_ns = 0
        orig_only_trig_steps = 0
        orig_only_trig_count = 0

        for vm_v, c_v in zip(probing_vm_visits, causal_rows):
            total_causal_ns += c_v["ns"]
            if vm_v["ordinal"] == 1:
                first_causal_ns += c_v["ns"]
            else:
                revisit_causal_ns += c_v["ns"]
                if not c_v["sum_changed"] and not c_v["err_changed"]:
                    noop_revisit_steps += vm_v["steps"]
                    noop_revisit_count += 1
                    noop_revisit_ns += c_v["ns"]
                else:
                    chg_revisit_steps += vm_v["steps"]
                    chg_revisit_count += 1
                # Check if ALL triggers were origin-only (no flag changes, no writes added/removed/flags, no req)
                trigs = list(c_v["triggers"].values())
                if trigs and all(("flg=0" in t or "ret.fields" not in t) and "writes:" not in t and "req:" not in t and "ret.root:flags" not in t and "INIT" not in t for t in trigs):
                    orig_only_trig_steps += vm_v["steps"]
                    orig_only_trig_count += 1

        print(f"\n=== REVISIT CAUSAL CLASSIFICATION (PROBING) ===")
        print(f"No-op revisits (output unchanged):  {noop_revisit_count:4d} visits, {noop_revisit_steps:13,} VM steps ({100.0*noop_revisit_steps/c04_steps:5.2f}% of C04), {noop_revisit_ns/1e9:6.2f}s native ({100.0*noop_revisit_ns/max(1,total_causal_ns):5.2f}% of body ns)")
        print(f"Output-changing revisits:           {chg_revisit_count:4d} visits, {chg_revisit_steps:13,} VM steps ({100.0*chg_revisit_steps/c04_steps:5.2f}% of C04)")
        print(f"Native body ns: total={total_causal_ns/1e9:.2f}s | visit1={first_causal_ns/1e9:.2f}s ({100.0*first_causal_ns/max(1,total_causal_ns):.1f}%) | revisits={revisit_causal_ns/1e9:.2f}s ({100.0*revisit_causal_ns/max(1,total_causal_ns):.1f}%)")
        print(f"UPPER BOUND if only NO-OP revisits eliminated: {c04_steps/(c04_steps-noop_revisit_steps):.2f}x VM C04")

        # Aggregate return-value discard vs read stats across all call sites
        total_inst_fields = sum(c["inst_fields"] for c in causal_rows)
        total_inst_views = sum(c["inst_views"] for c in causal_rows)
        total_inst_origins = sum(c["inst_origins"] for c in causal_rows)
        total_invokes = sum(c["invokes"] for c in causal_rows)
        total_loop_iters = sum(c["loop_iters"] for c in causal_rows)
        print(f"Volumes across all probing visits: invokes={total_invokes:,} inst_fields={total_inst_fields:,} inst_views={total_inst_views:,} inst_origins={total_inst_origins:,} loop_iters={total_loop_iters:,}")

    # Top 15 functions by total VM steps
    print(f"\n=== TOP 15 FUNCTIONS BY TOTAL VM STEPS ===")
    fn_totals = sorted(
        [(t, sum(v["steps"] for v in vs), sum(v["steps"] for v in vs if v["ordinal"] > 1), len(vs)) for t, vs in by_fn.items()],
        key=lambda x: x[1],
        reverse=True,
    )
    for t, tot, rev, cnt in fn_totals[:15]:
        ns_tot = sum(causal_by_key[(t, v["ordinal"])]["ns"] for v in by_fn[t] if (t, v["ordinal"]) in causal_by_key) / 1e9 if aligned else 0
        print(f"  {short_fn(t):38s} visits={cnt:2d} total={tot:12,} ({100.0*tot/c04_steps:5.2f}%) revisit={rev:12,} ({100.0*rev/c04_steps:5.2f}%) native={ns_tot:6.2f}s")

    # Detailed per-visit causal table for Top 5 functions
    print(f"\n=== DETAILED CAUSAL TABLE FOR TOP 5 FUNCTIONS ===")
    summary_json = {"top_functions": []}
    for t, tot, rev, cnt in fn_totals[:5]:
        print(f"\n--- {short_fn(t)} ({t}) | visits={cnt} | total={tot:,} steps ({100.0*tot/c04_steps:.2f}% C04) ---")
        fn_entry = {"target": t, "short": short_fn(t), "total_steps": tot, "revisit_steps": rev, "visits": []}
        for v in by_fn[t]:
            c_v = causal_by_key.get((t, v["ordinal"]))
            top_h = sorted(v["helpers"].items(), key=lambda kv: kv[1]["exclusive"], reverse=True)[:5]
            top_h_str = ", ".join(f"{k}:{w['exclusive']:,}({100.0*w['exclusive']/max(1,v['steps']):.0f}%)" for k, w in top_h)
            if c_v:
                trigs_short = {short_fn(k): val for k, val in c_v["triggers"].items()}
                # Summarize reads: how many sites discarded vs field-read vs bound/returned/arg
                discarded_sites = []
                field_sites = []
                other_sites = []
                for s_key, info in c_v["sites"].items():
                    reads = sorted(c_v["reads"].get(s_key, {}).keys())
                    if not reads or reads == ["DISCARDED"]:
                        discarded_sites.append((s_key, info, reads))
                    elif any(r.startswith("field:") for r in reads):
                        field_sites.append((s_key, info, reads))
                    else:
                        other_sites.append((s_key, info, reads))
                print(
                    f"  Visit #{v['ordinal']} (pass={c_v['pass']}): steps={v['steps']:,} native={c_v['ns']/1e6:.1f}ms "
                    f"work={c_v['work']} invokes={c_v['invokes']} inst_views={c_v['inst_views']:,} inst_orig={c_v['inst_origins']:,} "
                    f"loops={c_v['loop_enters']}/{c_v['loop_iters']}(max {c_v['max_loop_passes']})"
                )
                print(f"    Triggers ({len(trigs_short)}): {trigs_short}")
                print(f"    Sites: total={len(c_v['sites'])} discarded={len(discarded_sites)} field_read={len(field_sites)} bind/arg/ret={len(other_sites)}")
                for sk, inf, rds in (discarded_sites[:3] + field_sites[:3]):
                    print(f"      site {short_fn(sk)}: {inf} -> reads={rds[:6]}")
                print(f"    Output changed={c_v['sum_changed']} (err={c_v['err_changed']}): {c_v['out_diff']}")
                print(f"    Top helpers: {top_h_str}")
                fn_entry["visits"].append({
                    "ordinal": v["ordinal"],
                    "pass": c_v["pass"],
                    "vm_steps": v["steps"],
                    "native_ms": round(c_v["ns"] / 1e6, 2),
                    "work": c_v["work"],
                    "invokes": c_v["invokes"],
                    "inst_fields": c_v["inst_fields"],
                    "inst_views": c_v["inst_views"],
                    "inst_origins": c_v["inst_origins"],
                    "loop_enters": c_v["loop_enters"],
                    "loop_iters": c_v["loop_iters"],
                    "max_loop_passes": c_v["max_loop_passes"],
                    "triggers": trigs_short,
                    "sites_total": len(c_v["sites"]),
                    "sites_discarded": len(discarded_sites),
                    "sites_field_read": len(field_sites),
                    "sum_changed": c_v["sum_changed"],
                    "out_diff": c_v["out_diff"],
                    "top_helpers": {k: w["exclusive"] for k, w in top_h},
                })
            else:
                print(f"  Visit #{v['ordinal']} (probing={v['probing']}): steps={v['steps']:,} top_helpers={top_h_str}")
        summary_json["top_functions"].append(fn_entry)

    (OUT / "causal-analysis-summary.json").write_text(json.dumps(summary_json, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
