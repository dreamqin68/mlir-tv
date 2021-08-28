// VERIFY

func @f(%idx: index, %idx2: index) -> (f32, f32) {
  %local = memref.alloc(): memref<8xf32>
  %local2 = memref.alloc(): memref<8xf32>
  %f1 = constant 1.0: f32
  %f2 = constant 2.0: f32
  %c1 = constant 0: index
  %c2 = constant 2: index

  memref.store %f1, %local[%idx]: memref<8xf32>
  memref.store %f2, %local2[%idx2]: memref<8xf32>
  %v1 = memref.load %local[%idx]: memref<8xf32>
  %v2 = memref.load %local2[%idx2]: memref<8xf32>
  return %v1, %v2: f32, f32
}
