#include <ATen/AccumulateType.h>
#include <ATen/Config.h>
#include <ATen/NativeFunctions.h>
#include <ATen/ceil_div.h>
#include <ATen/native/Pool.h>

#include <core/detail/IndexUtils.h>
#include <oneDNN/oneDNN.h>
#include <vector>
#include "comm/ATDispatch.h"
#include "comm/RegistrationDeclarations.h"
#include "utils/ComputeEngine.h"

using namespace dnnl;
using namespace at::native;
using namespace xpu::dpcpp;
using namespace xpu::oneDNN;

namespace at {
namespace AtenIpexTypeXPU {
namespace impl {

template <typename scalar_t, typename accscalar_t, typename index_t>
void avg_pool3d_out_frame(
    Tensor& work_input,
    Tensor& work_output,
    const int kT,
    const int kH,
    const int kW,
    const int dT,
    const int dH,
    const int dW,
    const int padT,
    const int padH,
    const int padW,
    const bool count_include_pad,
    const int offsetZ,
    const int totalZ,
    const int divisor_override) {
  index_t oWidth = work_output.size(-1);
  index_t oHeight = work_output.size(-2);
  index_t oDepth = work_output.size(-3);
  index_t iWidth = work_input.size(-1);
  index_t iHeight = work_input.size(-2);
  index_t iDepth = work_input.size(-3);

  index_t ostride0 = work_output.stride(0);
  index_t ostride1 = work_output.stride(1);
  index_t ostride2 = work_output.stride(2);
  index_t ostride3 = work_output.stride(3);

  index_t istride0 = work_input.stride(0);
  index_t istride1 = work_input.stride(1);
  index_t istride2 = work_input.stride(2);
  index_t istride3 = work_input.stride(3);

  // width size is fixed size = 32, height dim equals = dpcppMaxWorkGroupSize /
  // width_size
  index_t width_group_size = 32;
  index_t height_group_size = dpcppMaxWorkGroupSize() / width_group_size;
  index_t width_group_range = ceil_div<index_t>(oHeight, width_group_size);
  index_t height_group_range = ceil_div<index_t>(oHeight, height_group_size);

  index_t z_group_range = totalZ > 65535 ? 65535 : totalZ;

  scalar_t* input = work_input.data_ptr<scalar_t>();
  scalar_t* output = work_output.data_ptr<scalar_t>();
  auto cgf = DPCPP_Q_CGF(cgh) {
    auto kfn = DPCPP_Q_KFN(sycl::nd_item<3> item) {
      index_t oCol = item.get_global_id()[2];
      index_t oRow = item.get_global_id()[1];
      index_t oFrame = (item.get_group(0) + offsetZ) % oDepth;
      index_t slice = (item.get_group(0) + offsetZ) / oDepth;

      if (oRow < oHeight && oCol < oWidth) {
        accscalar_t sum = 0.0f;

        index_t tstart = oFrame * dT - padT;
        index_t hstart = oRow * dH - padH;
        index_t wstart = oCol * dW - padW;
        index_t tend = Numerics<index_t>::min(tstart + kT, iDepth + padT);
        index_t hend = Numerics<index_t>::min(hstart + kH, iHeight + padH);
        index_t wend = Numerics<index_t>::min(wstart + kW, iWidth + padW);
        index_t pool_size = (tend - tstart) * (hend - hstart) * (wend - wstart);

        tstart = Numerics<index_t>::max(tstart, 0);
        hstart = Numerics<index_t>::max(hstart, 0);
        wstart = Numerics<index_t>::max(wstart, 0);
        tend = Numerics<index_t>::min(tend, iDepth);
        hend = Numerics<index_t>::min(hend, iHeight);
        wend = Numerics<index_t>::min(wend, iWidth);

        if (tstart >= tend || hstart >= hend || wstart >= wend) {
          output
              [oCol * ostride3 + oRow * ostride2 + oFrame * ostride1 +
               slice * ostride0] = 0.0f;
          return;
        }

        accscalar_t divide_factor;
        if (divisor_override) {
          divide_factor = static_cast<accscalar_t>(divisor_override);
        } else {
          if (count_include_pad) {
            divide_factor = static_cast<accscalar_t>(pool_size);
          } else {
            divide_factor = static_cast<accscalar_t>(
                (tend - tstart) * (hend - hstart) * (wend - wstart));
          }
        }

        index_t ti, hi, wi;
        for (ti = tstart; ti < tend; ++ti) {
          for (hi = hstart; hi < hend; ++hi) {
            for (wi = wstart; wi < wend; ++wi) {
              scalar_t val = input
                  [wi * istride3 + hi * istride2 + ti * istride1 +
                   slice * istride0];
              sum += val;
            }
          }
        }
        output
            [oCol * ostride3 + oRow * ostride2 + oFrame * ostride1 +
             slice * ostride0] = static_cast<scalar_t>(sum / divide_factor);
      }
    };
    cgh.parallel_for(
        sycl::nd_range<3>(
            sycl::range<3>{
                z_group_range,
                height_group_range * height_group_size,
                width_group_range * width_group_size,
            },
            sycl::range<3>{1, height_group_size, width_group_size}),
        kfn);
  };

  DPCPP_Q_SUBMIT(dpcppGetCurrentQueue(), cgf);
}

void avg_pool3d_out_template(
    Tensor& output,
    const Tensor& input,
    IntArrayRef kernel_size,
    IntArrayRef stride,
    IntArrayRef padding,
    bool ceil_mode,
    bool count_include_pad,
    c10::optional<int64_t> divisor_override) {
  TORCH_CHECK(
      kernel_size.size() == 1 || kernel_size.size() == 3,
      "avg_pool3d: kernel_size must either be a single int, or a tuple of "
      "three ints");
  const int kD = safe_downcast<int, int64_t>(kernel_size[0]);
  const int kH = kernel_size.size() == 1
      ? kD
      : safe_downcast<int, int64_t>(kernel_size[1]);
  const int kW = kernel_size.size() == 1
      ? kD
      : safe_downcast<int, int64_t>(kernel_size[2]);

  TORCH_CHECK(
      stride.empty() || stride.size() == 1 || stride.size() == 3,
      "avg_pool3d: stride must either be omitted, a single int, or a tuple of "
      "three ints");
  const int dD = stride.empty() ? kD : safe_downcast<int, int64_t>(stride[0]);
  const int dH = stride.empty() ? kH
      : stride.size() == 1      ? dD
                                : safe_downcast<int, int64_t>(stride[1]);
  const int dW = stride.empty() ? kW
      : stride.size() == 1      ? dD
                                : safe_downcast<int, int64_t>(stride[2]);

  TORCH_CHECK(
      padding.size() == 1 || padding.size() == 3,
      "avg_pool3d: padding must either be a single int, or a tuple of three "
      "ints");
  const int padD = safe_downcast<int, int64_t>(padding[0]);
  const int padH =
      padding.size() == 1 ? padD : safe_downcast<int, int64_t>(padding[1]);
  const int padW =
      padding.size() == 1 ? padD : safe_downcast<int, int64_t>(padding[2]);

  /* Applies a 3D average pooling over an input signal composed of
     several input planes. This op only support 4D and 5D input. 4D: Input (C,
     D, H, W),  Output (C, D0, H0, W0) 5D: Input (N, C, D, H, W),  Output (N,
     C, D0, H0, W0)
  */
  TORCH_CHECK(
      (input.ndimension() == 4 || input.ndimension() == 5),
      "non-empty 4D or 5D (batch mode) tensor expected for input");

  /* sizes */
  const int64_t nbatch = input.ndimension() == 5 ? input.size(-5) : 1;
  const int64_t nblock = input.size(-4);
  const int64_t idepth = input.size(-3);
  const int64_t iheight = input.size(-2);
  const int64_t iwidth = input.size(-1);

  const int64_t outputDepth =
      pooling_output_shape<int64_t>(idepth, kD, padD, dD, 1, ceil_mode);
  const int64_t outputHeight =
      pooling_output_shape<int64_t>(iheight, kH, padH, dH, 1, ceil_mode);
  const int64_t outputWidth =
      pooling_output_shape<int64_t>(iwidth, kW, padW, dW, 1, ceil_mode);

  // if divisor==0 then we will ignore it
  int64_t divisor = 0;
  if (divisor_override.has_value()) {
    divisor = divisor_override.value();
  }

  pool3d_shape_check(
      input,
      nblock,
      kD,
      kH,
      kW,
      dD,
      dH,
      dW,
      padD,
      padH,
      padW,
      1,
      1,
      1,
      idepth,
      iheight,
      iwidth,
      outputDepth,
      outputHeight,
      outputWidth,
      "avg_pool3d_out_template()",
      /*check_input_size=*/true);

  xpu::COMPUTE_ENG real_eng =
      choose_compute_eng(xpu::COMPUTE_ENG::ONEDNN, input);

  // for onednn block format
  if (xpu::COMPUTE_ENG::ONEDNN == real_eng) {
    Tensor input_;
    if (input.ndimension() == 4) {
      // 4D: Input (C, D, H, W),  Output (C, D0, H0, W0)
      // cannot give channels last for 4D tensor from frontend user
      // perspective the 2nd dim is outputDepth, not channel dim
      input_ = input.contiguous();
      output.resize_({nblock, outputDepth, outputHeight, outputWidth});
    } else {
      // 5D: Input (N, C, D, H, W),  Output (N, C, D0, H0, W0)
      // smf supports ChannelsLast3D and Contiguous cases.
      auto smf = input.suggest_memory_format();
      input_ = contiguous_if_needed(input, smf);
      output.resize_(
          {nbatch, nblock, outputDepth, outputHeight, outputWidth}, smf);
    }
    std::vector<int64_t> kernel_size_vec = {kD, kH, kW};
    std::vector<int64_t> stride_vec = {dD, dH, dW};
    std::vector<int64_t> padding_vec = {padD, padH, padW};
    // per oneDNN definition, no dilation means dilation ratio is 0
    std::vector<int64_t> dilation_vec = {0, 0, 0};
    if (count_include_pad) {
      ::xpu::oneDNN::pooling<::xpu::oneDNN::alg::pooling_avg_include_padding>(
          output,
          input_,
          nbatch,
          nblock,
          idepth,
          iheight,
          iwidth,
          outputDepth,
          outputHeight,
          outputWidth,
          stride_vec,
          kernel_size_vec,
          dilation_vec,
          padding_vec,
          padding_vec);
    } else {
      ::xpu::oneDNN::pooling<::xpu::oneDNN::alg::pooling_avg_exclude_padding>(
          output,
          input_,
          nbatch,
          nblock,
          idepth,
          iheight,
          iwidth,
          outputDepth,
          outputHeight,
          outputWidth,
          stride_vec,
          kernel_size_vec,
          dilation_vec,
          padding_vec,
          padding_vec);
    }
    return;
  } else {
    // for plain format
    Tensor work_input = input.contiguous();
    Tensor work_output = output;
    if (input.ndimension() == 5) {
      work_input =
          work_input.reshape({nbatch * nblock, idepth, iheight, iwidth});
      work_output = at::zeros_like(output);
      work_output = work_output.reshape(
          {nbatch * nblock, outputDepth, outputHeight, outputWidth});
    }

    IPEX_DISPATCH_FLOATING_TYPES_AND2(
        kHalf, kBFloat16, input.scalar_type(), "avg_pool3d_out_template", [&] {
          using accscalar_t = acc_type<scalar_t, true>;
          int64_t totalZ = outputDepth * nblock * nbatch;
          int64_t offsetZ = 0;

          while (totalZ > 0) {
            if (xpu::dpcpp::detail::canUse32BitIndexMath(input)) {
              avg_pool3d_out_frame<scalar_t, accscalar_t, int32_t>(
                  work_input,
                  work_output,
                  kD,
                  kH,
                  kW,
                  dD,
                  dH,
                  dW,
                  padD,
                  padH,
                  padW,
                  count_include_pad,
                  offsetZ,
                  totalZ,
                  divisor);
            } else {
              avg_pool3d_out_frame<scalar_t, accscalar_t, int64_t>(
                  work_input,
                  work_output,
                  kD,
                  kH,
                  kW,
                  dD,
                  dH,
                  dW,
                  padD,
                  padH,
                  padW,
                  count_include_pad,
                  offsetZ,
                  totalZ,
                  divisor);
            }
            totalZ -= 65535;
            offsetZ += 65535;
          }
        });
    output = work_output.resize_as_(output);
  }
}

Tensor& avg_pool3d_backward_out_template(
    Tensor& gradInput,
    const Tensor& gradOutput,
    const Tensor& input,
    IntArrayRef kernel_size,
    IntArrayRef stride,
    IntArrayRef padding,
    bool ceil_mode,
    bool count_include_pad) {
  TORCH_CHECK(
      kernel_size.size() == 1 || kernel_size.size() == 3,
      "avg_pool3d: kernel_size must either be a single int, or a tuple of "
      "three ints");
  const int kD = safe_downcast<int, int64_t>(kernel_size[0]);
  const int kH = kernel_size.size() == 1
      ? kD
      : safe_downcast<int, int64_t>(kernel_size[1]);
  const int kW = kernel_size.size() == 1
      ? kD
      : safe_downcast<int, int64_t>(kernel_size[2]);
  std::vector<int64_t> kernel_vec = {kD, kH, kW};

  TORCH_CHECK(
      stride.empty() || stride.size() == 1 || stride.size() == 3,
      "avg_pool3d: stride must either be omitted, a single int, or a tuple of "
      "three ints");
  const int dD = stride.empty() ? kD : safe_downcast<int, int64_t>(stride[0]);
  const int dH = stride.empty() ? kH
      : stride.size() == 1      ? dD
                                : safe_downcast<int, int64_t>(stride[1]);
  const int dW = stride.empty() ? kW
      : stride.size() == 1      ? dD
                                : safe_downcast<int, int64_t>(stride[2]);
  std::vector<int64_t> stride_vec = {dD, dH, dW};

  TORCH_CHECK(
      padding.size() == 1 || padding.size() == 3,
      "avg_pool3d: padding must either be a single int, or a tuple of three "
      "ints");
  const int padD = safe_downcast<int, int64_t>(padding[0]);
  const int padH =
      padding.size() == 1 ? padD : safe_downcast<int, int64_t>(padding[1]);
  const int padW =
      padding.size() == 1 ? padD : safe_downcast<int, int64_t>(padding[2]);
  std::vector<int64_t> padding_vec = {padD, padH, padW};

  TORCH_CHECK(
      (input.ndimension() == 4 || input.ndimension() == 5),
      "non-empty 4D or 5D (batch mode) tensor expected for input");

  TORCH_CHECK(
      (gradOutput.ndimension() == 4 || gradOutput.ndimension() == 5),
      "non-empty 4D or 5D (batch mode) tensor expected for gradOutput");

  /* sizes */
  const int64_t nbatch = input.ndimension() == 5 ? input.size(-5) : 1;
  const int64_t nblock = input.size(-4);
  const int64_t idepth = input.size(-3);
  const int64_t iheight = input.size(-2);
  const int64_t iwidth = input.size(-1);

  const int64_t odepth = gradOutput.size(-3);
  const int64_t oheight = gradOutput.size(-2);
  const int64_t owidth = gradOutput.size(-1);

  const int64_t odepth_for_shape_check =
      pooling_output_shape<int64_t>(idepth, kD, padD, dD, 1, ceil_mode);
  const int64_t oheight_for_shape_check =
      pooling_output_shape<int64_t>(iheight, kH, padH, dH, 1, ceil_mode);
  const int64_t owidth_for_chape_check =
      pooling_output_shape<int64_t>(iwidth, kW, padW, dW, 1, ceil_mode);

  avg_pool3d_backward_shape_check(
      input,
      gradOutput,
      nblock,
      kD,
      kH,
      kW,
      dD,
      dH,
      dW,
      padD,
      padH,
      padW,
      idepth,
      iheight,
      iwidth,
      odepth,
      oheight,
      owidth,
      "avg_pool3d_backward_out_template()");

  // per oneDNN definition, no dilation means dilation ratio is 0
  std::vector<int64_t> dilation_vec = {0, 0, 0};
  if (count_include_pad) {
    ::xpu::oneDNN::pooling_backward<
        ::xpu::oneDNN::alg::pooling_avg_include_padding>(
        gradInput,
        gradOutput,
        input,
        nbatch,
        nblock,
        idepth,
        iheight,
        iwidth,
        odepth,
        oheight,
        owidth,
        stride_vec,
        kernel_vec,
        dilation_vec,
        padding_vec,
        padding_vec);
  } else {
    ::xpu::oneDNN::pooling_backward<
        ::xpu::oneDNN::alg::pooling_avg_exclude_padding>(
        gradInput,
        gradOutput,
        input,
        nbatch,
        nblock,
        idepth,
        iheight,
        iwidth,
        odepth,
        oheight,
        owidth,
        stride_vec,
        kernel_vec,
        dilation_vec,
        padding_vec,
        padding_vec);
  }
  return gradInput;
}
} // namespace impl

Tensor& avg_pool3d_out(
    const Tensor& input,
    IntArrayRef kernel_size,
    IntArrayRef stride,
    IntArrayRef padding,
    bool ceil_mode,
    bool count_include_pad,
    c10::optional<int64_t> divisor_override,
    Tensor& output) {
  impl::avg_pool3d_out_template(
      output,
      input,
      kernel_size,
      stride,
      padding,
      ceil_mode,
      count_include_pad,
      divisor_override);

  return output;
}

Tensor& avg_pool3d_backward_out(
    const Tensor& grad_output_,
    const Tensor& self_,
    IntArrayRef kernel_size,
    IntArrayRef stride,
    IntArrayRef padding,
    bool ceil_mode,
    bool count_include_pad,
    c10::optional<int64_t> divisor_override,
    Tensor& grad_input) {
  TORCH_CHECK(
      !divisor_override.has_value(),
      "dpcpp_avg_pool3d operator does not support divisor");
  Tensor self, grad_output;
  if (self_.ndimension() == 4) {
    // 4D: Input (C, D, H, W),  Output (C, D0, H0, W0)
    // cannot give channels last for 4D tensor from frontend user perspective
    // the 2nd dim is outputDepth, not channel dim
    self = self_.contiguous();
    grad_output = grad_output_.contiguous();
    grad_input.resize_as_(self);
  } else {
    // 5D: Input (N, C, D, H, W),  Output (N, C, D0, H0, W0)
    // smf supports ChannelsLast3D and Contiguous cases.
    auto smf = self_.suggest_memory_format();
    self = self_.contiguous(smf);
    grad_output = grad_output_.contiguous(smf);
    grad_input.resize_as_(self_, smf);
  }

  impl::avg_pool3d_backward_out_template(
      grad_input,
      grad_output,
      self,
      kernel_size,
      stride,
      padding,
      ceil_mode,
      count_include_pad);
  return grad_input;
}
} // namespace AtenIpexTypeXPU
} // namespace at