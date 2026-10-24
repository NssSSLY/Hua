# 复制200个4 KiB文件，5轮
import os, tempfile, shutil
root = os.path.join(tempfile.gettempdir(), "hua_bench_copy_py")
shutil.rmtree(root, ignore_errors=True)
src = os.path.join(root, "src")
os.makedirs(src)
data = bytes((i * 7) % 256 for i in range(4096))
for i in range(200):
    with open(os.path.join(src, f"f{i}.bin"), "wb") as f:
        f.write(data)
copied = 0
for r in range(5):
    dst = os.path.join(root, f"dst{r}")
    os.makedirs(dst)
    for name in sorted(os.listdir(src)):
        shutil.copyfile(os.path.join(src, name), os.path.join(dst, name))
        copied += 1
shutil.rmtree(root)
print(copied)
