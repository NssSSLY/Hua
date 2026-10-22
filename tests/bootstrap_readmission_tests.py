"""Independent numerical and freeze precondition checks; no compiler/VM workloads."""
from pathlib import Path
import sys,unittest,math
sys.path.insert(0,str(Path(__file__).resolve().parent/'tools'))
from audit_bootstrap_readmission import fit,predict
class ModelTests(unittest.TestCase):
 def test_known_nonnegative_linear_system(self):
  rows=[{'x':1,'z':0},{'x':0,'z':1},{'x':2,'z':1},{'x':1,'z':3}]
  m=fit(rows,[2,3,7,11],['x','z']);self.assertAlmostEqual(predict(m,{'x':4,'z':2}),14,places=8)
 def test_vm_step_scale_does_not_make_matrix_singular(self):
  rows=[{'x':1,'z':0},{'x':0,'z':1},{'x':2,'z':1},{'x':1,'z':3}]
  m=fit(rows,[200000000,300000000,700000000,1100000000],['x','z']);self.assertAlmostEqual(predict(m,{'x':4,'z':2})/1e8,14,places=8)
 def test_collinear_features_use_valid_subset(self):
  m=fit([{'x':1,'z':2},{'x':3,'z':6}],[4,12],['x','z']);self.assertAlmostEqual(predict(m,{'x':2,'z':4}),8)
 def test_zero_targets(self):self.assertEqual(predict(fit([{'x':1},{'x':2}],[0,0],['x']),{'x':9}),0)
 def test_two_independent_scales_required(self):
  with self.assertRaises(AssertionError):fit([{'x':1}],[2],['x'])
 def test_negative_feature_rejected(self):
  with self.assertRaises(AssertionError):fit([{'x':-1},{'x':2}],[1,2],['x'])
 def test_nonfinite_target_rejected(self):
  with self.assertRaises(AssertionError):fit([{'x':1},{'x':2}],[1,float('nan')],['x'])
 def test_nonfinite_prediction_feature_rejected(self):
  with self.assertRaises(AssertionError):predict({'coefficients':{'x':2}},{'x':float('inf')})
if __name__=='__main__':unittest.main()
