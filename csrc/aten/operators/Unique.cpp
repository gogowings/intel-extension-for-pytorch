#include <ATen/ATen.h>

#include <core/Memory.h>
#include <runtime/Utils.h>
#include <utils/DPCPP.h>
#include <iterator>
#include <tuple>

#include <ATen/AtenIpexTypeXPU.h>
#include "comm/ATDispatch.h"

#ifdef USE_ONEDPL
#include <oneapi/dpl/algorithm>
#include <oneapi/dpl/execution>
#include <oneapi/dpl/iterator>
#include <oneapi/dpl/numeric>
#endif

using namespace xpu::dpcpp;

namespace at {
namespace AtenIpexTypeXPU {
namespace impl {

#ifdef USE_ONEDPL
template <typename policy_t, typename scalar_t, typename not_equal_t>
Tensor compute_inverse(
    const policy_t& policy,
    scalar_t* data,
    int64_t num_inp,
    const Tensor& sorted_indices,
    const bool return_inverse,
    TensorOptions options,
    not_equal_t not_equal) {
  // inverse indices
  Tensor inverse_indices;
  auto data_begin = data;
  if (!return_inverse) {
    inverse_indices = at::empty({0}, options);
  } else {
    TORCH_CHECK(
        sorted_indices.defined(),
        "compute_inverse is invoked, but sorted_indices is undefined. Send a bug report!");
    int64_t* sorted_indices_ptr = sorted_indices.data_ptr<int64_t>();
    auto sorted_indices_begin = sorted_indices_ptr;
    Tensor inv_loc = at::empty({num_inp}, options);
    inverse_indices = at::empty({num_inp}, options);
    int64_t* inv_loc_ptr = inv_loc.data_ptr<int64_t>();
    auto inv_loc_begin = inv_loc_ptr;
    std::adjacent_difference(
        policy, data_begin, data_begin + num_inp, inv_loc_begin, not_equal);
    inv_loc[0] = 0;
    std::inclusive_scan(
        policy, inv_loc_begin, inv_loc_begin + num_inp, inv_loc_begin);

    auto zipped_begin =
        oneapi::dpl::make_zip_iterator(sorted_indices_begin, inv_loc_begin);
    std::stable_sort(
        policy, zipped_begin, zipped_begin + num_inp, [](auto lhs, auto rhs) {
          using std::get;
          return get<0>(lhs) < get<0>(rhs);
        });
    inverse_indices = inv_loc;
  }

  return inverse_indices;
}

template <
    typename policy_t,
    typename scalar_t,
    typename equal_t,
    typename equal_by_key_t>
std::tuple<Tensor, int64_t> compute_unique(
    const policy_t& policy,
    scalar_t* data,
    int64_t num_inp,
    const Tensor& sorted_indices,
    const bool return_counts,
    TensorOptions options,
    equal_t equal,
    equal_by_key_t equal_by_key) {
  auto data_begin = data;
  // unique and count
  Tensor counts = at::empty({0}, options);
  int64_t num_out;
  if (!return_counts) {
    num_out = std::unique(policy, data_begin, data_begin + num_inp, equal) -
        data_begin;
  } else {
    Tensor range = at::empty({0}, options);
    range = at::AtenIpexTypeXPU::arange_out(range, 0, num_inp + 1, 1);
    int64_t* range_ptr = range.data_ptr<int64_t>();
    auto range_begin = range_ptr;
    auto zipped_begin = oneapi::dpl::make_zip_iterator(data_begin, range_begin);
    num_out = std::unique(
                  policy, zipped_begin, zipped_begin + num_inp, equal_by_key) -
        zipped_begin;
    range[num_out] = num_inp;
    counts.resize_(num_out);
    int64_t* counts_ptr = counts.data_ptr<int64_t>();
    // auto counts_begin = oneapi::dpl::begin(counts_ptr);
    auto counts_begin = counts_ptr;
    std::adjacent_difference(
        policy, range_begin + 1, range_begin + num_out + 1, counts_begin);
  }

  return std::tuple<Tensor, int64_t>(counts, num_out);
}
#endif

template <typename scalar_t>
std::tuple<Tensor, Tensor, Tensor> unique_template(
    const Tensor& self,
    const bool consecutive,
    const bool return_inverse,
    const bool return_counts) {
#ifdef USE_ONEDPL
  auto& dpcpp_queue = dpcppGetCurrentQueue();
  auto policy = oneapi::dpl::execution::make_device_policy(dpcpp_queue);

  auto options = self.options().dtype(kLong);
  Tensor output = self.clone().reshape(-1);
  int64_t num_inp = output.numel();
  scalar_t* output_data = output.data_ptr<scalar_t>();
  auto output_begin = output_data;
  Tensor sorted_indices;
  if (!return_inverse) {
    if (!consecutive) {
      std::sort(policy, output_begin, output_begin + num_inp);
    }
  } else {
    sorted_indices = at::arange(0, num_inp, options);
    if (!consecutive) {
      int64_t* sorted_indices_ptr = sorted_indices.data_ptr<int64_t>();
      auto sorted_indices_begin = sorted_indices_ptr;
      auto zipped_begin =
          oneapi::dpl::make_zip_iterator(output_begin, sorted_indices_begin);
      std::stable_sort(
          policy, zipped_begin, zipped_begin + num_inp, [](auto lhs, auto rhs) {
            using std::get;
            return get<0>(lhs) < get<0>(rhs);
          });
    }
  }

  Tensor inverse_indices, counts;
  int64_t num_out;

  inverse_indices = compute_inverse(
      policy,
      output_data,
      num_inp,
      sorted_indices,
      return_inverse,
      options,
      [](auto lhs, auto rhs) -> bool {
        if (lhs != rhs) {
          return true;
        }
        return false;
      });

  std::tie(counts, num_out) = compute_unique(
      policy,
      output_data,
      num_inp,
      sorted_indices,
      return_counts,
      options,
      [](auto lhs, auto rhs) -> bool {
        if (lhs != rhs) {
          return false;
        }
        return true;
      },
      [](auto lhs, auto rhs) -> bool {
        using std::get;
        if (get<0>(lhs) != get<0>(rhs)) {
          return false;
        }
        return true;
      });
  output.resize_(num_out);

  if (return_inverse) {
    inverse_indices.resize_(self.sizes());
  }

  return std::tuple<Tensor, Tensor, Tensor>(output, inverse_indices, counts);
#else
  AT_ERROR("Unique is not implemented for backend: ", self.device());
#endif
}

template <typename scalar_t>
std::tuple<Tensor, Tensor, Tensor> unique_dim_template(
    const Tensor& self,
    const int64_t dim,
    const bool consecutive,
    const bool return_inverse,
    const bool return_counts) {
#ifdef USE_ONEDPL
  auto& dpcpp_queue = dpcppGetCurrentQueue();
  auto policy = oneapi::dpl::execution::make_device_policy(dpcpp_queue);

  auto sizes = self.sizes().vec();
  auto num_zero_dims = std::count(sizes.begin(), sizes.end(), 0);

  if (self.size(dim) == 0) {
    TORCH_CHECK(
        num_zero_dims == 1,
        "Number of zero sized dimensions is more than one, so unique cannot be applied ")
    Tensor output = at::empty({0}, self.options());
    Tensor inverse_indices = at::empty({0}, self.options().dtype(kLong));
    Tensor counts = at::empty({0}, self.options().dtype(kLong));

    return std::make_tuple(output, inverse_indices, counts);
  }

  TORCH_CHECK(
      num_zero_dims == 0,
      "There are 0 sized dimensions, and they aren't selected, so unique cannot be applied");

  int64_t num_inp = self.size(dim);
  auto options = self.options().dtype(kLong);
  Tensor input_flat = self.transpose(dim, 0).contiguous().view({num_inp, -1});
  int64_t n = input_flat.size(1);
  scalar_t* input_flat_ptr = input_flat.data_ptr<scalar_t>();

  Tensor indices = at::arange(0, num_inp, options);
  int64_t* indices_data = indices.data_ptr<int64_t>();
  auto indices_begin = indices_data;
  if (!consecutive) {
    std::sort(
        policy,
        indices_begin,
        indices_begin + num_inp,
        [=](int64_t a, int64_t b) -> bool {
          for (int64_t i = 0; i < n; ++i) {
            scalar_t lhs = input_flat_ptr[i + a * n];
            scalar_t rhs = input_flat_ptr[i + b * n];
            if (lhs < rhs) {
              return true;
            } else if (lhs > rhs) {
              return false;
            }
          }
          return false;
        });
  }
  Tensor origin_indices = indices.clone();
  Tensor inverse_indices, counts;
  int64_t num_out;

  inverse_indices = compute_inverse(
      policy,
      indices_data,
      num_inp,
      indices,
      return_inverse,
      options,
      [=](auto a, auto b) -> int64_t {
        for (int64_t i = 0; i < n; ++i) {
          scalar_t lhs = input_flat_ptr[i + a * n];
          scalar_t rhs = input_flat_ptr[i + b * n];
          if (lhs != rhs) {
            return 1;
          }
        }
        return 0;
      });

  std::tie(counts, num_out) = compute_unique(
      policy,
      indices_data,
      num_inp,
      indices,
      return_counts,
      options,
      [=](auto a, auto b) -> bool {
        for (int64_t i = 0; i < n; ++i) {
          scalar_t lhs = input_flat_ptr[i + a * n];
          scalar_t rhs = input_flat_ptr[i + b * n];
          if (lhs != rhs) {
            return false;
          }
        }
        return true;
      },
      [=](auto a, auto b) -> bool {
        using std::get;
        for (int64_t i = 0; i < n; ++i) {
          scalar_t lhs = input_flat_ptr[i + get<0>(a) * n];
          scalar_t rhs = input_flat_ptr[i + get<0>(b) * n];
          if (lhs != rhs) {
            return false;
          }
        }
        return true;
      });
  origin_indices.resize_(num_out);
  return std::tuple<Tensor, Tensor, Tensor>(
      self.index_select(dim, origin_indices), inverse_indices, counts);
#else
  AT_ERROR(
      "Unique with dimension is not implemented due to the lack of USM and oneDPL in buidling.");
#endif
}
} // namespace impl

std::tuple<Tensor, Tensor> _unique(
    const Tensor& self,
    const bool sorted,
    const bool return_inverse) {
  return IPEX_DISPATCH_ALL_TYPES(self.scalar_type(), "unique", [&] {
    Tensor output, inverse;
    std::tie(output, inverse, std::ignore) =
        impl::unique_template<scalar_t>(self, false, return_inverse, false);
    return std::make_tuple(output, inverse);
  });
}

std::tuple<Tensor, Tensor, Tensor> _unique2(
    const Tensor& self,
    const bool sorted,
    const bool return_inverse,
    const bool return_counts) {
  return IPEX_DISPATCH_ALL_TYPES(self.scalar_type(), "unique", [&] {
    return impl::unique_template<scalar_t>(
        self, false, return_inverse, return_counts);
  });
}

std::tuple<Tensor, Tensor, Tensor> unique_dim(
    const Tensor& self,
    const int64_t dim,
    const bool sorted,
    const bool return_inverse,
    const bool return_counts) {
  return IPEX_DISPATCH_ALL_TYPES(self.scalar_type(), "unique_dim", [&] {
    return impl::unique_dim_template<scalar_t>(
        self, dim, false, return_inverse, return_counts);
  });
}

std::tuple<Tensor, Tensor, Tensor> unique_dim_consecutive(
    const Tensor& self,
    const int64_t dim,
    const bool return_inverse,
    const bool return_counts) {
  return IPEX_DISPATCH_ALL_TYPES(self.scalar_type(), "unique_dim", [&] {
    return impl::unique_dim_template<scalar_t>(
        self, dim, true, return_inverse, return_counts);
  });
}

std::tuple<Tensor, Tensor, Tensor> unique_consecutive(
    const Tensor& self,
    const bool return_inverse,
    const bool return_counts,
    c10::optional<int64_t> dim) {
  if (!dim.has_value()) {
    return IPEX_DISPATCH_ALL_TYPES(self.scalar_type(), "unique", [&] {
      return impl::unique_template<scalar_t>(
          self, true, return_inverse, return_counts);
    });
  }
  return at::AtenIpexTypeXPU::unique_dim_consecutive(
      self, dim.value(), return_inverse, return_counts);
}

} // namespace AtenIpexTypeXPU
} // namespace at
