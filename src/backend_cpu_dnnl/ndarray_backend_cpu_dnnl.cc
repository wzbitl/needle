#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "cpu_dnnl.hpp"
#include "dnnl_debug.h"
#include "oneapi/dnnl/dnnl.hpp"

namespace needle {
namespace cpu_dnnl {

void conv_forward_dnnl(scalar_t* input, scalar_t* weight, scalar_t* output,
                       const memory::dim N, const memory::dim H,
                       const memory::dim W, const memory::dim C_in,
                       const memory::dim C_out, const memory::dim K,
                       const memory::dim S, const memory::dim P) {
  // Create execution dnnl::engine.
  dnnl::engine engine(engine::kind::cpu, 0);
  // Create dnnl::stream.
  dnnl::stream engine_stream(engine);
  const memory::dim NH = (H - K + 2 * P) / S + 1;
  const memory::dim NW = (W - K + 2 * P) / S + 1;
  memory::dims input_dims = {N, C_in, H, W};
  memory::dims weight_dims = {C_out, C_in, K, K};
  memory::dims output_dims = {N, C_out, NH, NW};
  memory::dims strides_dims = {S, S};
  memory::dims padding_dims_l = {P, P};
  memory::dims padding_dims_r = {P, P};

  // Create memory objects for tensor data. input: NHWC layout, weight: HWIO
  // layout, output: NHWC layout.
  auto user_input_mem = memory({input_dims, dt::f32, tag::nhwc}, engine);
  auto user_weight_mem = memory({weight_dims, dt::f32, tag::hwio}, engine);
  auto user_output_mem = memory({output_dims, dt::f32, tag::nhwc}, engine);
  // Let convolution primitive choose an optimized memory layouts
  auto conv_input_md = memory::desc(input_dims, dt::f32, tag::any);
  auto conv_weight_md = memory::desc(weight_dims, dt::f32, tag::any);
  auto conv_output_md = memory::desc(output_dims, dt::f32, tag::any);
  // Write data to memory object's handle.
  write_to_dnnl_memory(input, user_input_mem);
  write_to_dnnl_memory(weight, user_weight_mem);
  // Create primitive descriptor.
  auto conv_pd = convolution_forward::primitive_desc(
      engine, prop_kind::forward_inference, algorithm::convolution_direct,
      conv_input_md, conv_weight_md, conv_output_md, strides_dims,
      padding_dims_l, padding_dims_r);

  auto conv_input_mem = user_input_mem;
  auto conv_weight_mem = user_weight_mem;
  auto conv_output_mem = user_output_mem;
  // Reorder memory layouts if needed
  if (conv_pd.src_desc() != user_input_mem.get_desc()) {
    conv_input_mem = memory(conv_pd.src_desc(), engine);
    reorder(user_input_mem, conv_input_mem)
        .execute(engine_stream, user_input_mem, conv_input_mem);
  }
  if (conv_pd.weights_desc() != user_weight_mem.get_desc()) {
    conv_weight_mem = memory(conv_pd.weights_desc(), engine);
    reorder(user_weight_mem, conv_weight_mem)
        .execute(engine_stream, user_weight_mem, conv_weight_mem);
  }
  if (conv_pd.dst_desc() != user_output_mem.get_desc()) {
    conv_output_mem = memory(conv_pd.dst_desc(), engine);
  }
  // Create conv primitive.
  auto conv_prim = convolution_forward(conv_pd);
  // Primitive arguments.
  std::unordered_map<int, memory> conv_args;
  conv_args.insert({DNNL_ARG_SRC, conv_input_mem});
  conv_args.insert({DNNL_ARG_WEIGHTS, conv_weight_mem});
  conv_args.insert({DNNL_ARG_DST, conv_output_mem});
  // Primitive execution: convolution.
  conv_prim.execute(engine_stream, conv_args);
  // Reorder the output if needed
  if (conv_pd.dst_desc() != user_output_mem.get_desc()) {
    reorder(conv_output_mem, user_output_mem)
        .execute(engine_stream, conv_output_mem, user_output_mem);
  } else {
    user_output_mem = conv_output_mem;
  }
  // Wait for the computation to finalize.
  engine_stream.wait();
  // Read data from memory object's handle.
  read_from_dnnl_memory(output, user_output_mem);
}

void conv_backward_weight_dnnl(scalar_t* input, scalar_t* weight_diff,
                               scalar_t* output_diff, const memory::dim N,
                               const memory::dim H, const memory::dim W,
                               const memory::dim C_in, const memory::dim C_out,
                               const memory::dim K, const memory::dim S,
                               const memory::dim P) {
  // Create execution dnnl::engine.
  dnnl::engine engine(engine::kind::cpu, 0);
  // Create dnnl::stream.
  dnnl::stream engine_stream(engine);
  const memory::dim NH = (H - K + 2 * P) / S + 1;
  const memory::dim NW = (W - K + 2 * P) / S + 1;
  memory::dims input_dims = {N, C_in, H, W};
  memory::dims weight_dims = {C_out, C_in, K, K};
  memory::dims output_dims = {N, C_out, NH, NW};
  memory::dims strides_dims = {S, S};
  memory::dims padding_dims_l = {P, P};
  memory::dims padding_dims_r = {P, P};

  auto user_weight_diff_mem =
      memory({{weight_dims}, dt::f32, tag::hwio}, engine);
  auto user_input_mem = memory({{input_dims}, dt::f32, tag::nhwc}, engine);
  auto user_output_diff_mem =
      memory({{output_dims}, dt::f32, tag::nhwc}, engine);

  write_to_dnnl_memory(input, user_input_mem);
  write_to_dnnl_memory(output_diff, user_output_diff_mem);

  // create memory descriptors
  auto conv_input_md = memory::desc(input_dims, dt::f32, tag::any);
  auto conv_weight_diff_md = memory::desc(weight_dims, dt::f32, tag::any);
  auto conv_output_diff_md = memory::desc(output_dims, dt::f32, tag::any);

  auto conv_pd = convolution_forward::primitive_desc(
      engine, prop_kind::forward_inference, algorithm::convolution_direct,
      conv_input_md, conv_weight_diff_md, conv_output_diff_md, strides_dims,
      padding_dims_l, padding_dims_r);

  // create backward convolution primitive descriptor
  auto conv_bwd_weight_pd = convolution_backward_weights::primitive_desc(
      engine, algorithm::convolution_direct, conv_input_md, conv_weight_diff_md,
      conv_output_diff_md, strides_dims, padding_dims_l, padding_dims_r,
      conv_pd);

  // create reorder primitives
  auto conv_bwd_input_mem = user_input_mem;
  auto conv_output_diff_mem = user_output_diff_mem;
  auto conv_weight_diff_mem = user_weight_diff_mem;
  if (conv_bwd_weight_pd.src_desc() != user_input_mem.get_desc()) {
    conv_bwd_input_mem = memory(conv_bwd_weight_pd.src_desc(), engine);
    reorder(user_input_mem, conv_bwd_input_mem)
        .execute(engine_stream, user_input_mem, conv_bwd_input_mem);
  }

  if (conv_bwd_weight_pd.diff_dst_desc() != user_output_diff_mem.get_desc()) {
    conv_output_diff_mem = memory(conv_bwd_weight_pd.diff_dst_desc(), engine);
    reorder(user_output_diff_mem, conv_output_diff_mem)
        .execute(engine_stream, user_output_diff_mem, conv_output_diff_mem);
  }
  if (conv_bwd_weight_pd.diff_weights_desc() !=
      user_weight_diff_mem.get_desc()) {
    conv_weight_diff_mem =
        memory(conv_bwd_weight_pd.diff_weights_desc(), engine);
  }

  // create backward convolution primitive
  auto conv_bwd_prim = convolution_backward_weights(conv_bwd_weight_pd);
  // Primitive arguments.
  std::unordered_map<int, memory> conv_bwd_args;
  conv_bwd_args.insert({DNNL_ARG_SRC, conv_bwd_input_mem});
  conv_bwd_args.insert({DNNL_ARG_DIFF_DST, conv_output_diff_mem});
  conv_bwd_args.insert({DNNL_ARG_DIFF_WEIGHTS, conv_weight_diff_mem});
  // Primitive execution: convolution backward.
  conv_bwd_prim.execute(engine_stream, conv_bwd_args);
  // create reorder primitives between conv diff weights and user diff weights
  // if needed
  if (conv_bwd_weight_pd.diff_weights_desc() !=
      user_weight_diff_mem.get_desc()) {
    reorder(conv_weight_diff_mem, user_weight_diff_mem)
        .execute(engine_stream, conv_weight_diff_mem, user_weight_diff_mem);
  } else {
    user_weight_diff_mem = conv_weight_diff_mem;
  }
  engine_stream.wait();
  read_from_dnnl_memory(weight_diff, user_weight_diff_mem);
}

void conv_backward_input_dnnl(scalar_t* input_diff, scalar_t* weight,
                              scalar_t* output_diff, const memory::dim N,
                              const memory::dim H, const memory::dim W,
                              const memory::dim C_in, const memory::dim C_out,
                              const memory::dim K, const memory::dim S,
                              const memory::dim P) {
  // Create execution dnnl::engine.
  dnnl::engine engine(engine::kind::cpu, 0);
  // Create dnnl::stream.
  dnnl::stream engine_stream(engine);
  const memory::dim NH = (H - K + 2 * P) / S + 1;
  const memory::dim NW = (W - K + 2 * P) / S + 1;
  memory::dims input_dims = {N, C_in, H, W};
  memory::dims weight_dims = {C_out, C_in, K, K};
  memory::dims output_dims = {N, C_out, NH, NW};
  memory::dims strides_dims = {S, S};
  memory::dims padding_dims_l = {P, P};
  memory::dims padding_dims_r = {P, P};

  auto user_weight_mem = memory({{weight_dims}, dt::f32, tag::hwio}, engine);
  auto user_input_diff_mem = memory({{input_dims}, dt::f32, tag::nhwc}, engine);
  auto user_output_diff_mem =
      memory({{output_dims}, dt::f32, tag::nhwc}, engine);

  write_to_dnnl_memory(weight, user_weight_mem);
  write_to_dnnl_memory(output_diff, user_output_diff_mem);

  // create memory descriptors
  auto conv_input_diff_md = memory::desc(input_dims, dt::f32, tag::any);
  auto conv_weight_md = memory::desc(weight_dims, dt::f32, tag::any);
  auto conv_output_diff_md = memory::desc(output_dims, dt::f32, tag::any);

  auto conv_pd = convolution_forward::primitive_desc(
      engine, prop_kind::forward_inference, algorithm::convolution_direct,
      conv_input_diff_md, conv_weight_md, conv_output_diff_md, strides_dims,
      padding_dims_l, padding_dims_r);

  // create backward convolution primitive descriptor
  auto conv_bwd_input_pd = convolution_backward_data::primitive_desc(
      engine, algorithm::convolution_direct, conv_input_diff_md, conv_weight_md,
      conv_output_diff_md, strides_dims, padding_dims_l, padding_dims_r,
      conv_pd);

  // create reorder primitives
  auto conv_input_diff_mem = user_input_diff_mem;
  auto conv_output_diff_mem = user_output_diff_mem;
  auto conv_bwd_weight_mem = user_weight_mem;
  if (conv_bwd_input_pd.weights_desc() != user_weight_mem.get_desc()) {
    conv_bwd_weight_mem = memory(conv_bwd_input_pd.weights_desc(), engine);
    reorder(user_weight_mem, conv_bwd_weight_mem)
        .execute(engine_stream, user_weight_mem, conv_bwd_weight_mem);
  }

  if (conv_bwd_input_pd.diff_dst_desc() != user_output_diff_mem.get_desc()) {
    conv_output_diff_mem = memory(conv_bwd_input_pd.diff_dst_desc(), engine);
    reorder(user_output_diff_mem, conv_output_diff_mem)
        .execute(engine_stream, user_output_diff_mem, conv_output_diff_mem);
  }
  if (conv_bwd_input_pd.diff_src_desc() != user_input_diff_mem.get_desc()) {
    conv_input_diff_mem = memory(conv_bwd_input_pd.diff_src_desc(), engine);
  }

  // create backward convolution primitive
  auto conv_bwd_prim = convolution_backward_data(conv_bwd_input_pd);
  // Primitive arguments.
  std::unordered_map<int, memory> conv_bwd_args;
  conv_bwd_args.insert({DNNL_ARG_DIFF_SRC, conv_input_diff_mem});
  conv_bwd_args.insert({DNNL_ARG_DIFF_DST, conv_output_diff_mem});
  conv_bwd_args.insert({DNNL_ARG_WEIGHTS, conv_bwd_weight_mem});
  // Primitive execution: convolution backward.
  conv_bwd_prim.execute(engine_stream, conv_bwd_args);
  // create reorder primitives
  if (conv_bwd_input_pd.diff_src_desc() != user_input_diff_mem.get_desc()) {
    reorder(conv_input_diff_mem, user_input_diff_mem)
        .execute(engine_stream, conv_input_diff_mem, user_input_diff_mem);
  } else {
    user_input_diff_mem = conv_input_diff_mem;
  }
  engine_stream.wait();
  read_from_dnnl_memory(input_diff, user_input_diff_mem);
}

}  // namespace cpu_dnnl
}  // namespace needle
