(module
  (func (export "add") (param i32 i32) (result i32)
    local.get 0 local.get 1 i32.add)
  (func (export "twice") (param f64) (result f64)
    local.get 0 f64.const 2 f64.mul)
)
