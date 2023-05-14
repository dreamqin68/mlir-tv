// VERIFY
// ARGS: --unroll-fp-sum-bound 18
func.func @conv() -> tensor<1x1x1x1xf32> {
    %img = arith.constant dense<[[[[-0.0,1.0],[-0.0,-0.0],[-0.0,-0.0]],[[-0.0,-0.0],[-0.0,-0.0],[-0.0,-0.0]],[[-0.0,-0.0],[-0.0,-0.0],[-0.0,-0.0]]]]> : tensor<1x3x3x2xf32>
    %filter = arith.constant dense<[[[[1.0,1.0],[1.0,1.0],[1.0,1.0]],[[1.0,1.0],[1.0,1.0],[1.0,1.0]],[[1.0,1.0],[1.0,1.0],[1.0,1.0]]]]> : tensor<1x3x3x2xf32>
    %c0 = arith.constant -0.0 : f32
    %bias = tensor.from_elements %c0: tensor<1xf32>
    %0 = "tosa.conv2d"(%img, %filter, %bias) {dilation = array<i64: 1, 1>, pad = array<i64: 0, 0, 0, 0>, stride = array<i64: 1, 1>} : (tensor<1x3x3x2xf32>, tensor<1x3x3x2xf32>, tensor<1xf32>) -> tensor<1x1x1x1xf32>
    return %0 : tensor<1x1x1x1xf32>
}