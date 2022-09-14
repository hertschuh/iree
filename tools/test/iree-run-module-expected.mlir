// RUN: (iree-compile --iree-hal-target-backends=vmvx %s | iree-run-module --device=local-task --entry_function=abs --function_input=f32=-2 --expected_output=f32=2.0) | FileCheck %s --check-prefix=SUCCESS-MATCHES
// RUN: (iree-compile --iree-hal-target-backends=vmvx %s | iree-run-module --device=local-task --entry_function=abs --function_input=f32=-2 --expected_output="(ignored)") | FileCheck %s --check-prefix=SUCCESS-IGNORED
// RUN: (iree-compile --iree-hal-target-backends=vmvx %s | not iree-run-module --device=local-task --entry_function=abs --function_input=f32=-2 --expected_output=4xf32=2.0) | FileCheck %s --check-prefix=FAILED-SHAPE
// RUN: (iree-compile --iree-hal-target-backends=vmvx %s | not iree-run-module --device=local-task --entry_function=abs --function_input=f32=-2 --expected_output=f32=4.5) | FileCheck %s --check-prefix=FAILED-VALUE
// RUN: (iree-compile --iree-hal-target-backends=vmvx %s | not iree-run-module --device=local-task --entry_function=abs --function_input=f32=-2 --expected_output=8) | FileCheck %s --check-prefix=FAILED-TYPE

// SUCCESS-MATCHES: [SUCCESS]
// SUCCESS-IGNORED: [SUCCESS]
// FAILED-VALUE: [FAILED] result[0]: element at index 0 (2) does not match the expected (4.5)
// FAILED-SHAPE: [FAILED] result[0]: metadata is f32; expected that the view matches 4xf32
// FAILED-TYPE: [FAILED] result[0]: variant types mismatch

func.func @abs(%input : tensor<f32>) -> (tensor<f32>) {
  %result = math.absf %input : tensor<f32>
  return %result : tensor<f32>
}
