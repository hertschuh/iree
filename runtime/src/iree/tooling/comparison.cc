// Copyright 2022 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/tooling/comparison.h"

#include <cstdint>
#include <cstdio>

#include "iree/base/api.h"
#include "iree/base/status_cc.h"
#include "iree/base/tracing.h"
#include "iree/hal/api.h"
#include "iree/modules/hal/module.h"
#include "iree/tooling/buffer_view_matchers.h"
#include "iree/tooling/vm_util.h"
#include "iree/vm/ref_cc.h"

using namespace iree;

// Prints a buffer view with contents without a trailing newline.
static iree_status_t iree_tooling_append_buffer_view_string(
    iree_hal_buffer_view_t* buffer_view, iree_host_size_t max_element_count,
    iree_string_builder_t* builder) {
  // NOTE: we could see how many bytes are available in the builder (capacity -
  // size) and then pass those in to the initial format - if there's enough
  // space it'll fill what it needs. We'd need to adjust the string builder
  // afterward somehow.
  iree_host_size_t required_length = 0;
  iree_status_t status = iree_hal_buffer_view_format(
      buffer_view, max_element_count, /*buffer_capacity=*/0, /*buffer=*/NULL,
      &required_length);
  if (!iree_status_is_out_of_range(status)) return status;
  char* buffer = NULL;
  IREE_RETURN_IF_ERROR(
      iree_string_builder_append_inline(builder, required_length, &buffer));
  if (!buffer) return iree_ok_status();
  return iree_hal_buffer_view_format(buffer_view, max_element_count,
                                     required_length + /*NUL=*/1, buffer,
                                     &required_length);
}

static iree_status_t iree_vm_append_variant_type_string(
    iree_vm_variant_t variant, iree_string_builder_t* builder) {
  if (iree_vm_variant_is_empty(variant)) {
    return iree_string_builder_append_string(builder, IREE_SV("empty"));
  } else if (iree_vm_variant_is_value(variant)) {
    const char* type = NULL;
    switch (variant.type.value_type) {
      case IREE_VM_VALUE_TYPE_I8:
        type = "i8";
        break;
      case IREE_VM_VALUE_TYPE_I16:
        type = "i16";
        break;
      case IREE_VM_VALUE_TYPE_I32:
        type = "i32";
        break;
      case IREE_VM_VALUE_TYPE_I64:
        type = "i64";
        break;
      case IREE_VM_VALUE_TYPE_F32:
        type = "f32";
        break;
      case IREE_VM_VALUE_TYPE_F64:
        type = "f64";
        break;
      default:
        type = "?";
        break;
    }
    return iree_string_builder_append_cstring(builder, type);
  } else if (iree_vm_variant_is_ref(variant)) {
    return iree_string_builder_append_string(
        builder, iree_vm_ref_type_name(variant.type.ref_type));
  } else {
    return iree_string_builder_append_string(builder, IREE_SV("unknown"));
  }
}

static bool iree_tooling_compare_values(int result_index,
                                        iree_vm_variant_t expected_variant,
                                        iree_vm_variant_t actual_variant,
                                        iree_string_builder_t* builder) {
  IREE_ASSERT_EQ(expected_variant.type.value_type,
                 actual_variant.type.value_type);
  switch (expected_variant.type.value_type) {
    case IREE_VM_VALUE_TYPE_I8:
      if (expected_variant.i8 != actual_variant.i8) {
        IREE_CHECK_OK(iree_string_builder_append_format(
            builder,
            "[FAILED] result[%d]: i8 values differ\n  expected: %" PRIi8
            "\n  actual: %" PRIi8 "\n",
            result_index, expected_variant.i8, actual_variant.i8));
        return false;
      }
      return true;
    case IREE_VM_VALUE_TYPE_I16:
      if (expected_variant.i16 != actual_variant.i16) {
        IREE_CHECK_OK(iree_string_builder_append_format(
            builder,
            "[FAILED] result[%d]: i16 values differ\n  expected: %" PRIi16
            "\n  actual: %" PRIi16 "\n",
            result_index, expected_variant.i16, actual_variant.i16));
        return false;
      }
      return true;
    case IREE_VM_VALUE_TYPE_I32:
      if (expected_variant.i32 != actual_variant.i32) {
        IREE_CHECK_OK(iree_string_builder_append_format(
            builder,
            "[FAILED] result[%d]: i32 values differ\n  expected: %" PRIi32
            "\n  actual: %" PRIi32 "\n",
            result_index, expected_variant.i32, actual_variant.i32));
        return false;
      }
      return true;
    case IREE_VM_VALUE_TYPE_I64:
      if (expected_variant.i64 != actual_variant.i64) {
        IREE_CHECK_OK(iree_string_builder_append_format(
            builder,
            "[FAILED] result[%d]: i64 values differ\n  expected: %" PRIi64
            "\n  actual: %" PRIi64 "\n",
            result_index, expected_variant.i64, actual_variant.i64));
        return false;
      }
      return true;
    case IREE_VM_VALUE_TYPE_F32:
      // TODO(benvanik): use tolerance flag.
      if (expected_variant.f32 != actual_variant.f32) {
        IREE_CHECK_OK(iree_string_builder_append_format(
            builder,
            "[FAILED] result[%d]: f32 values differ\n  expected: %G\n  actual: "
            "%G\n",
            result_index, expected_variant.f32, actual_variant.f32));
        return false;
      }
      return true;
    case IREE_VM_VALUE_TYPE_F64:
      // TODO(benvanik): use tolerance flag.
      if (expected_variant.f64 != actual_variant.f64) {
        IREE_CHECK_OK(iree_string_builder_append_format(
            builder,
            "[FAILED] result[%d]: f64 values differ\n  expected: %G\n  actual: "
            "%G\n",
            result_index, expected_variant.f64, actual_variant.f64));
        return false;
      }
      return true;
    default:
      IREE_CHECK_OK(iree_string_builder_append_format(
          builder, "[FAILED] result[%d]: unknown value type, cannot match\n",
          result_index));
      return false;
  }
}

static bool iree_tooling_compare_buffer_views(
    int result_index, iree_hal_buffer_view_t* expected_view,
    iree_hal_buffer_view_t* actual_view, iree_allocator_t host_allocator,
    iree_host_size_t max_element_count, iree_string_builder_t* builder) {
  iree_string_builder_t subbuilder;
  iree_string_builder_initialize(host_allocator, &subbuilder);

  // TODO(benvanik): take equality configuration from flags.
  iree_hal_buffer_equality_t equality = {
      IREE_HAL_BUFFER_EQUALITY_APPROXIMATE_ABSOLUTE,
  };
  bool did_match = false;
  IREE_CHECK_OK(iree_hal_buffer_view_match_equal(
      equality, expected_view, actual_view, &subbuilder, &did_match));
  if (did_match) {
    iree_string_builder_deinitialize(&subbuilder);
    return true;
  }
  IREE_CHECK_OK(iree_string_builder_append_format(
      builder, "[FAILED] result[%d]: ", result_index));
  IREE_CHECK_OK(iree_string_builder_append_string(
      builder, iree_string_builder_view(&subbuilder)));
  iree_string_builder_deinitialize(&subbuilder);

  IREE_CHECK_OK(
      iree_string_builder_append_string(builder, IREE_SV("\n  expected:\n")));
  IREE_CHECK_OK(iree_tooling_append_buffer_view_string(
      expected_view, max_element_count, builder));
  IREE_CHECK_OK(
      iree_string_builder_append_string(builder, IREE_SV("\n  actual:\n")));
  IREE_CHECK_OK(iree_tooling_append_buffer_view_string(
      actual_view, max_element_count, builder));
  IREE_CHECK_OK(iree_string_builder_append_string(builder, IREE_SV("\n")));

  return false;
}

static bool iree_tooling_compare_variants(int result_index,
                                          iree_vm_variant_t expected_variant,
                                          iree_vm_variant_t actual_variant,
                                          iree_allocator_t host_allocator,
                                          iree_host_size_t max_element_count,
                                          iree_string_builder_t* builder) {
  IREE_TRACE_SCOPE();

  if (iree_vm_variant_is_empty(expected_variant)) {
    return true;  // expected empty is sentinel for (ignored)
  } else if (iree_vm_variant_is_empty(actual_variant) &&
             iree_vm_variant_is_empty(expected_variant)) {
    return true;  // both empty
  } else if (iree_vm_variant_is_value(actual_variant) &&
             iree_vm_variant_is_value(expected_variant)) {
    if (expected_variant.type.value_type != actual_variant.type.value_type) {
      return iree_tooling_compare_values(result_index, expected_variant,
                                         actual_variant, builder);
    }
  } else if (iree_vm_variant_is_ref(actual_variant) &&
             iree_vm_variant_is_ref(expected_variant)) {
    if (iree_hal_buffer_view_isa(actual_variant.ref) &&
        iree_hal_buffer_view_isa(expected_variant.ref)) {
      return iree_tooling_compare_buffer_views(
          result_index, iree_hal_buffer_view_deref(expected_variant.ref),
          iree_hal_buffer_view_deref(actual_variant.ref), host_allocator,
          max_element_count, builder);
    }
  }

  IREE_CHECK_OK(iree_string_builder_append_format(
      builder, "[FAILED] result[%d]: ", result_index));
  IREE_CHECK_OK(iree_string_builder_append_string(
      builder, IREE_SV("variant types mismatch; expected ")));
  IREE_CHECK_OK(iree_vm_append_variant_type_string(expected_variant, builder));
  IREE_CHECK_OK(
      iree_string_builder_append_string(builder, IREE_SV(" but got ")));
  IREE_CHECK_OK(iree_vm_append_variant_type_string(actual_variant, builder));
  IREE_CHECK_OK(iree_string_builder_append_string(builder, IREE_SV("\n")));

  return false;
}

static bool iree_tooling_compare_variants_to_stream(
    int result_index, iree_vm_variant_t expected_variant,
    iree_vm_variant_t actual_variant, iree_allocator_t host_allocator,
    iree_host_size_t max_element_count, std::ostream* os) {
  iree_string_builder_t builder;
  iree_string_builder_initialize(host_allocator, &builder);
  bool did_match = iree_tooling_compare_variants(result_index, expected_variant,
                                                 actual_variant, host_allocator,
                                                 max_element_count, &builder);
  os->write(iree_string_builder_buffer(&builder),
            iree_string_builder_size(&builder));
  iree_string_builder_deinitialize(&builder);
  return did_match;
}

bool iree_tooling_compare_variant_lists(iree_vm_list_t* expected_list,
                                        iree_vm_list_t* actual_list,
                                        iree_allocator_t host_allocator,
                                        std::ostream* os) {
  IREE_TRACE_SCOPE();

  if (iree_vm_list_size(expected_list) != iree_vm_list_size(actual_list)) {
    *os << "[FAILED] expected " << iree_vm_list_size(expected_list)
        << " list elements but " << iree_vm_list_size(actual_list)
        << " provided\n";
    return false;
  }

  bool all_match = true;
  for (iree_host_size_t i = 0; i < iree_vm_list_size(expected_list); ++i) {
    iree_vm_variant_t expected_variant = iree_vm_variant_empty();
    IREE_CHECK_OK(
        iree_vm_list_get_variant(expected_list, i, &expected_variant));
    iree_vm_variant_t actual_variant = iree_vm_variant_empty();
    IREE_CHECK_OK(iree_vm_list_get_variant(actual_list, i, &actual_variant));
    bool did_match = iree_tooling_compare_variants_to_stream(
        (int)i, expected_variant, actual_variant, host_allocator,
        /*max_element_count=*/1024, os);
    if (!did_match) all_match = false;
  }

  return all_match;
}
