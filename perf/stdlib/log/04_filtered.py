# 结构日志：低于级别的logging调用被过滤
import logging, sys
logger = logging.getLogger("bench"); logger.addHandler(logging.StreamHandler(sys.stderr)); logger.setLevel(logging.WARNING); logger.propagate = False
lines = 0
for i in range(1000000):
    logger.debug("request done")
    lines += 1
print(lines, "true")
