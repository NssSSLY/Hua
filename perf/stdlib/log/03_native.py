# 结构日志：logging模块，logfmt风格Formatter写stderr
import logging, sys
handler = logging.StreamHandler(sys.stderr)
formatter = logging.Formatter('ts=%(asctime)s.%(msecs)03dZ level=%(levelname)s msg="%(message)s" id=%(id)s path=%(path)s', "%Y-%m-%dT%H:%M:%S"); formatter.converter = __import__("time").gmtime
handler.setFormatter(formatter)
logger = logging.getLogger("bench"); logger.addHandler(handler); logger.setLevel(logging.INFO); logger.propagate = False
lines = 0
for i in range(100000):
    logger.info("request done", extra={"id": str(i), "path": "/api/items"})
    lines += 1
print(lines, "true")
