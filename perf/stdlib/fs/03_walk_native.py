# 递归遍历：10×10目录、每目录10个文件，遍历30轮
import os, tempfile, shutil
root = os.path.join(tempfile.gettempdir(), "hua_bench_walk_py")
shutil.rmtree(root, ignore_errors=True)
for a in range(10):
    for b in range(10):
        d = os.path.join(root, f"d{a}", f"e{b}")
        os.makedirs(d)
        for c in range(10):
            with open(os.path.join(d, f"f{c}.txt"), "w") as f:
                f.write("x")
total = 0
dirs = 0
for r in range(30):
    for base, subdirs, files in os.walk(root):
        total += len(subdirs) + len(files)
        dirs += len(subdirs)
shutil.rmtree(root)
print(total, dirs)
