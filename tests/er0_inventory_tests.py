"""Every current baseline API must have an independently maintained ER0 row."""
from pathlib import Path
import re,sys
root=Path(sys.argv[1])
registry=(root/"src/stdlib.cpp").read_text(encoding="utf-8")
functions=re.findall(r'^\s*\{"([^"]+)","([^"]+)",\{',registry,re.M)
text=(root/"docs/标准库与内建接口.md").read_text(encoding="utf-8")
listed=re.findall(r'^\| ER0-S\d{3} \| std\.([^ ]+) \|',text,re.M)
expected=[m+"."+n for m,n in functions if m!="error"]
assert len(listed)==len(set(listed))==len(expected)==125,(len(listed),len(expected))
assert set(listed)==set(expected),set(expected)-set(listed)
runtime=(root/"src/runtime.cpp").read_text(encoding="utf-8")
names=re.findall(r'"([^"]+)"',re.search(r'out=\{([^}]+)\}',runtime).group(1))
internal={"task_start","task_await","task_group_begin","task_group_end","parallel_map","simd_check"}
builtins=set(names)-internal
recorded=re.findall(r'^\| ER0-B\d\d \| ([^ ]+) \|',text,re.M)
assert len(recorded)==len(set(recorded))==38 and set(recorded)==builtins
wrappers=re.findall(r'^pub fn (\w+)\(', (root/"bridges/python/python.hua").read_text(encoding="utf-8"),re.M)
recorded=re.findall(r'^\| ER0-P\d\d \| (\w+)\(',text,re.M)
assert len(recorded)==len(set(recorded))==13 and set(recorded)==set(wrappers)
assert len([x for x in functions if x[0]=="error"])==16
print("ER0: 125 standard, 38 builtin, 13 Python, 6 internal excluded; 16 new Error APIs separately.")
