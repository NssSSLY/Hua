"""Development runner: validate/publish reports; parsing and analysis live in Hua."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
ROOT = Path(__file__).resolve().parents[1]

def emit_error(code, stage, message, exit_code, source="", profile="C02-P1"):
    schema="hua.bytecode.image.v1" if profile=="C06-BC1" else "hua.core.lowered.v1" if profile=="C05-L1" else "hua.types.capabilities.v1" if profile=="C04-T1" else "hua.names.modules.v1" if profile=="C03-N1" else "hua.ast.slice.v1"
    print(json.dumps({"schema":schema,"profile":profile,"status":"error",
                      "stage":stage,"source":source,"error":message,"code":code},ensure_ascii=False),file=sys.stderr)
    return exit_code

class CommandParser(argparse.ArgumentParser):
    def error(self,message):
        emit_error("E8204","command",message,2)
        raise SystemExit(2)

def validate_nodes(report):
    nodes=report["nodes"];budget=report["stats"]["pool_nodes"]
    if len(nodes)>budget:raise ValueError("AST exceeds reported node count")
    kinds={"Program","Block","Let","Var","Const","Function","Parameter","ReturnTypes","TypeName","Integer","Float","Boolean","Name","Unary","Binary","Assignment","Call","If","While","Return","Break","Continue","ExpressionStatement"}
    if report["profile"]=="C02-P2":
        kinds.update({"String","Nil","Struct","Field","Import","For","Update","Index","Slice","Member","Array","StructLiteral","FieldInit","Range","Omitted","SliceType","ArrayType","GenericType","OptionalType","MutableType","MapLiteral","MapEntry","Pack","BindingList","MultiBinding","MultiAssignment","Propagate","Defer","Enum","Variant","Match","MatchArm"})
    seen=set();expected=[(nodes[0].get("id"),0)]
    if nodes[0].get("kind")!="Program":raise ValueError("AST root is not Program")
    for n in nodes:
        if not isinstance(n,dict):raise ValueError("invalid AST record")
        for field in ("id","depth","start","end","line","column"):
            if type(n.get(field)) is not int:raise ValueError("invalid AST integer field")
        if not 0<=n["id"]<budget or n["id"] in seen or not expected or expected.pop()!=(n["id"],n["depth"]):raise ValueError("invalid AST identity/order")
        if n["kind"] not in kinds or not isinstance(n.get("text"),str):raise ValueError("invalid AST variant")
        if not 0<=n["start"]<=n["end"]<=report["stats"]["input_bytes"] or n["line"]<1 or n["column"]<1:raise ValueError("invalid AST source span")
        children=n.get("children")
        if not isinstance(children,list) or any(type(x) is not int or not 0<=x<n["id"] for x in children):raise ValueError("invalid AST children")
        seen.add(n["id"])
        expected.extend((x,n["depth"]+1) for x in reversed(children))
    if expected:raise ValueError("AST has missing children")

def validate_modules(report,contract):
    limits=contract["analysis"]["budgets"]
    units=report["modules"];edges=report["edges"];symbols=report["symbols"];refs=report["references"]
    if not isinstance(units,list) or not 1<=len(units)<=limits["modules"]:raise ValueError("invalid module count")
    def integer(item,key,upper):
        if type(item.get(key)) is not int or not 0<=item[key]<=upper:raise ValueError("invalid analysis counter: "+key)
    ids={};total=0
    for unit in units:
        name=unit["id"]
        if not isinstance(name,str) or not name or "\\" in name or ":" in name or name.startswith("/") or any(x in ("",".","..") for x in name.split("/")) or name in ids:raise ValueError("invalid logical module identity")
        for key,budget in (("pool_nodes","nodes"),("tokens","tokens"),("input_bytes","source_bytes")):integer(unit,key,contract["budgets"][budget])
        ids[name]=unit;total+=unit["input_bytes"]
    if report["source"]!=units[0]["id"]:raise ValueError("entry module mismatch")
    integer(report["stats"],"work",limits["work"])
    integer(report["stats"],"input_bytes",limits["source_bytes_total"])
    if report["stats"]["input_bytes"]!=total:raise ValueError("module byte count mismatch")
    order=report["initialization"]
    if not isinstance(order,list) or len(order)!=len(ids) or set(order)!=set(ids):raise ValueError("invalid initialization order")
    positions={x:i for i,x in enumerate(order)}
    def node(item):
        module=item.get("module",item.get("source"))
        if module not in ids:raise ValueError("unknown module in record")
        integer(item,"node",ids[module]["pool_nodes"]-1)
    if not isinstance(edges,list) or len(edges)>len(ids)*contract["budgets"]["tokens"]:raise ValueError("invalid import edges")
    namespaces=set()
    for edge in edges:
        node(edge);namespace=edge["namespace"]
        if not isinstance(namespace,str) or not re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*(?:\.[A-Za-z_][A-Za-z_0-9]*)*",namespace):raise ValueError("invalid namespace")
        pair=(edge["source"],namespace)
        if pair in namespaces:raise ValueError("duplicate namespace")
        namespaces.add(pair)
        target=edge["target"]
        if target in ids:
            if positions[target]>=positions[edge["source"]]:raise ValueError("invalid dependency order")
        elif target not in {"builtin:"+x for x in contract["analysis"]["standard_modules"]}:raise ValueError("invalid import target")
    if not isinstance(symbols,list) or len(symbols)>limits["symbols"]:raise ValueError("invalid symbols")
    targets=set()
    for item in symbols:
        node(item);integer(item,"scope",contract["budgets"]["nodes"])
        if type(item.get("published")) is not bool or not isinstance(item.get("name"),str) or not item["name"]:raise ValueError("invalid declaration")
        if item["kind"] not in {"Function","Struct","Enum","Variant","Let","Var","Const","let","var","Parameter","For","Pattern"}:raise ValueError("invalid declaration kind")
        expected=item["module"]+("#s"+str(item["scope"])+":" if item["scope"] else "#")+item["name"]
        if item.get("target")!=expected or expected in targets or (item["scope"] and item["published"]):raise ValueError("invalid symbol identity")
        targets.add(expected)
    if not isinstance(refs,list) or len(refs)>limits["references"]:raise ValueError("invalid references")
    for item in refs:
        node(item)
        if not isinstance(item.get("name"),str) or not item["name"] or type(item.get("type_name")) is not bool:raise ValueError("invalid reference")
        target=item["target"]
        if target not in targets and not re.fullmatch(r"builtin:(?:core:[A-Za-z_][A-Za-z_0-9]*|type:[A-Za-z_][A-Za-z_0-9]*|std\.[A-Za-z_]+#[A-Za-z_][A-Za-z_0-9]*)",target):raise ValueError("invalid reference target")

def validate_types(report,contract):
    graph=report["graph"];validate_modules(graph,contract)
    if graph.get("budget_profile")!="compiler" or graph["source"]!=report["source"]:raise ValueError("invalid typed graph budget/source")
    targets={x["target"] for x in graph["symbols"]};modules={x["id"]:x for x in graph["modules"]}
    symbols=report["symbols"];seen=set()
    for s in symbols:
        if s["target"] not in targets or s["target"] in seen or not isinstance(s["type_name"],str) or not s["type_name"] or type(s["writable"]) is not bool or type(s["mutable_binding"]) is not bool:raise ValueError("invalid typed symbol")
        seen.add(s["target"])
    if seen!=targets:raise ValueError("missing typed symbol")
    seen=set()
    for fact in report["facts"]:
        u=modules[fact["module"]];identity=(fact["module"],fact["node"])
        if type(fact["node"]) is not int or not 0<=fact["node"]<u["pool_nodes"] or identity in seen:raise ValueError("invalid typed node identity")
        if any(type(fact[k]) is not int for k in ("start","end")) or not 0<=fact["start"]<=fact["end"]<=u["input_bytes"]:raise ValueError("invalid typed span")
        if not isinstance(fact["type_name"],str) or not fact["type_name"] or type(fact["writable"]) is not bool or not isinstance(fact["kind"],str):raise ValueError("invalid typed fact")
        seen.add(identity)
    expected={x["target"] for x in graph["symbols"] if x["kind"]=="Function"};seen=set()
    for f in report["functions"]:
        if f["target"] not in expected or f["target"] in seen or type(f["writable"]) is not bool or not isinstance(f["result"],str) or not f["result"]:raise ValueError("invalid typed function")
        declaration=next(x for x in graph["symbols"] if x["target"]==f["target"])
        if f["module"]!=declaration["module"] or f["node"]!=declaration["node"]:raise ValueError("invalid function identity")
        for p in f["parameters"]:
            if p["target"] not in targets or type(p["writable"]) is not bool or not isinstance(p["type_name"],str) or not p["type_name"]:raise ValueError("invalid parameter")
        seen.add(f["target"])
    if seen!=expected:raise ValueError("missing typed function")
    stats=report["stats"]
    if type(stats["work"]) is not int or not 0<=stats["work"]<=contract["types"]["work"] or type(stats["capability_passes"]) is not int or not 1<=stats["capability_passes"]<=len(expected)+2:raise ValueError("invalid type analysis counters")

def validate_lowering(report,contract):
    analysis=report["analysis"]
    if analysis.get("schema")!="hua.types.capabilities.v1" or analysis.get("profile")!="C04-T1" or analysis.get("status")!="ok" or analysis.get("source")!=report["source"]:raise ValueError("invalid C05 analysis")
    validate_types(analysis,contract)
    modules={u["id"]:u for u in analysis["graph"]["modules"]}
    if report["entry"]!=report["source"]:raise ValueError("invalid C05 entry")
    sources=report["sources"]
    if not isinstance(sources,list) or [x["module"] for x in sources]!=list(modules):raise ValueError("invalid C05 source table")
    positions={}
    for item in sources:
        text=item["text"]
        if not isinstance(text,str) or chr(13) in text or text.startswith(chr(65279)):raise ValueError("invalid C05 normalized source")
        raw=text.encode("utf-8")
        if len(raw)>modules[item["module"]]["input_bytes"]:raise ValueError("invalid C05 source bytes")
        positions[item["module"]]=(raw,[0]+[i+1 for i,b in enumerate(raw) if b==10])
    nodes=report["nodes"];limits=contract["lowering"];stats=report["stats"]
    if not isinstance(nodes,list) or type(stats["nodes"]) is not int or stats["nodes"]!=len(nodes) or len(nodes)>limits["nodes"] or type(stats["work"]) is not int or not len(nodes)<=stats["work"]<=limits["work"]:raise ValueError("invalid C05 budget")
    exact={k:0 for k in ("Integer","Float","String","Boolean","Nil","Name","TypeName","Break","Continue","Omitted")}
    exact.update({k:1 for k in ("Unary","Update","Member","FieldInit","ExpressionStatement","Propagate","Defer","SliceType","ArrayType","OptionalType","MutableType")})
    exact.update({k:2 for k in ("Binary","Assignment","Index","MapEntry","While","MultiBinding","MultiAssignment")})
    exact.update({k:3 for k in ("Slice","MatchArm")})
    choice={"If":{2,3},"Range":{2,3},"Parameter":{0,1},"Let":{1,2},"Var":{1,2},"Field":{1,2},"For":{3,4}}
    minimum={k:1 for k in ("Call","ReturnTypes","GenericType","MapLiteral","Match","Function")};minimum["Pack"]=2
    free={"Struct","Block","Array","StructLiteral","BindingList","Return"}
    parents=[0]*len(nodes);depths=[];declarations=set()
    symbols={(x["module"],x["node"]):x for x in analysis["graph"]["symbols"]}
    typed={x["target"]:x for x in analysis["symbols"]}
    references={(x["module"],x["node"]):x["target"] for x in analysis["graph"]["references"]}
    facts={(x["module"],x["node"]):x for x in analysis["facts"]}
    functions={x["target"]:x for x in analysis["functions"]}
    prefixes={x:"$m"+x.encode("utf-8").hex()+"$" for x in modules}
    def runtime_name(target):
        if target==report["entry"]+"#main":return "main"
        if target.startswith("builtin:core:"):return "$core$"+target[13:]
        if target=="builtin:std.json#Value":return "Json"
        if target.startswith("builtin:type:"):return target[13:]
        if target.startswith("builtin:std."):return "$core$$std$"+target[12:].replace("#","$")
        module,sep,name=target.rpartition("#")
        return prefixes[module]+name.replace(":","$c$") if sep else target
    def runtime_type(t):
        t=t.replace("builtin:core:","builtin:").replace("builtin:std.","builtin:$std$").replace("variant:","fn:").replace("enum:","struct:")
        for module in sorted(modules,key=len,reverse=True):t=t.replace(module+"#",prefixes[module])
        t=t.replace("#","$")
        marker="fn:"+prefixes[report["entry"]]+"main"
        for end in (",",">","?"):t=t.replace(marker+end,"fn:main"+end)
        return t[:-len(marker)]+"fn:main" if t.endswith(marker) else t
    unary={"+","-","!","~"}
    binary={"+","-","*","/","//","%","**","==","!=","<","<=",">",">=","&&","||","&","|","^","<<",">>"}
    assignments={"=","+=","-=","*=","/=","//=","%=","**="}
    for i,n in enumerate(nodes):
        if type(n.get("id")) is not int or n["id"]!=i or n["module"] not in modules or type(n["origin"]) is not int or not 0<=n["origin"]<modules[n["module"]]["pool_nodes"]:raise ValueError("invalid C05 node identity")
        if any(type(n[k]) is not int for k in ("start","end","line","column")) or not 0<=n["start"]<=n["end"]<=modules[n["module"]]["input_bytes"] or min(n["line"],n["column"])<1:raise ValueError("invalid C05 source")
        raw,lines=positions[n["module"]]
        if n["end"]>len(raw) or n["line"]>len(lines) or lines[n["line"]-1]+n["column"]-1!=n["start"] or (n["line"]<len(lines) and n["start"]>=lines[n["line"]]):raise ValueError("invalid C05 normalized position")
        if not isinstance(n["text"],str) or not isinstance(n["type_name"],str) or type(n["writable"]) is not bool or n["role"] not in ("source","enum-layout","enum-constructor"):raise ValueError("invalid C05 metadata")
        c=n["children"];k=n["kind"]
        if not isinstance(c,list) or any(type(x) is not int or not 0<=x<i for x in c):raise ValueError("invalid C05 children")
        if not (k in free or k in exact and len(c)==exact[k] or k in choice and len(c) in choice[k] or k in minimum and len(c)>=minimum[k]):raise ValueError("invalid C05 shape")
        if k=="Unary" and n["text"] not in unary or k=="Binary" and n["text"] not in binary or k=="Assignment" and n["text"] not in assignments or k=="Range" and n["text"] not in ("..","..=") or k=="Update" and n["text"] not in ("++","--"):raise ValueError("invalid C05 operator")
        if n["role"]=="source":
            identity=(n["module"],n["origin"]);symbol=symbols.get(identity);fact=facts.get(identity);ref=references.get(identity)
            if symbol and k in ("Function","Struct","Let","Var","Parameter","Name"):
                expected=runtime_name(symbol["target"])
                if k=="Parameter" and n["text"].endswith(" mut"):expected+=" mut"
                if n["text"]!=expected:raise ValueError("invalid C05 symbol mapping")
            elif ref and k in ("Name","TypeName","StructLiteral"):
                if n["text"]!=runtime_name(ref):raise ValueError("invalid C05 reference mapping")
            expected_type=None
            if symbol and symbol["target"] in typed:
                expected_type=typed[symbol["target"]]["type_name"]
            elif fact:expected_type=fact["type_name"]
            if expected_type is not None and n["type_name"]!=runtime_type(expected_type):raise ValueError("invalid C05 type projection")
            if k=="Function":
                f=functions[symbol["target"]]
                if n["writable"]!=f["writable"]:raise ValueError("invalid C05 return capability")
        d=1+max((depths[x] for x in c),default=0)
        if d>limits["depth"]:raise ValueError("invalid C05 depth")
        depths.append(d)
        for x in c:
            if nodes[x]["module"]!=n["module"]:raise ValueError("C05 cross-module tree edge")
            parents[x]+=1
        if k=="Function":
            if nodes[c[-1]]["kind"]!="Block":raise ValueError("invalid C05 function")
            seen_return=False
            for j in range(len(c)-1):
                p=nodes[c[j]]
                if p["kind"]=="ReturnTypes":
                    if seen_return or j!=len(c)-2:raise ValueError("invalid C05 return signature")
                    seen_return=True
                elif p["kind"]!="Parameter" or seen_return:raise ValueError("invalid C05 parameter")
        if k=="Defer" and nodes[c[0]]["kind"]!="Call":raise ValueError("invalid C05 defer")
        if k=="MatchArm" and [nodes[x]["kind"] for x in c[1:]]!=["BindingList","Block"]:raise ValueError("invalid C05 match arm")
        if k in ("Let","Var","Call","Match") and not n["type_name"]:raise ValueError("missing C05 execution type")
        if k in ("Function","Struct","Let","Var","Parameter") and n["text"]!="_":
            name=n["text"].split(" ")[0]
            if name in declarations:raise ValueError("duplicate C05 declaration")
            declarations.add(name)
    inits=report["initialization"]
    if not isinstance(inits,list) or [x["module"] for x in inits]!=analysis["graph"]["initialization"]:raise ValueError("invalid C05 initialization")
    for init in inits:
        if not isinstance(init["roots"],list):raise ValueError("invalid C05 roots")
        for root in init["roots"]:
            if type(root) is not int or not 0<=root<len(nodes) or nodes[root]["module"]!=init["module"]:raise ValueError("invalid C05 root")
            parents[root]+=1
    if any(x!=1 for x in parents):raise ValueError("C05 shared/disconnected core")
    # Inspect each node once. Nested declarations and closure captures are outside SB1.
    stack=[(x,False,0,0) for init in inits for x in reversed(init["roots"])]
    while stack:
        i,function,loops,depth=stack.pop();n=nodes[i];k=n["kind"];c=n["children"]
        if depth>limits["depth"]:raise ValueError("invalid C05 nesting")
        if k in ("Function","Struct") and depth!=0:raise ValueError("nested C05 declaration")
        if k in ("Break","Continue") and not loops:raise ValueError("C05 loop exit outside loop")
        if k=="Return" and not function:raise ValueError("C05 return outside function")
        if k in ("Defer","Propagate") and not function:raise ValueError("C05 cleanup/propagation outside function")
        for j,x in enumerate(c):
            nested_function=function or k=="Function"
            nested_loops=loops+int(k in ("While","For") and j==len(c)-1)
            stack.append((x,nested_function,nested_loops,depth+1))


def validate_bytecode(report,contract):
    """Validate C06 transport, identities, metadata and reported bounds; Hua verifies control flow."""
    import math
    limits=contract["bytecode"];sites=report["sites"];sources=report["sources"]
    if report.get("schema")!=limits["schema"] or report.get("profile")!=limits["profile"]:raise ValueError("invalid C06 envelope")
    if not isinstance(sources,list) or not 1<=len(sources)<=contract["compiler_budget"]["analysis"]["modules"]:raise ValueError("invalid C06 sources")
    texts={}
    for source in sources:
        name=source["module"];text=source["text"]
        if not isinstance(name,str) or not name or name in texts or not isinstance(text,str) or "\r" in text or text.startswith("\ufeff"):raise ValueError("invalid C06 normalized source")
        texts[name]=text.encode("utf-8")
    if sum(map(len,texts.values()))>contract["compiler_budget"]["analysis"]["source_bytes_total"] or report["entry"] not in texts:raise ValueError("invalid C06 source budget/entry")
    if not isinstance(sites,list) or not 1<=len(sites)<=limits["sites"]:raise ValueError("invalid C06 sites")
    seen_nodes=set()
    for site in sites:
        if site["module"] not in texts or any(type(site[k]) is not int for k in ("node","origin","start","end","line","column")):raise ValueError("invalid C06 source identity")
        data=texts[site["module"]];start,end=site["start"],site["end"]
        if not 0<=start<=end<=len(data) or site["node"] in seen_nodes:raise ValueError("invalid C06 source bounds")
        seen_nodes.add(site["node"])
        prefix=data[:start]
        if site["line"]!=1+prefix.count(b"\n") or site["column"]!=start-prefix.rfind(b"\n"):raise ValueError("invalid C06 position")
        if site["node"]==-1:
            if site["origin"]!=-1 or site["role"]!="initializer" or start!=0 or end!=0 or site["module"]!=report["entry"]:raise ValueError("invalid C06 initializer site")
        elif not 0<=site["node"]<contract["lowering"]["nodes"] or not 0<=site["origin"]<contract["compiler_budget"]["parser"]["nodes"] or site["role"] not in ("source","enum-layout","enum-constructor"):raise ValueError("invalid C06 origin")
    def small(n,empty=False):
        return isinstance(n,str) and (empty or bool(n)) and len(n.encode("utf-8"))<=4096 and "\0" not in n
    def site(n):
        return type(n) is int and 0<=n<len(sites)
    structures=report["structures"];functions=report["functions"]
    if not isinstance(structures,list) or not isinstance(functions,list) or len(structures)+len(functions)>8192:raise ValueError("invalid C06 declarations")
    sn={x["name"] for x in structures};fn={x["name"] for x in functions}
    if len(sn)!=len(structures) or len(fn)!=len(functions) or sn&fn:raise ValueError("duplicate C06 declaration")
    core=set(limits["intrinsics"])
    def intrinsic(n):
        if n.startswith("$core$"):n=n[6:]
        if n.startswith("$std$"):
            p=n[5:].split("$")
            return len(p)==2 and "builtin:std."+p[0]+"#"+p[1] in contract["bytecode"]["standard_intrinsics"]
        return n in core
    primitive={"","void","nil","bool","string","int","float","byte","i8","i16","i32","i64","u8","u16","u32","u64","isize","usize","f32","f64","Bytes","Buffer","Json"}
    def parts(t,name):
        if not (t.startswith(name+"<") and t.endswith(">")):return []
        raw=t[len(name)+1:-1];depth=0;begin=0;result=[]
        for i,c in enumerate(raw):
            depth+=(c=="<")-(c==">")
            if c=="," and depth==0:result.append(raw[begin:i]);begin=i+1
            if depth<0:return []
        if depth:return []
        return result+[raw[begin:]]
    def valid_type(t,depth=0):
        if depth>64 or not small(t,True):return False
        if t in primitive or t in sn:return True
        if t.startswith("fn:"):return t[3:] in fn
        if t.startswith("builtin:"):return intrinsic(t[8:])
        if t.startswith("struct:"):return t[7:] in sn
        if t.endswith("?"):return valid_type(t[:-1],depth+1)
        if t.startswith("["):
            a=re.fullmatch(r"\[([0-9_]*)\](.*)",t)
            return bool(a) and (not a[1] or bool(a[1].replace("_",""))) and valid_type(a[2],depth+1)
        for name,counts in [("Result",{2}),("map",{2}),("List",{1}),("multi",None)]:
            ps=parts(t,name)
            if ps:return (len(ps) in counts if counts else len(ps)>=2) and all(valid_type(p,depth+1) for p in ps)
        return False
    for st in structures:
        if not small(st["name"]) or not site(st["site"]) or type(st["published"]) is not bool or type(st["enumeration"]) is not bool or not isinstance(st["fields"],list) or len(st["fields"])>4096:raise ValueError("invalid C06 structure")
        names=set()
        for f in st["fields"]:
            if not small(f["name"]) or f["name"] in names or not site(f["site"]) or not valid_type(f["type_name"]):raise ValueError("invalid C06 field")
            names.add(f["name"])
    for f in functions:
        if not small(f["name"]) or not site(f["site"]) or type(f["published"]) is not bool or type(f["writable"]) is not bool or not isinstance(f["parameters"],list) or not isinstance(f["returns"],list) or max(len(f["parameters"]),len(f["returns"]))>4096 or f["name"]!=f["code"]["name"]:raise ValueError("invalid C06 function")
        names=set()
        for p in f["parameters"]:
            if not small(p["name"]) or p["name"] in names or not valid_type(p["type_name"]) or not site(p["site"]) or type(p["writable"]) is not bool:raise ValueError("invalid C06 parameter")
            names.add(p["name"])
        if not all(valid_type(t) for t in f["returns"]) or f["name"]=="main" and f["parameters"]:raise ValueError("invalid C06 return/entry")
    initializer=report["initializer"]
    if initializer["name"]!="<initialize>":raise ValueError("invalid C06 initializer")
    globals_=fn|{op["text"] for op in initializer["instructions"] if op["op"]==2}|{n for op in initializer["instructions"] if op["op"]==35 for n in op["names"]}
    total=0
    for code,owner in [(initializer,None)]+[(f["code"],f) for f in functions]:
        ops=code["instructions"]
        if not isinstance(ops,list) or not 1<=len(ops)<=limits["chunk_instructions"]:raise ValueError("invalid C06 chunk")
        total+=len(ops)
        names=globals_|({p["name"] for p in owner["parameters"]} if owner else set())|{op["text"] for op in ops if op["op"]==2}|{n for op in ops if op["op"] in (25,35) for n in op["names"]}
        for op in ops:
            tag=op["op"];const=op["constant"]
            if type(tag) is not int or not 0<=tag<len(limits["opcodes"]) or tag in limits["excluded_opcodes"] or not site(op["site"]) or not small(op["text"],True) or not (tag==27 and op["type_name"]=="E4002" or valid_type(op["type_name"])) or type(op["flag"]) is not bool:raise ValueError("invalid C06 instruction")
            if type(op["argument"]) is not int or not 0<=op["argument"]<=4096 or type(op["target"]) is not int or op["target"]<0:raise ValueError("invalid C06 operand")
            if not isinstance(op["names"],list) or not isinstance(op["contextual"],list) or max(len(op["names"]),len(op["contextual"]))>4096 or not all(small(n) for n in op["names"]) or not all(type(b) is bool for b in op["contextual"]):raise ValueError("invalid C06 vector metadata")
            kind,text=const["kind"],const["text"]
            if type(kind) is not int or kind not in range(5) or not isinstance(text,str):raise ValueError("invalid C06 scalar")
            if kind==0 and text!="" or kind==1 and text not in ("true","false"):raise ValueError("invalid C06 nil/bool")
            if kind==2 and (not re.fullmatch(r"-?(0|[1-9]\d*)",text) or not -(1<<63)<=int(text)<1<<63 or str(int(text))!=text):raise ValueError("invalid C06 int")
            if kind==3 and (not re.fullmatch(r"-?(0|[1-9]\d*)(\.\d+)?([eE][+-]?\d+)?",text) or not math.isfinite(float(text))):raise ValueError("invalid C06 float")
            if kind==4 and len(text.encode("utf-8"))>16777216:raise ValueError("invalid C06 string")
            if tag!=0 and kind!=0:raise ValueError("C06 unexpected constant")
            if tag in (6,7,8,25) and op["target"]>=len(ops):raise ValueError("invalid C06 jump")
            if tag==25 and not 1<=len(op["names"])<=2 or tag==18 and len(op["contextual"])!=2 or tag in (21,40) and len(op["contextual"])!=op["argument"] or tag==2 and (len(op["contextual"])!=1 or not op["text"] or not op["type_name"]):raise ValueError("invalid C06 call/bind/loop metadata")
            if tag==34 and (op["argument"]<2 or len(op["contextual"])!=op["argument"]) or tag==36 and op["argument"]<2:raise ValueError("invalid C06 pack")
            if tag==35 and (op["argument"]<2 or len(op["names"])!=op["argument"] or len(parts(op["type_name"],"multi"))!=op["argument"] or len([n for n in op["names"] if n!="_"])!=len({n for n in op["names"] if n!="_"})):raise ValueError("invalid C06 multi binding")
            if tag==32:
                ps=parts(op["type_name"],"map")
                if len(ps)!=2 or ps[0] not in ("bool","int","string") or ps[1] in ("","void") or ps[1].startswith("Result<"):raise ValueError("invalid C06 map")
            if tag==37 and (not owner or len(owner["returns"])!=1 or len(parts(owner["returns"][0],"Result"))!=2):raise ValueError("invalid C06 propagation")
            if tag==19 and op["text"] not in sn or tag in (1,12) and op["text"]!="_" and op["text"] not in names and not (tag==1 and intrinsic(op["text"])):raise ValueError("invalid C06 reference")
            if tag==27 and (op["type_name"]!="E4002" or not op["text"]):raise ValueError("invalid C06 failure")
            if tag==4 and op["text"] not in ("+","-","!","~") or tag==5 and op["text"] not in ("+","-","*","/","//","%","**","&","|","^","<<",">>","==","!=","<",">","<=",">=","&&","||") or tag==15 and op["text"] not in ("=","+=","-=","*=","/=","//=","%=","**="):raise ValueError("invalid C06 operator")
    if type(report["instructions"]) is not int or report["instructions"]!=total or total>limits["instructions"] or type(report["work"]) is not int or not total<=report["work"]<=limits["work"]:raise ValueError("invalid C06 totals")
    v=report["verification"]
    if v["instructions"]!=total or type(v["instructions"]) is not int:raise ValueError("invalid C06 verification count")
    for key,maximum in [("reachable",total),("slots",limits["state_slots"]),("work",limits["work"]),("max_stack",limits["stack"]),("max_scopes",limits["scopes"]),("max_iterators",limits["iterators"])]:
        if type(v[key]) is not int or not 0<=v[key]<=maximum:raise ValueError("invalid C06 verification budget")
    if v["reachable"]<1 or v["slots"]<v["reachable"] or v["work"]<v["reachable"]:raise ValueError("invalid C06 verification counters")

def main(argv=None):
    args = CommandParser(description="Hua Parser / 分析 / 核心降级 / 指令生成；输出 JSON，HUAB 编码待 C07")
    args.add_argument("command",choices=["parse","analyze","check","lower","emit"])
    args.add_argument("source",type=Path)
    args.add_argument("-o","--output",type=Path)
    args.add_argument("--profile",choices=["C02-P1","C02-P2"],default="C02-P1")
    args.add_argument("--hua",type=Path,default=ROOT/"build"/("hua.exe" if os.name=="nt" else "hua"))
    args.add_argument("--budget",choices=["slice","compiler"],default=None)
    options=args.parse_args(argv)
    compiler=options.budget=="compiler" or options.command in ("check","lower","emit")
    if options.command in ("check","lower","emit") and options.budget=="slice":return emit_error("E8204","command",options.command+" requires compiler budget",2,profile="C06-BC1" if options.command=="emit" else "C05-L1" if options.command=="lower" else "C04-T1")
    profile="C06-BC1" if options.command=="emit" else "C05-L1" if options.command=="lower" else "C04-T1" if options.command=="check" else "C03-N1" if options.command=="analyze" else options.profile
    schema="hua.bytecode.image.v1" if profile=="C06-BC1" else "hua.core.lowered.v1" if profile=="C05-L1" else "hua.types.capabilities.v1" if profile=="C04-T1" else "hua.names.modules.v1" if profile=="C03-N1" else "hua.ast.slice.v1"
    def fail(code,stage,message,exit_code,source=""):
        return emit_error(code,stage,message,exit_code,source,profile)
    try:
        contract=json.loads((ROOT/"selfhost/contract.json").read_text(encoding="utf-8"))
        if compiler:
            contract["budgets"]=contract["compiler_budget"]["parser"]
            contract["analysis"]["budgets"]=contract["compiler_budget"]["analysis"]
        if compiler and profile=="C02-P1":return fail("E8204","command","compiler budget requires C02-P2",2)
        source=options.source.resolve()
        if source.suffix!=".hua":return fail("E8204","command","expected .hua source",2,str(source))
        if options.output and options.output.suffix!=".json":return fail("E8204","command","frontend output must be .json; HUAB generation is pending C07",2,str(source))
        # Bound the read before launching the runtime. Encoding is checked by std.fs in Hua.
        with source.open("rb") as stream:raw=stream.read(contract["budgets"]["source_bytes"]+1)
        if len(raw)>contract["budgets"]["source_bytes"]:return fail("E8202","parser","E8202@1:1 source byte budget",1,source.name)
        entry={"C02-P1":"parser_main.hua","C02-P2":"parser_extended_main.hua","C03-N1":"modules_main.hua","C04-T1":"types_main.hua","C05-L1":"lowering_main.hua","C06-BC1":"bytecode_main.hua"}[profile]
        if compiler and profile in ("C02-P2","C03-N1"):entry="parser_compiler_main.hua" if profile=="C02-P2" else "modules_compiler_main.hua"
        arguments=[str(source)] if profile in ("C03-N1","C04-T1","C05-L1","C06-BC1") else [str(source),source.name]
        policy=["--execution-profile","compiler"] if compiler else []
        timeout=contract["compiler_budget"]["supervisor_seconds"] if compiler else 40
        report_limit=contract["compiler_budget"]["report_bytes"] if compiler else 1048576
        process=subprocess.run([str(options.hua.resolve()),"run",str(ROOT/"selfhost"/entry),*policy,"--",*arguments],capture_output=True,encoding="utf-8",errors="strict",timeout=timeout)
        if process.returncode:
            code="E4099" if "[E4099]" in process.stderr else "E8203"
            return fail(code,"host",process.stderr.strip() or "host execution failed",1 if code=="E4099" else 3,source.name)
        if len(process.stdout.encode("utf-8"))>report_limit:return fail("E8203","host","frontend report exceeds configured byte budget",3,source.name)
        report=json.loads(process.stdout)
        if not isinstance(report,dict) or report.get("schema")!=schema or report.get("profile")!=profile or report.get("source")!=source.name:raise ValueError("invalid Hua frontend envelope")
        if report.get("status")=="error":
            if report.get("stage") not in ({"bytecode"} if profile=="C06-BC1" else {"lowering"} if profile=="C05-L1" else {"types"} if profile=="C04-T1" else {"analysis"} if profile=="C03-N1" else {"parser","lexer","input","command"}):raise ValueError("invalid diagnostic stage")
            if any(k in report for k in ("nodes","modules","symbols","references","initialization","graph","facts","functions","analysis","entry","sites","structures","initializer","verification","instructions","sources","stats")):raise ValueError("failed frontend included partial artifact")
            detail=report.get("error","");code=re.match(r"(E\d+)@\d+:\d+",detail)
            if not code:raise ValueError("invalid frontend diagnostic")
            code=code[1];sys.stderr.write(process.stdout)
            return 2 if report.get("stage")=="input" or code=="E8204" else 3 if code=="E8203" else 1
        if report.get("status")!="ok":raise ValueError("invalid successful frontend report")
        if compiler and profile not in ("C04-T1","C05-L1","C06-BC1") and report.get("budget_profile")!="compiler":raise ValueError("missing compiler budget profile")
        if profile=="C06-BC1":validate_bytecode(report,contract)
        elif profile=="C05-L1":validate_lowering(report,contract)
        elif profile=="C04-T1":validate_types(report,contract)
        elif profile=="C03-N1":validate_modules(report,contract)
        else:
            if not isinstance(report.get("nodes"),list) or not report["nodes"]:raise ValueError("missing AST")
            stats=report["stats"]
            for key,budget in [("input_bytes","source_bytes"),("tokens","tokens"),("pool_nodes","nodes"),("work","work")]:
                if type(stats.get(key)) is not int or not 0<=stats[key]<=contract["budgets"][budget]:raise ValueError("invalid reported budget counters")
            validate_nodes(report)
        if options.output:
            target=options.output.resolve();temporary=None
            try:
                with tempfile.NamedTemporaryFile(mode="wb",prefix=target.name+".tmp-",dir=target.parent,delete=False) as stream:
                    temporary=Path(stream.name);stream.write(process.stdout.encode("utf-8"));stream.flush();os.fsync(stream.fileno())
                os.replace(temporary,target);temporary=None
            finally:
                if temporary is not None:temporary.unlink(missing_ok=True)
        else:sys.stdout.write(process.stdout)
        return 0
    except subprocess.TimeoutExpired:
        return fail("E8202","host","frontend supervisor timeout; no output published",1)
    except (OSError,UnicodeError) as error:
        return fail("E8204","input",str(error),2)
    except (ValueError,KeyError,TypeError,AttributeError) as error:
        return fail("E8203","internal",str(error),3)

if __name__=="__main__":raise SystemExit(main())
