#pragma once

#include <ATen/ATen.h>

#include <ATen/AtenIpexTypeXPU.h>
#include <oneDNN/LRUCache.h>
#include <oneDNN/Reorder.h>
#include <oneDNN/Runtime.h>
#include <quantized/Quantizer.h>
#include <runtime/Utils.h>
#include <tensor/Context.h>
#include "Utils.h"

#include <oneapi/dnnl/dnnl.hpp>

using namespace dnnl;
using namespace at::AtenIpexTypeXPU;
using namespace at::AtenIpexTypeQuantizedXPU;

namespace xpu {
namespace oneDNN {

template <
    dnnl::algorithm algo,
    dnnl::algorithm algo_post = dnnl::algorithm::binary_add>
static inline Tensor bin(
    Tensor& output,
    const Tensor& t1,
    const Tensor& t2,
    const Tensor t3 = at::Tensor()) {
  auto engine =
      GpuEngineManager::Instance().get_engine({kXPU, current_device()});
  auto strm = GpuStreamManager::Instance().get_stream();
  auto ctx1 = DPCPPTensorContext::get_tensor_ctx(t1);
  auto ctx2 = DPCPPTensorContext::get_tensor_ctx(t2);

  auto tar_ctx = ctx1.is_plain() ? (ctx2.is_plain() ? ctx1 : ctx2) : ctx1;
  auto tar_md = (ctx1.is_plain() && ctx2.is_plain())
      ? memory::desc(
            get_onednn_dims(t1), get_onednn_dtype(t1), get_onednn_strides(t1))
      : tar_ctx.meta();

  auto md1 = ctx1.is_plain()
      ? memory::desc(
            get_onednn_dims(t1), get_onednn_dtype(t1), get_onednn_strides(t1))
      : ctx1.meta();
  auto md2 = ctx2.is_plain()
      ? memory::desc(
            get_onednn_dims(t2), get_onednn_dtype(t2), get_onednn_strides(t2))
      : ctx2.meta();

  auto m1_usr = dpcpp_onednn_memory(md1, engine, t1.data_ptr());
  auto m2_usr = dpcpp_onednn_memory(md2, engine, t2.data_ptr());

  primitive_attr attr;

  post_ops post;
  memory::desc md3;
  memory m3_usr;
  if (t3.defined()) {
    auto ctx3 = DPCPPTensorContext::get_tensor_ctx(t3);
    md3 = ctx3.is_plain()
        ? memory::desc(
              get_onednn_dims(t3), get_onednn_dtype(t3), get_onednn_strides(t3))
        : ctx3.meta();
    m3_usr = dpcpp_onednn_memory(md3, engine, t3.data_ptr());
    post.append_binary(algo_post, md3);
    attr.set_post_ops(post);
  }

  if (t1.is_quantized()) {
    float t1_scale = t1.q_scale();
    float t2_scale = t2.q_scale();
    attr.set_scales(DNNL_ARG_SRC_0, 0, {t1_scale});
    attr.set_scales(DNNL_ARG_SRC_1, 0, {t2_scale});
  }

  Tensor _t1;
  auto m1 = m1_usr;
  if (md1 != tar_md) {
    _t1 = empty_opaque_tensor(tar_md, t1.options(), c10::nullopt);
    m1 = dpcpp_onednn_memory(tar_md, engine, _t1.data_ptr());
    xpu::oneDNN::reorder(t1, _t1);
    md1 = tar_md;
  }
  Tensor _t2;
  auto m2 = m2_usr;
  if (md2 != tar_md && t1.sizes() == t2.sizes()) {
    _t2 = empty_opaque_tensor(tar_md, t2.options(), c10::nullopt);
    m2 = dpcpp_onednn_memory(tar_md, engine, _t2.data_ptr());
    xpu::oneDNN::reorder(t2, _t2);
    md2 = tar_md;
  }

  // 1. output: undefined, lazy_reorder: off
  // 2. output: undefined, lazy_reorder: on, output: plain
  if (!output.defined() && tar_ctx.is_plain()) {
    auto ndim = t1.ndimension();
    if (4 == ndim) {
      if (!t1.is_contiguous() &&
          t1.is_contiguous(at::MemoryFormat::ChannelsLast)) {
        output = at::empty_like(t1, at::MemoryFormat::ChannelsLast);
      } else {
        output = at::empty_like(t1);
      }
    } else if (5 == ndim) {
      if (!t1.is_contiguous() &&
          t1.is_contiguous(at::MemoryFormat::ChannelsLast3d)) {
        output = at::empty_like(t1, at::MemoryFormat::ChannelsLast3d);
      } else {
        output = at::empty_like(t1);
      }
    } else {
      output = at::empty_like(t1);
    }
  }
  // 1. output: undefined, lazy_reorder: on, output: block
  // 2. output: defined, lazy_reorder: on, output block
  else if (Settings::I().is_onednn_layout_enabled() && !tar_ctx.is_plain()) {
    if (t1.is_quantized()) {
      auto quantizer =
          dpcpp_make_per_tensor_affine_quantizer(output.q_scale(), 0, kQInt8);
      output = empty_opaque_qtensor(tar_md, c10::nullopt, quantizer);
    } else {
      output = empty_opaque_tensor(tar_md, t1.options(), c10::nullopt);
    }
  }
  auto mo = dpcpp_onednn_memory(tar_md, engine, output.data_ptr());

#ifdef USE_PRIMITIVE_CACHE
  lru_key_t key;
  if (t3.defined()) {
    create_key(key, algo, md1, md2, tar_md, attr.get_post_ops().len());
  } else {
    create_key(key, algo, md1, md2, tar_md);
  }
#endif
  binary::primitive_desc pd;
  if (t3.defined()) {
    pd = binary::primitive_desc({algo, md1, md2, tar_md}, attr, engine);
  } else {
    pd = binary::primitive_desc({algo, md1, md2, tar_md}, engine);
  }
#ifdef USE_PRIMITIVE_CACHE
  auto prim = fetch_or_create_m<binary>(key, pd);
#else
  auto prim = binary(pd);
#endif

  if (t3.defined()) {
    prim.execute(
        strm,
        {{DNNL_ARG_SRC_0, m1},
         {DNNL_ARG_SRC_1, m2},
         {DNNL_ARG_ATTR_MULTIPLE_POST_OP(0) | DNNL_ARG_SRC_1, m3_usr},
         {DNNL_ARG_DST, mo}});
  } else {
    DPCPP_ONEDNN_EXEC(
        prim,
        strm,
        {{DNNL_ARG_SRC_0, m1}, {DNNL_ARG_SRC_1, m2}, {DNNL_ARG_DST, mo}});
  }

  return output;
}
} // namespace oneDNN
} // namespace xpu
