"""The prepared-source watchdog keeps phase limits and stops a stalled phase."""
import sys,tempfile,time,unittest
from pathlib import Path
from capability_prepared_support import execute

class PhaseGuard(unittest.TestCase):
    def call(self,folder,body,limit=.3,prepare_limit=None):
        trace=folder/'phase.trace'
        code='from pathlib import Path\nimport time\np=Path('+repr(str(trace))+')\ns=time.monotonic()\ndef event(n):\n with p.open("a") as f:f.write(n+" "+str(time.monotonic()-s)+"\\n")\n'+body
        return execute([sys.executable,'-c',code],trace,folder/'out',folder/'err',limit,time.monotonic()+5,prepare_limit=prepare_limit)
    def test_prepare_once_separate_phase_budgets(self):
        with tempfile.TemporaryDirectory() as d:
            r,m=self.call(Path(d),'event("prepare-begin")\ntime.sleep(.15)\nevent("prepare-end")\nevent("AST-begin")\ntime.sleep(.15)\nevent("AST-end")\nevent("VM-begin")\ntime.sleep(.15)\nevent("VM-end")\nprint("complete")')
            self.assertEqual(r.returncode,0);self.assertEqual(r.stdout.strip(),'complete');self.assertGreater(m['seconds'],.3)
    def test_separate_prepare_limit(self):
        with tempfile.TemporaryDirectory() as d:
            r,m=self.call(Path(d),'event("prepare-begin")\ntime.sleep(.35)\nevent("prepare-end")\nevent("AST-begin")\nevent("AST-end")\nevent("VM-begin")\nevent("VM-end")',.2,prepare_limit=.7)
            self.assertEqual(r.returncode,0)
    def test_prepare_limit_does_not_extend_executor(self):
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaisesRegex(RuntimeError,'phase deadline; no retry'):
                self.call(Path(d),'event("prepare-begin")\nevent("prepare-end")\nevent("AST-begin")\ntime.sleep(2)',.2,prepare_limit=.7)
    def test_stall_stops_and_retains_output(self):
        with tempfile.TemporaryDirectory() as d:
            folder=Path(d)
            with self.assertRaisesRegex(RuntimeError,'phase deadline; no retry'):
                self.call(folder,'event("prepare-begin")\nprint("partial",flush=True)\ntime.sleep(2)',.2)
            self.assertIn('partial',(folder/'out').read_text())
    def test_invalid_phase_does_not_extend_deadline(self):
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaisesRegex(RuntimeError,'invalid phase order'):
                self.call(Path(d),'event("prepare-begin")\nevent("arbitrary")\ntime.sleep(2)')
if __name__=='__main__':unittest.main()
