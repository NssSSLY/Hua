"""Regression of BO-11 admission safety; no compiler/VM workloads."""
from pathlib import Path
import sys,unittest
sys.path.insert(0,str(Path(__file__).resolve().parent/'tools'))
from audit_bootstrap_scale import decide,phase_values,estimate,feature

class AdmissionTests(unittest.TestCase):
 def base(self,**updates):
  args=dict(step_errors={'parse':1,'capabilities':2},wall_errors={'parse':2},guarded_seconds=399,projected_steps=499999999,correctness=True,structural_verified=True,ordinary_timing_verified=True,call_depth_verified=True,report_limit_verified=True);args.update(updates);return decide(**args)
 def test_valid_margins_only_admit_one(self):
  result=self.base();self.assertTrue(result['admitted']);self.assertEqual(result['formal_attempts_authorized_by_this_audit'],1)
 def test_underestimated_phase_blocks_even_when_time_fits(self):self.assertFalse(self.base(step_errors={'capabilities':-78.4})['admitted'])
 def test_overestimated_phase_also_blocks_unreliable_model(self):self.assertFalse(self.base(step_errors={'capabilities':44.5})['admitted'])
 def test_step_budget_is_independent_from_seconds(self):self.assertFalse(self.base(guarded_seconds=100,projected_steps=500000001)['admitted'])
 def test_margin_not_formal_timeout(self):self.assertFalse(self.base(guarded_seconds=401)['admitted'])
 def test_wall_failure_not_hidden_by_aggregate(self):self.assertFalse(self.base(wall_errors={'types':-45,'parse':1})['admitted'])
 def test_missing_holdout(self):self.assertFalse(self.base(step_errors={},wall_errors={})['admitted'])
 def test_missing_correctness(self):self.assertFalse(self.base(correctness=False)['admitted'])
 def test_unverified_structural_proxy(self):self.assertFalse(self.base(structural_verified=False)['admitted'])
 def test_exact_policy_boundaries(self):self.assertTrue(self.base(step_errors={'capabilities':25},guarded_seconds=400,projected_steps=500000000)['admitted'])
 def test_diagnostic_timing_is_not_ordinary(self):self.assertFalse(self.base(ordinary_timing_verified=False)['admitted'])
 def test_ast_depth_cannot_certify_vm_call_depth(self):self.assertFalse(self.base(call_depth_verified=False)['admitted'])
 def test_report_limit_unknown_blocks(self):self.assertFalse(self.base(report_limit_verified=False)['admitted'])
 def test_nan_cannot_admit(self):self.assertFalse(self.base(projected_steps=float('nan'))['admitted'])
 def test_negative_work_cannot_admit(self):self.assertFalse(self.base(projected_steps=-1)['admitted'])
 def test_capability_proxy_not_reused_for_other_phases(self):
  c={'instructions':10,'ast_nodes':20,'source_functions':3}
  self.assertEqual(feature('capabilities',c,'interaction'),60);self.assertEqual(feature('capabilities',c,'first-order'),20);self.assertEqual(feature('parse',c,'interaction'),20);self.assertEqual(feature('encode',c,'interaction'),10)

if __name__=='__main__':unittest.main()
