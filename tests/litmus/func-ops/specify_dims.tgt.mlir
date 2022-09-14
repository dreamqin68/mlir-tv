func.func private @dyn_tensor_1(%v: f32) -> tensor<?x?xf32>
func.func private @dyn_tensor_2(%v: f32) -> tensor<?x?xf32>

func.func @cast_tensor(%v1: f32) -> i1 {
  %t1 = func.call @dyn_tensor_1(%v1): (f32) -> tensor<?x?xf32>
  %t2 = func.call @dyn_tensor_2(%v1): (f32) -> tensor<?x?xf32>
  %c0 = arith.constant 1: index
  %d1 = tensor.dim %t1, %c0: tensor<?x?xf32>
  %d2 = tensor.dim %t2, %c0: tensor<?x?xf32>
  %r = arith.cmpi eq, %d1, %d2 : index
  return %r: i1
}
