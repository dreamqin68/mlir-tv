#map0 = affine_map<(d0, d1, d2, d3) -> (d3)>
#map1 = affine_map<(d0, d1, d2, d3) -> (d0, d1, d2, d3)>
module  {
  func @depthwise3(%arg0: tensor<1x11x9x3xf32>, %arg1: tensor<3x1x3x11xf32>, %arg2: tensor<33xf32>) -> tensor<1x5x5x33xf32> {
    %0 = linalg.init_tensor [1, 5, 5, 3, 11] : tensor<1x5x5x3x11xf32>
    %cst = arith.constant 0.000000e+00 : f32
    %1 = linalg.fill(%cst, %0): f32, tensor<1x5x5x3x11xf32> -> tensor<1x5x5x3x11xf32> 
    %2 = linalg.init_tensor [1, 5, 5, 33] : tensor<1x5x5x33xf32>
    %3 = linalg.depthwise_conv_2d_nhwc_hwcm {dilations = dense<1> : tensor<2xi64>, strides = dense<2> : tensor<2xi64>} ins(%arg0, %arg1 : tensor<1x11x9x3xf32>, tensor<3x1x3x11xf32>) outs(%1 : tensor<1x5x5x3x11xf32>) -> tensor<1x5x5x3x11xf32>
    %4 = tensor.collapse_shape %3 [[0], [1], [2], [3, 4]] : tensor<1x5x5x3x11xf32> into tensor<1x5x5x33xf32>
    %5 = linalg.generic {indexing_maps = [#map0, #map1, #map1], iterator_types = ["parallel", "parallel", "parallel", "parallel"]} ins(%arg2, %4 : tensor<33xf32>, tensor<1x5x5x33xf32>) outs(%2 : tensor<1x5x5x33xf32>) {
    ^bb0(%arg3: f32, %arg4: f32, %arg5: f32):  // no predecessors
      %6 = arith.addf %arg3, %arg4 : f32
      linalg.yield %6 : f32
    } -> tensor<1x5x5x33xf32>
    return %5 : tensor<1x5x5x33xf32>
  }
}

