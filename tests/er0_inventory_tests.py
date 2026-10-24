"""Every current baseline API must have an independently maintained ER0 row."""
from pathlib import Path
import re,sys
root=Path(sys.argv[1])
registry=(root/"src/stdlib.cpp").read_text(encoding="utf-8")
functions=re.findall(r'^\s*\{"([^"]+)","([^"]+)",\{',registry,re.M)
# since-9/10 additions (HUA-D038 slices 1/2) have their own ER0-T/ER0-U rows; the original baseline stays 125.
text_later={(m,n) for m,n in re.findall(r'^\s*\{"([^"]+)","([^"]+)",\{.*,9\},\s*$',registry,re.M)}
collection_later={(m,n) for m,n in re.findall(r'^\s*\{"([^"]+)","([^"]+)",\{.*,10\},\s*$',registry,re.M)}
callback_later={(m,n) for m,n in re.findall(r'^\s*\{"([^"]+)","([^"]+)",\{.*,11,true\},\s*$',registry,re.M)}
later=text_later|collection_later|callback_later
text=(root/"docs/标准库与内建接口.md").read_text(encoding="utf-8")
listed=re.findall(r'^\| ER0-S\d{3} \| std\.([^ ]+) \|',text,re.M)
expected=[m+"."+n for m,n in functions if m not in {"error","file"} and (m,n) not in later]
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
new_file=[m+"."+n for m,n in functions if m=="file"]
file_rows=re.findall(r"^\| ER0-R\d\d \| std\.([^ ]+) \|",text,re.M)
assert len(file_rows)==len(set(file_rows))==len(new_file)==12 and set(file_rows)==set(new_file)
text_rows=re.findall(r"^\| ER0-T\d\d \| std\.([^ ]+) \|",text,re.M)
assert len(text_rows)==len(set(text_rows))==len(text_later)==23 and set(text_rows)=={m+"."+n for m,n in text_later},(len(text_rows),len(text_later))
collection_rows=re.findall(r"^\| ER0-U\d\d \| std\.([^ ]+) \|",text,re.M)
assert len(collection_rows)==len(set(collection_rows))==len(collection_later)==21 and set(collection_rows)=={m+"."+n for m,n in collection_later},(len(collection_rows),len(collection_later))
callback_rows=re.findall(r"^\| ER0-V\d\d \| std\.([^ ]+) \|",text,re.M)
assert len(callback_rows)==len(set(callback_rows))==len(callback_later)==15 and set(callback_rows)=={m+"."+n for m,n in callback_later},(len(callback_rows),len(callback_later))
print("ER0: 125 standard, 38 builtin, 13 Python, 6 internal excluded; 16 new Error APIs, 12 File, 23 slice-1, 21 slice-2 and 15 L0-1 callback APIs separately.")
