// Copyright 2022 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/builtins/ukernel/mmt4d.h"

#include "iree/builtins/ukernel/arch/mmt4d_select_tile_arch.h"
#include "iree/builtins/ukernel/mmt4d_select_tile_generic.h"

#define OUTSIDE_UINT_RANGE(value, bits) (((value) < 0) || ((value) >> (bits)))

static iree_ukernel_mmt4d_status_t iree_ukernel_mmt4d_validate(
    const iree_ukernel_mmt4d_params_t* params) {
  if (params->flags & ~IREE_VMVX_MATMUL_FLAG_ACCUMULATE) {
    return iree_ukernel_mmt4d_status_bad_flags;
  }
  switch (params->type) {
    case iree_ukernel_mmt4d_type_f32f32f32:
    case iree_ukernel_mmt4d_type_i8i8i32:
      break;
    default:
      return iree_ukernel_mmt4d_status_bad_type;
  }
  // Some implementations may wish to avoid supporting absurdly wide types. For
  // instance, K is the innermost (i.e. hottest) loop bound, so some 32bit
  // targets may benefit from K being int32, not int64. We still let K be of
  // type int64 to be future-proof, as types are hard to change later. But we
  // enforce a narrower range here, as we can always relax that later as needed.
  if (OUTSIDE_UINT_RANGE(params->M, 31) || OUTSIDE_UINT_RANGE(params->M, 31) ||
      OUTSIDE_UINT_RANGE(params->K, 31) || OUTSIDE_UINT_RANGE(params->M0, 15) ||
      OUTSIDE_UINT_RANGE(params->N0, 15) ||
      OUTSIDE_UINT_RANGE(params->K0, 15)) {
    return iree_ukernel_mmt4d_status_unsupported_huge_or_negative_dimension;
  }
  return iree_ukernel_mmt4d_status_ok;
}

// On success, *out_tile_func is the tile function to use to perform the mmt4d
// with the given *params.
static iree_ukernel_mmt4d_status_t iree_ukernel_mmt4d_select_tile_func(
    const iree_ukernel_mmt4d_params_t* params,
    iree_ukernel_mmt4d_tile_func_t* out_tile_func) {
  iree_ukernel_mmt4d_tile_func_t arch_tile_func =
      iree_ukernel_mmt4d_select_tile_func_arch(params);
  if (arch_tile_func) {
    *out_tile_func = arch_tile_func;
    return iree_ukernel_mmt4d_status_ok;
  }
  return iree_ukernel_mmt4d_select_tile_func_generic(params, out_tile_func);
}

// General mmt4d implementation, shared among all cases. The idea is that the
// only really performance-critical part is the inner-most loop, and that's
// handled by the tile_func passed as argument here. Sharing the outer loops
// across all cases is a roughly 2x code shrink compared to if we were
// emitting the whole loop nest for each case.
static void iree_ukernel_mmt4d_using_tile_func(
    const iree_ukernel_mmt4d_params_t* params,
    iree_ukernel_mmt4d_tile_func_t tile_func) {
  const int32_t M = params->M;
  const int32_t N = params->N;
  const int32_t K = params->K;
  const int16_t M0 = params->M0;
  const int16_t N0 = params->N0;
  const int16_t lhs_elem_size_log2 =
      iree_ukernel_mmt4d_lhs_elem_size_log2(params->type);
  const int16_t rhs_elem_size_log2 =
      iree_ukernel_mmt4d_rhs_elem_size_log2(params->type);
  const int16_t out_elem_size_log2 =
      iree_ukernel_mmt4d_out_elem_size_log2(params->type);
  char* out_tile_row = params->out_buffer;
  const char* lhs_panel = params->lhs_buffer;
  int32_t out_tile_size = (M0 * N0) << out_elem_size_log2;
  iree_ukernel_ssize_t lhs_panel_stride = params->lhs_stride
                                          << lhs_elem_size_log2;
  iree_ukernel_ssize_t rhs_panel_stride = params->rhs_stride
                                          << rhs_elem_size_log2;
  iree_ukernel_ssize_t out_stride = params->out_stride << out_elem_size_log2;
  for (int32_t i = 0; i < M; ++i) {
    char* out_tile = out_tile_row;
    const char* rhs_panel = params->rhs_buffer;
    for (int32_t j = 0; j < N; ++j) {
      tile_func(out_tile, lhs_panel, rhs_panel, K, params->flags, params);
      out_tile += out_tile_size;
      rhs_panel += rhs_panel_stride;
    }
    out_tile_row += out_stride;
    lhs_panel += lhs_panel_stride;
  }
}

IREE_UKERNEL_EXPORT iree_ukernel_mmt4d_status_t
iree_ukernel_mmt4d(const iree_ukernel_mmt4d_params_t* params) {
  IREE_UKERNEL_MMT4D_RETURN_IF_ERROR(iree_ukernel_mmt4d_validate(params));
  iree_ukernel_mmt4d_tile_func_t tile_func;
  IREE_UKERNEL_MMT4D_RETURN_IF_ERROR(
      iree_ukernel_mmt4d_select_tile_func(params, &tile_func));
  iree_ukernel_mmt4d_using_tile_func(params, tile_func);
  return iree_ukernel_mmt4d_status_ok;
}

IREE_UKERNEL_EXPORT const char* iree_ukernel_mmt4d_status_message(
    iree_ukernel_mmt4d_status_t status) {
  switch (status) {
    case iree_ukernel_mmt4d_status_bad_flags:
      return "bad mmt4d flags";
    case iree_ukernel_mmt4d_status_bad_type:
      return "bad mmt4d type enum";
    case iree_ukernel_mmt4d_status_unsupported_huge_or_negative_dimension:
      return "unsupported huge or negative size in mmt4d";
    case iree_ukernel_mmt4d_status_unsupported_generic_tile_size:
      return "tile size too large for the generic tile implementation";
    default:
      return "unknown";
  }
}
