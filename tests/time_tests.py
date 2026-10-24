# HUA-D038 step 5 group 2 (std.time Duration and UTC dates) across three execution paths.
# Dates are checked against Python datetime (UTC, proleptic Gregorian); durations
# against Python ports of Go's Duration.String and ParseDuration.
from pathlib import Path
from datetime import datetime,timedelta,timezone
import difflib,random,struct,subprocess,sys,tempfile
exe,root,build=(Path(x).resolve() for x in sys.argv[1:]);checks=0
EPOCH=datetime(1970,1,1,tzinfo=timezone.utc)
MIN_MS=int((datetime(1,1,1,tzinfo=timezone.utc)-EPOCH)/timedelta(milliseconds=1));MAX_MS=int((datetime(9999,12,31,23,59,59,999000,tzinfo=timezone.utc)-EPOCH)/timedelta(milliseconds=1))
I64=(1<<63)

def run(command,path,*extra,cwd=None,expected=None,code=0,error=None):
    global checks
    p=subprocess.run([str(exe),command,str(path),*map(str,extra)],cwd=cwd,capture_output=True,timeout=120)
    out=p.stdout.decode('utf-8').replace('\r\n','\n');err=p.stderr.decode('utf-8')
    assert p.returncode==code,(command,path,p.returncode,out,err)
    if expected is not None:assert out==expected,(command,''.join(difflib.unified_diff(expected.splitlines(True),out.splitlines(True),'expected','actual')),err)
    if error:assert error in err,(command,out,err,error)
    checks+=1;return out
def header_format(artifact):return struct.unpack_from('<I',artifact.read_bytes(),8)[0]
def lit(v):return f'({v+1} - 1)' if v==-I64 else str(v)

def dt(ms):return EPOCH+timedelta(milliseconds=ms)
def rfc(ms,zone='Z',offset=0):
    d=dt(ms+offset*60000);frac=(ms+offset*60000)%1000
    return f'{d.year:04d}-{d.month:02d}-{d.day:02d}T{d.hour:02d}:{d.minute:02d}:{d.second:02d}'+(f'.{frac:03d}' if frac else '')+zone
def go_format(d):
    neg=d<0;u=-d if neg else d
    def frac(v,prec):
        out='';printed=False
        for _ in range(prec):
            digit=v%10;printed=printed or digit!=0
            if printed:out=str(digit)+out
            v//=10
        return ('.'+out if printed else ''),v
    if u<10**9:
        if u==0:return '0s'
        prec,unit=(0,'ns') if u<1000 else (3,'µs') if u<10**6 else (6,'ms')
        f,u=frac(u,prec);s=f'{u}{f}{unit}'
    else:
        f,u=frac(u,9);s=f'{u%60}{f}s';u//=60
        if u>0:
            s=f'{u%60}m'+s;u//=60
            if u>0:s=f'{u}h'+s
    return ('-' if neg else '')+s
UNITS={'ns':1,'us':1000,'µs':1000,'μs':1000,'ms':10**6,'s':10**9,'m':60*10**9,'h':3600*10**9}
def go_parse(s):
    neg=False
    if s and s[0] in '+-':neg=s[0]=='-';s=s[1:]
    if s=='0':return 'ok(0)'
    if not s:return 'err(TIME_SYNTAX: invalid duration)'
    total=0;syntax='err(TIME_SYNTAX: invalid duration)';rng='err(TIME_RANGE: duration overflows int64 nanoseconds)'
    while s:
        if not(s[0]=='.' or s[0].isdigit()):return syntax
        i=0;v=0
        while i<len(s) and s[i].isdigit():
            if v>(I64-1)//10:return rng
            v=v*10+int(s[i]);i+=1
            if v>I64:return rng
        pre=i>0;s=s[i:];f=0;scale=1.0;post=False
        if s and s[0]=='.':
            s=s[1:];j=0;over=False
            while j<len(s) and s[j].isdigit():
                if not over:
                    if f>(I64-1)//10:over=True
                    else:
                        y=f*10+int(s[j])
                        if y>I64:over=True
                        else:f=y;scale*=10
                j+=1
            post=j>0;s=s[j:]
        if not pre and not post:return syntax
        k=0
        while k<len(s) and s[k]!='.' and not s[k].isdigit():k+=1
        if k==0:return syntax
        unit=UNITS.get(s[:k]);s=s[k:]
        if unit is None:return syntax
        if v>I64//unit:return rng
        v*=unit
        if f>0:
            v+=int(float(f)*(float(unit)/scale))
            if v>I64:return rng
        total+=v
        if total>I64:return rng
    if neg:return f'ok({-total})'
    if total>I64-1:return rng
    return f'ok({total})'

rng=random.Random(20261024)
program=['import std.time as time','fn show(ms int) {','    let y, mo, d, h, mi, s, f = time.utc_parts(ms)','    print(y, mo, d, h, mi, s, f, time.format_rfc3339(ms), time.weekday(ms), time.day_of_year(ms))','}','fn main() {'];expected=[]
stamps=[MIN_MS,MIN_MS+1,-1,0,1,951782400000,951868799999,-2208988800000,4102444800000,MAX_MS]+[rng.randrange(MIN_MS,MAX_MS+1) for _ in range(300)]
for ms in stamps:
    d=dt(ms);program.append(f'    show({ms})')
    expected.append(f'{d.year} {d.month} {d.day} {d.hour} {d.minute} {d.second} {ms%1000} {rfc(ms)} {d.isoweekday()} {d.timetuple().tm_yday}')
    off=rng.randrange(-1439,1440)
    if MIN_MS<=ms+off*60000<=MAX_MS:
        a=abs(off);zone=f"{'-' if off<0 else '+'}{a//60:02d}:{a%60:02d}"
        program.append(f'    print(time.format_rfc3339_offset({ms}, {off}), unwrap(time.parse_rfc3339(time.format_rfc3339_offset({ms}, {off}))))');expected.append(f'{rfc(ms,zone,off)} {ms}')
    program.append(f'    print(unwrap(time.utc_from_parts({d.year}, {d.month}, {d.day}, {d.hour}, {d.minute}, {d.second}, {ms%1000})), unwrap(time.parse_rfc3339(time.format_rfc3339({ms}))))');expected.append(f'{ms} {ms}')
for y in [1,4,100,400,1900,2000,2024,2100,9999]:
    program.append(f'    print(time.is_leap_year({y}), time.days_in_month({y}, 2), time.days_in_month({y}, 12))');ly=(y%4==0 and y%100!=0) or y%400==0;expected.append(f"{'true' if ly else 'false'} {29 if ly else 28} 31")
bad=[('2026-10-24T05:22:60Z','err(TIME_RANGE: leap seconds are not representable)'),('2026-02-30T00:00:00Z','err(TIME_RANGE: date-time field out of range)'),
     ('2026-10-24T24:00:00Z','err(TIME_RANGE: date-time field out of range)'),('2026-10-24T05:22:15','err(TIME_SYNTAX: invalid RFC 3339 date-time)'),
     ('2026-10-24 05:22:15Z','err(TIME_SYNTAX: invalid RFC 3339 date-time)'),('2026-10-24T05:22:15.Z','err(TIME_SYNTAX: invalid RFC 3339 date-time)'),
     ('2026-10-24T05:22:15.1234567890Z','err(TIME_SYNTAX: invalid RFC 3339 date-time)'),('2026-10-24T05:22:15+24:00','err(TIME_RANGE: offset outside -23:59..+23:59)'),
     ('0000-12-31T00:00:00Z','err(TIME_RANGE: date-time field out of range)'),('0001-01-01T00:00:00+00:01','err(TIME_RANGE: time outside years 0001..9999)'),
     ('2026-10-24t05:22:15.9z','ok(1792819335900)'),('1970-01-01T00:00:00.123456789Z','ok(123)'),('2026-10-24T13:22:15+08:00','ok(1792819335000)')]
for t,want in bad:program.append(f'    print(time.parse_rfc3339("{t}"))');expected.append(want)
program+=['    print(time.utc_from_parts(1900, 2, 29, 0, 0, 0, 0), time.utc_from_parts(2026, 13, 1, 0, 0, 0, 0), time.utc_from_parts(2026, 1, 1, 0, 0, 0, 1000))']
expected.append('err(TIME_RANGE: date-time field out of range) err(TIME_RANGE: date-time field out of range) err(TIME_RANGE: date-time field out of range)')
durations=[0,1,999,1000,1001,999999,10**6,10**9-1,10**9,10**9+1,59*10**9,60*10**9,3599*10**9,3600*10**9,I64-1,-I64,-1,-10**9]+[rng.randrange(-I64,I64) for _ in range(150)]+[rng.randrange(-10**12,10**12) for _ in range(100)]
for d in durations:
    program.append(f'    print(time.format_duration({lit(d)}), unwrap(time.parse_duration(time.format_duration({lit(d)}))))');expected.append(f'{go_format(d)} {d}')
texts=['1h30m','1.5s','-2m3.004005006s','300ms','2us','µs','1µs','1μs','0','+5h','-0','1d','','.s','s','1.','.5m','9223372036s','2562047h47m16.854775807s','-2562047h47m16.854775808s','2562047h47m16.854775808s','1h1h','99999999999999999999ns','0.000000001s','1.0000000000000000001s','3m.5s']
for t in texts:program.append(f'    print(time.parse_duration("{t}"))');expected.append(go_parse(t))
program.append('    let start = time.monotonic_ns()\n    print(time.since_ns(start) >= 0, time.since_ns(time.monotonic_ns() + 1000000000) < 0)');expected.append('true true')
program.append('}');source='\n'.join(program)+'\n';expected='\n'.join(expected)+'\n'

with tempfile.TemporaryDirectory(prefix='hua-time-',dir=build) as temp:
    base=Path(temp);src=base/'source';src.mkdir();work=base/'work';work.mkdir();n=0
    def fixture(text):
        global n
        n+=1;p=src/f'case{n}.hua';p.write_text(text,encoding='utf-8',newline='\n');return p
    def triple(text,want):
        p=fixture(text);run('check',p,expected='OK\n');artifact=base/f'case{n}.huab';run('build',p,'-o',artifact)
        for command,target in [('interpret',p),('run',p),('run',artifact)]:
            if target==artifact:p.unlink();run('check',artifact,expected='OK\n')
            run(command,target,cwd=work,expected=want)
        return artifact
    def failure(text,error,static=False):
        p=fixture(text)
        if static:run('check',p,code=1,error=error);return
        run('check',p,expected='OK\n');artifact=base/f'case{n}.huab';run('build',p,'-o',artifact)
        for command,target in [('interpret',p),('run',p),('run',artifact)]:run(command,target,cwd=work,code=1,error=error)
    artifact=triple(source,expected)
    assert header_format(artifact)==15,header_format(artifact)
    old=triple('import std.time as time\nfn main() { print(time.unix_ms() > 0) }\n','true\n')
    assert header_format(old)<15,header_format(old)
    data=bytearray(artifact.read_bytes());struct.pack_into('<I',data,8,14);bad_artifact=base/'downgraded.huab';bad_artifact.write_bytes(bytes(data))
    run('run',bad_artifact,code=1,error='E6002')
    for body in ['    print(time.format_rfc3339(253402300800000))','    let a, b, c, d, e, f, g = time.utc_parts(-62135596800001)\n    print(a)','    print(time.format_rfc3339_offset(0, 1440))','    print(time.days_in_month(2026, 0))','    print(time.format_rfc3339_offset(253402300799999, 1))']:
        failure('import std.time as time\nfn main() {\n'+body+'\n}\n','E4003')
    for body in ['print(time.format_rfc3339(1.5))','print(time.parse_duration(5))','print(time.utc_from_parts(1, 2, 3))','let a, b = time.utc_parts(0)']:
        failure('import std.time as time\nfn main() {\n    '+body+'\n}\n','E300',True)
print(f'time three-path checks passed: {checks}')
