# Shared driver for standard library tests: one source runs on the AST interpreter, the
# source VM and a source-free HUAB. Usage: Cases(*sys.argv[1:], prefix) inside `with`.
from pathlib import Path
import struct,subprocess,tempfile

class Cases:
    def __init__(self,exe,root,build,prefix):
        self.exe=Path(exe).resolve();self.root=Path(root).resolve();self.build=Path(build).resolve()
        self.prefix=prefix;self.checks=0;self.count=0
    def __enter__(self):
        self.temp=tempfile.TemporaryDirectory(prefix=self.prefix,dir=self.build);self.base=Path(self.temp.name);return self
    def __exit__(self,*error):self.temp.cleanup()
    def source(self,text):
        self.count+=1;p=self.base/f'case{self.count}.hua';p.write_text(text,encoding='utf-8',newline='\n');return p
    def run(self,args,code=0,timeout=120,stdin=None):
        r=subprocess.run([str(self.exe),*map(str,args)],cwd=self.base,capture_output=True,timeout=timeout,input=stdin)
        out=r.stdout.decode('utf-8').replace('\r\n','\n');err=r.stderr.decode('utf-8').replace('\r\n','\n')
        assert r.returncode==code,(args,r.returncode,out,err);self.checks+=1;return out,err
    def three(self,text,code=0,args=()):
        """AST interpreter, source VM and source-free HUAB; returns ([(stdout,stderr)],artifact)."""
        p=self.source(text);artifact=p.with_suffix('.huab');self.run(['build',p,'-o',artifact])
        extra=['--',*args] if args else []
        results=[self.run(['interpret',p,*extra],code),self.run(['run',p,*extra],code)]
        hidden=p.with_suffix('.moved');p.rename(hidden);results.append(self.run(['run',artifact,*extra],code));hidden.rename(p)
        return results,artifact
    def same(self,text,want,args=()):
        results,artifact=self.three(text,args=args)
        assert all(o==want for o,_ in results),(want,results);return artifact
    def fails(self,text,error):
        results,_=self.three(text,1)
        assert all(error in e for _,e in results),(text,error,results)
    def static(self,text,error):
        _,e=self.run(['check',self.source(text)],1);assert error in e,(text,error,e)
    def header(self,artifact,format_number):
        data=bytearray(artifact.read_bytes());found=struct.unpack_from('<I',data,8)[0]
        assert found==format_number,(found,format_number)
        struct.pack_into('<I',data,8,format_number-1);bad=self.base/f'downgraded{format_number}.huab';bad.write_bytes(bytes(data))
        _,e=self.run(['run',bad],1);assert 'E6002' in e,e
