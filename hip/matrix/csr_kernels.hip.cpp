/*******************************<GINKGO LICENSE>******************************
Copyright (c) 2017-2022, the Ginkgo authors
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

1. Redistributions of source code must retain the above copyright
notice, this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright
notice, this list of conditions and the following disclaimer in the
documentation and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
contributors may be used to endorse or promote products derived from
this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
******************************<GINKGO LICENSE>*******************************/

#include "core/matrix/csr_kernels.hpp"


#include <algorithm>


#include <hip/hip_runtime.h>


#include <ginkgo/core/base/array.hpp>
#include <ginkgo/core/base/exception_helpers.hpp>
#include <ginkgo/core/base/math.hpp>
#include <ginkgo/core/matrix/coo.hpp>
#include <ginkgo/core/matrix/dense.hpp>
#include <ginkgo/core/matrix/ell.hpp>
#include <ginkgo/core/matrix/hybrid.hpp>
#include <ginkgo/core/matrix/sellp.hpp>


#include "core/components/fill_array_kernels.hpp"
#include "core/components/prefix_sum_kernels.hpp"
#include "core/matrix/csr_builder.hpp"
#include "core/matrix/dense_kernels.hpp"
#include "core/synthesizer/implementation_selection.hpp"
#include "hip/base/config.hip.hpp"
#include "hip/base/hipsparse_bindings.hip.hpp"
#include "hip/base/math.hip.hpp"
#include "hip/base/pointer_mode_guard.hip.hpp"
#include "hip/base/types.hip.hpp"
#include "hip/components/atomic.hip.hpp"
#include "hip/components/cooperative_groups.hip.hpp"
#include "hip/components/intrinsics.hip.hpp"
#include "hip/components/merging.hip.hpp"
#include "hip/components/reduction.hip.hpp"
#include "hip/components/segment_scan.hip.hpp"
#include "hip/components/thread_ids.hip.hpp"
#include "hip/components/uninitialized_array.hip.hpp"


namespace gko {
namespace kernels {
namespace hip {
/**
 * @brief The Compressed sparse row matrix format namespace.
 *
 * @ingroup csr
 */
namespace csr {


constexpr int default_block_size = 512;
constexpr int warps_in_block = 4;
constexpr int spmv_block_size = warps_in_block * config::warp_size;
constexpr int classical_overweight = 32;


/**
 * A compile-time list of the number items per threads for which spmv kernel
 * should be compiled.
 */
using compiled_kernels = syn::value_list<int, 3, 4, 6, 7, 8, 12, 14>;

using classical_kernels =
    syn::value_list<int, config::warp_size, 32, 16, 8, 4, 2, 1>;

using spgeam_kernels =
    syn::value_list<int, 1, 2, 4, 8, 16, 32, config::warp_size>;


#include "common/cuda_hip/matrix/csr_kernels.hpp.inc"


namespace host_kernel {


template <int items_per_thread, typename ValueType, typename IndexType>
void merge_path_spmv(syn::value_list<int, items_per_thread>,
                     std::shared_ptr<const HipExecutor> exec,
                     const matrix::Csr<ValueType, IndexType>* a,
                     const matrix::Dense<ValueType>* b,
                     matrix::Dense<ValueType>* c,
                     const matrix::Dense<ValueType>* alpha = nullptr,
                     const matrix::Dense<ValueType>* beta = nullptr)
{
    const IndexType total = a->get_size()[0] + a->get_num_stored_elements();
    const IndexType grid_num =
        ceildiv(total, spmv_block_size * items_per_thread);
    const auto grid = grid_num;
    const auto block = spmv_block_size;
    Array<IndexType> row_out(exec, grid_num);
    Array<ValueType> val_out(exec, grid_num);

    for (IndexType column_id = 0; column_id < b->get_size()[1]; column_id++) {
        if (alpha == nullptr && beta == nullptr) {
            const auto b_vals = b->get_const_values() + column_id;
            auto c_vals = c->get_values() + column_id;
            hipLaunchKernelGGL(
                HIP_KERNEL_NAME(
                    kernel::abstract_merge_path_spmv<items_per_thread>),
                grid, block, 0, 0, static_cast<IndexType>(a->get_size()[0]),
                as_hip_type(a->get_const_values()), a->get_const_col_idxs(),
                as_hip_type(a->get_const_row_ptrs()),
                as_hip_type(a->get_const_srow()), as_hip_type(b_vals),
                b->get_stride(), as_hip_type(c_vals), c->get_stride(),
                as_hip_type(row_out.get_data()),
                as_hip_type(val_out.get_data()));
            hipLaunchKernelGGL(kernel::abstract_reduce, 1, spmv_block_size, 0,
                               0, grid_num, as_hip_type(val_out.get_data()),
                               as_hip_type(row_out.get_data()),
                               as_hip_type(c_vals), c->get_stride());

        } else if (alpha != nullptr && beta != nullptr) {
            const auto b_vals = b->get_const_values() + column_id;
            auto c_vals = c->get_values() + column_id;
            hipLaunchKernelGGL(
                HIP_KERNEL_NAME(
                    kernel::abstract_merge_path_spmv<items_per_thread>),
                grid, block, 0, 0, static_cast<IndexType>(a->get_size()[0]),
                as_hip_type(alpha->get_const_values()),
                as_hip_type(a->get_const_values()), a->get_const_col_idxs(),
                as_hip_type(a->get_const_row_ptrs()),
                as_hip_type(a->get_const_srow()), as_hip_type(b_vals),
                b->get_stride(), as_hip_type(beta->get_const_values()),
                as_hip_type(c_vals), c->get_stride(),
                as_hip_type(row_out.get_data()),
                as_hip_type(val_out.get_data()));
            hipLaunchKernelGGL(kernel::abstract_reduce, 1, spmv_block_size, 0,
                               0, grid_num, as_hip_type(val_out.get_data()),
                               as_hip_type(row_out.get_data()),
                               as_hip_type(alpha->get_const_values()),
                               as_hip_type(c_vals), c->get_stride());
        } else {
            GKO_KERNEL_NOT_FOUND;
        }
    }
}

GKO_ENABLE_IMPLEMENTATION_SELECTION(select_merge_path_spmv, merge_path_spmv);


template <typename ValueType, typename IndexType>
int compute_items_per_thread(std::shared_ptr<const HipExecutor> exec)
{
#if GINKGO_HIP_PLATFORM_NVCC


    const int version =
        (exec->get_major_version() << 4) + exec->get_minor_version();
    // The num_item is decided to make the occupancy 100%
    // TODO: Extend this list when new GPU is released
    //       Tune this parameter
    // 128 threads/block the number of items per threads
    // 3.0 3.5: 6
    // 3.7: 14
    // 5.0, 5.3, 6.0, 6.2: 8
    // 5.2, 6.1, 7.0: 12
    int num_item = 6;
    switch (version) {
    case 0x50:
    case 0x53:
    case 0x60:
    case 0x62:
        num_item = 8;
        break;
    case 0x52:
    case 0x61:
    case 0x70:
        num_item = 12;
        break;
    case 0x37:
        num_item = 14;
    }


#else


    // HIP uses the minimal num_item to make the code work correctly.
    // TODO: this parameter should be tuned.
    int num_item = 6;


#endif  // GINKGO_HIP_PLATFORM_NVCC


    // Ensure that the following is satisfied:
    // sizeof(IndexType) + sizeof(ValueType)
    // <= items_per_thread * sizeof(IndexType)
    constexpr int minimal_num =
        ceildiv(sizeof(IndexType) + sizeof(ValueType), sizeof(IndexType));
    int items_per_thread = num_item * 4 / sizeof(IndexType);
    return std::max(minimal_num, items_per_thread);
}


template <int subwarp_size, typename ValueType, typename IndexType>
void classical_spmv(syn::value_list<int, subwarp_size>,
                    std::shared_ptr<const HipExecutor> exec,
                    const matrix::Csr<ValueType, IndexType>* a,
                    const matrix::Dense<ValueType>* b,
                    matrix::Dense<ValueType>* c,
                    const matrix::Dense<ValueType>* alpha = nullptr,
                    const matrix::Dense<ValueType>* beta = nullptr)
{
    const auto nwarps = exec->get_num_warps_per_sm() *
                        exec->get_num_multiprocessor() * classical_overweight;
    const auto gridx =
        std::min(ceildiv(a->get_size()[0], spmv_block_size / subwarp_size),
                 int64(nwarps / warps_in_block));
    const dim3 grid(gridx, b->get_size()[1]);
    const auto block = spmv_block_size;

    if (alpha == nullptr && beta == nullptr) {
        hipLaunchKernelGGL(
            HIP_KERNEL_NAME(kernel::abstract_classical_spmv<subwarp_size>),
            grid, block, 0, 0, a->get_size()[0],
            as_hip_type(a->get_const_values()), a->get_const_col_idxs(),
            as_hip_type(a->get_const_row_ptrs()),
            as_hip_type(b->get_const_values()), b->get_stride(),
            as_hip_type(c->get_values()), c->get_stride());

    } else if (alpha != nullptr && beta != nullptr) {
        hipLaunchKernelGGL(
            HIP_KERNEL_NAME(kernel::abstract_classical_spmv<subwarp_size>),
            grid, block, 0, 0, a->get_size()[0],
            as_hip_type(alpha->get_const_values()),
            as_hip_type(a->get_const_values()), a->get_const_col_idxs(),
            as_hip_type(a->get_const_row_ptrs()),
            as_hip_type(b->get_const_values()), b->get_stride(),
            as_hip_type(beta->get_const_values()), as_hip_type(c->get_values()),
            c->get_stride());
    } else {
        GKO_KERNEL_NOT_FOUND;
    }
}

GKO_ENABLE_IMPLEMENTATION_SELECTION(select_classical_spmv, classical_spmv);


}  // namespace host_kernel


template <typename ValueType, typename IndexType>
void spmv(std::shared_ptr<const HipExecutor> exec,
          const matrix::Csr<ValueType, IndexType>* a,
          const matrix::Dense<ValueType>* b, matrix::Dense<ValueType>* c)
{
    if (a->get_strategy()->get_name() == "load_balance") {
        components::fill_array(exec, c->get_values(),
                               c->get_num_stored_elements(), zero<ValueType>());
        const IndexType nwarps = a->get_num_srow_elements();
        if (nwarps > 0) {
            const dim3 csr_block(config::warp_size, warps_in_block, 1);
            const dim3 csr_grid(ceildiv(nwarps, warps_in_block),
                                b->get_size()[1]);
            hipLaunchKernelGGL(
                kernel::abstract_spmv, csr_grid, csr_block, 0, 0, nwarps,
                static_cast<IndexType>(a->get_size()[0]),
                as_hip_type(a->get_const_values()), a->get_const_col_idxs(),
                as_hip_type(a->get_const_row_ptrs()),
                as_hip_type(a->get_const_srow()),
                as_hip_type(b->get_const_values()),
                as_hip_type(b->get_stride()), as_hip_type(c->get_values()),
                as_hip_type(c->get_stride()));
        } else {
            GKO_NOT_SUPPORTED(nwarps);
        }
    } else if (a->get_strategy()->get_name() == "merge_path") {
        int items_per_thread =
            host_kernel::compute_items_per_thread<ValueType, IndexType>(exec);
        host_kernel::select_merge_path_spmv(
            compiled_kernels(),
            [&items_per_thread](int compiled_info) {
                return items_per_thread == compiled_info;
            },
            syn::value_list<int>(), syn::type_list<>(), exec, a, b, c);
    } else {
        bool try_sparselib = (a->get_strategy()->get_name() == "sparselib" ||
                              a->get_strategy()->get_name() == "cusparse");
        try_sparselib = try_sparselib &&
                        hipsparse::is_supported<ValueType, IndexType>::value;
        try_sparselib =
            try_sparselib && b->get_stride() == 1 && c->get_stride() == 1;
        // rocSPARSE has issues with zero matrices
        try_sparselib = try_sparselib && a->get_num_stored_elements() > 0;
        if (try_sparselib) {
            auto handle = exec->get_hipsparse_handle();
            auto descr = hipsparse::create_mat_descr();
            {
                hipsparse::pointer_mode_guard pm_guard(handle);
                auto row_ptrs = a->get_const_row_ptrs();
                auto col_idxs = a->get_const_col_idxs();
                auto alpha = one<ValueType>();
                auto beta = zero<ValueType>();
                hipsparse::spmv(handle, HIPSPARSE_OPERATION_NON_TRANSPOSE,
                                a->get_size()[0], a->get_size()[1],
                                a->get_num_stored_elements(), &alpha, descr,
                                a->get_const_values(), row_ptrs, col_idxs,
                                b->get_const_values(), &beta, c->get_values());
            }
            hipsparse::destroy(descr);
        } else {
            IndexType max_length_per_row = 0;
            using Tcsr = matrix::Csr<ValueType, IndexType>;
            if (auto strategy =
                    std::dynamic_pointer_cast<const typename Tcsr::classical>(
                        a->get_strategy())) {
                max_length_per_row = strategy->get_max_length_per_row();
            } else if (auto strategy = std::dynamic_pointer_cast<
                           const typename Tcsr::automatical>(
                           a->get_strategy())) {
                max_length_per_row = strategy->get_max_length_per_row();
            } else {
                // as a fall-back: use average row length, at least 1
                max_length_per_row = std::max<size_type>(
                    a->get_num_stored_elements() /
                        std::max<size_type>(a->get_size()[0], 1),
                    1);
            }
            host_kernel::select_classical_spmv(
                classical_kernels(),
                [&max_length_per_row](int compiled_info) {
                    return max_length_per_row >= compiled_info;
                },
                syn::value_list<int>(), syn::type_list<>(), exec, a, b, c);
        }
    }
}

GKO_INSTANTIATE_FOR_EACH_VALUE_AND_INDEX_TYPE(GKO_DECLARE_CSR_SPMV_KERNEL);


template <typename ValueType, typename IndexType>
void advanced_spmv(std::shared_ptr<const HipExecutor> exec,
                   const matrix::Dense<ValueType>* alpha,
                   const matrix::Csr<ValueType, IndexType>* a,
                   const matrix::Dense<ValueType>* b,
                   const matrix::Dense<ValueType>* beta,
                   matrix::Dense<ValueType>* c)
{
    if (a->get_strategy()->get_name() == "load_balance") {
        dense::scale(exec, beta, c);

        const IndexType nwarps = a->get_num_srow_elements();

        if (nwarps > 0) {
            const dim3 csr_block(config::warp_size, warps_in_block, 1);
            const dim3 csr_grid(ceildiv(nwarps, warps_in_block),
                                b->get_size()[1]);
            hipLaunchKernelGGL(
                kernel::abstract_spmv, csr_grid, csr_block, 0, 0, nwarps,
                static_cast<IndexType>(a->get_size()[0]),
                as_hip_type(alpha->get_const_values()),
                as_hip_type(a->get_const_values()), a->get_const_col_idxs(),
                as_hip_type(a->get_const_row_ptrs()),
                as_hip_type(a->get_const_srow()),
                as_hip_type(b->get_const_values()),
                as_hip_type(b->get_stride()), as_hip_type(c->get_values()),
                as_hip_type(c->get_stride()));
        } else {
            GKO_NOT_SUPPORTED(nwarps);
        }
    } else if (a->get_strategy()->get_name() == "merge_path") {
        int items_per_thread =
            host_kernel::compute_items_per_thread<ValueType, IndexType>(exec);
        host_kernel::select_merge_path_spmv(
            compiled_kernels(),
            [&items_per_thread](int compiled_info) {
                return items_per_thread == compiled_info;
            },
            syn::value_list<int>(), syn::type_list<>(), exec, a, b, c, alpha,
            beta);
    } else {
        bool try_sparselib = (a->get_strategy()->get_name() == "sparselib" ||
                              a->get_strategy()->get_name() == "cusparse");
        try_sparselib = try_sparselib &&
                        hipsparse::is_supported<ValueType, IndexType>::value;
        try_sparselib =
            try_sparselib && b->get_stride() == 1 && c->get_stride() == 1;
        // rocSPARSE has issues with zero matrices
        try_sparselib = try_sparselib && a->get_num_stored_elements() > 0;
        if (try_sparselib) {
            auto descr = hipsparse::create_mat_descr();

            auto row_ptrs = a->get_const_row_ptrs();
            auto col_idxs = a->get_const_col_idxs();

            hipsparse::spmv(exec->get_hipsparse_handle(),
                            HIPSPARSE_OPERATION_NON_TRANSPOSE, a->get_size()[0],
                            a->get_size()[1], a->get_num_stored_elements(),
                            alpha->get_const_values(), descr,
                            a->get_const_values(), row_ptrs, col_idxs,
                            b->get_const_values(), beta->get_const_values(),
                            c->get_values());

            hipsparse::destroy(descr);
        } else {
            IndexType max_length_per_row = 0;
            using Tcsr = matrix::Csr<ValueType, IndexType>;
            if (auto strategy =
                    std::dynamic_pointer_cast<const typename Tcsr::classical>(
                        a->get_strategy())) {
                max_length_per_row = strategy->get_max_length_per_row();
            } else if (auto strategy = std::dynamic_pointer_cast<
                           const typename Tcsr::automatical>(
                           a->get_strategy())) {
                max_length_per_row = strategy->get_max_length_per_row();
            } else {
                // as a fall-back: use average row length, at least 1
                max_length_per_row = std::max<size_type>(
                    a->get_num_stored_elements() /
                        std::max<size_type>(a->get_size()[0], 1),
                    1);
            }
            host_kernel::select_classical_spmv(
                classical_kernels(),
                [&max_length_per_row](int compiled_info) {
                    return max_length_per_row >= compiled_info;
                },
                syn::value_list<int>(), syn::type_list<>(), exec, a, b, c,
                alpha, beta);
        }
    }
}

GKO_INSTANTIATE_FOR_EACH_VALUE_AND_INDEX_TYPE(
    GKO_DECLARE_CSR_ADVANCED_SPMV_KERNEL);


template <typename ValueType, typename IndexType>
void spgemm(std::shared_ptr<const HipExecutor> exec,
            const matrix::Csr<ValueType, IndexType>* a,
            const matrix::Csr<ValueType, IndexType>* b,
            matrix::Csr<ValueType, IndexType>* c)
{
    if (hipsparse::is_supported<ValueType, IndexType>::value) {
        auto handle = exec->get_hipsparse_handle();
        hipsparse::pointer_mode_guard pm_guard(handle);
        auto a_descr = hipsparse::create_mat_descr();
        auto b_descr = hipsparse::create_mat_descr();
        auto c_descr = hipsparse::create_mat_descr();
        auto d_descr = hipsparse::create_mat_descr();
        auto info = hipsparse::create_spgemm_info();

        auto alpha = one<ValueType>();
        auto a_nnz = static_cast<IndexType>(a->get_num_stored_elements());
        auto a_vals = a->get_const_values();
        auto a_row_ptrs = a->get_const_row_ptrs();
        auto a_col_idxs = a->get_const_col_idxs();
        auto b_nnz = static_cast<IndexType>(b->get_num_stored_elements());
        auto b_vals = b->get_const_values();
        auto b_row_ptrs = b->get_const_row_ptrs();
        auto b_col_idxs = b->get_const_col_idxs();
        auto null_value = static_cast<ValueType*>(nullptr);
        auto null_index = static_cast<IndexType*>(nullptr);
        auto zero_nnz = IndexType{};
        auto m = static_cast<IndexType>(a->get_size()[0]);
        auto n = static_cast<IndexType>(b->get_size()[1]);
        auto k = static_cast<IndexType>(a->get_size()[1]);
        auto c_row_ptrs = c->get_row_ptrs();
        matrix::CsrBuilder<ValueType, IndexType> c_builder{c};
        auto& c_col_idxs_array = c_builder.get_col_idx_array();
        auto& c_vals_array = c_builder.get_value_array();

        // allocate buffer
        size_type buffer_size{};
        hipsparse::spgemm_buffer_size(
            handle, m, n, k, &alpha, a_descr, a_nnz, a_row_ptrs, a_col_idxs,
            b_descr, b_nnz, b_row_ptrs, b_col_idxs, null_value, d_descr,
            zero_nnz, null_index, null_index, info, buffer_size);
        Array<char> buffer_array(exec, buffer_size);
        auto buffer = buffer_array.get_data();

        // count nnz
        IndexType c_nnz{};
        hipsparse::spgemm_nnz(
            handle, m, n, k, a_descr, a_nnz, a_row_ptrs, a_col_idxs, b_descr,
            b_nnz, b_row_ptrs, b_col_idxs, d_descr, zero_nnz, null_index,
            null_index, c_descr, c_row_ptrs, &c_nnz, info, buffer);

        // accumulate non-zeros
        c_col_idxs_array.resize_and_reset(c_nnz);
        c_vals_array.resize_and_reset(c_nnz);
        auto c_col_idxs = c_col_idxs_array.get_data();
        auto c_vals = c_vals_array.get_data();
        hipsparse::spgemm(handle, m, n, k, &alpha, a_descr, a_nnz, a_vals,
                          a_row_ptrs, a_col_idxs, b_descr, b_nnz, b_vals,
                          b_row_ptrs, b_col_idxs, null_value, d_descr, zero_nnz,
                          null_value, null_index, null_index, c_descr, c_vals,
                          c_row_ptrs, c_col_idxs, info, buffer);

        hipsparse::destroy_spgemm_info(info);
        hipsparse::destroy(d_descr);
        hipsparse::destroy(c_descr);
        hipsparse::destroy(b_descr);
        hipsparse::destroy(a_descr);
    } else {
        GKO_NOT_IMPLEMENTED;
    }
}

GKO_INSTANTIATE_FOR_EACH_VALUE_AND_INDEX_TYPE(GKO_DECLARE_CSR_SPGEMM_KERNEL);


namespace {


template <int subwarp_size, typename ValueType, typename IndexType>
void spgeam(syn::value_list<int, subwarp_size>,
            std::shared_ptr<const HipExecutor> exec, const ValueType* alpha,
            const IndexType* a_row_ptrs, const IndexType* a_col_idxs,
            const ValueType* a_vals, const ValueType* beta,
            const IndexType* b_row_ptrs, const IndexType* b_col_idxs,
            const ValueType* b_vals, matrix::Csr<ValueType, IndexType>* c)
{
    auto m = static_cast<IndexType>(c->get_size()[0]);
    auto c_row_ptrs = c->get_row_ptrs();
    // count nnz for alpha * A + beta * B
    auto subwarps_per_block = default_block_size / subwarp_size;
    auto num_blocks = ceildiv(m, subwarps_per_block);
    hipLaunchKernelGGL(HIP_KERNEL_NAME(kernel::spgeam_nnz<subwarp_size>),
                       num_blocks, default_block_size, 0, 0, a_row_ptrs,
                       a_col_idxs, b_row_ptrs, b_col_idxs, m, c_row_ptrs);

    // build row pointers
    components::prefix_sum(exec, c_row_ptrs, m + 1);

    // accumulate non-zeros for alpha * A + beta * B
    matrix::CsrBuilder<ValueType, IndexType> c_builder{c};
    auto c_nnz = exec->copy_val_to_host(c_row_ptrs + m);
    c_builder.get_col_idx_array().resize_and_reset(c_nnz);
    c_builder.get_value_array().resize_and_reset(c_nnz);
    auto c_col_idxs = c->get_col_idxs();
    auto c_vals = c->get_values();
    hipLaunchKernelGGL(
        HIP_KERNEL_NAME(kernel::spgeam<subwarp_size>), num_blocks,
        default_block_size, 0, 0, as_hip_type(alpha), a_row_ptrs, a_col_idxs,
        as_hip_type(a_vals), as_hip_type(beta), b_row_ptrs, b_col_idxs,
        as_hip_type(b_vals), m, c_row_ptrs, c_col_idxs, as_hip_type(c_vals));
}

GKO_ENABLE_IMPLEMENTATION_SELECTION(select_spgeam, spgeam);


}  // namespace


template <typename ValueType, typename IndexType>
void advanced_spgemm(std::shared_ptr<const HipExecutor> exec,
                     const matrix::Dense<ValueType>* alpha,
                     const matrix::Csr<ValueType, IndexType>* a,
                     const matrix::Csr<ValueType, IndexType>* b,
                     const matrix::Dense<ValueType>* beta,
                     const matrix::Csr<ValueType, IndexType>* d,
                     matrix::Csr<ValueType, IndexType>* c)
{
    if (hipsparse::is_supported<ValueType, IndexType>::value) {
        auto handle = exec->get_hipsparse_handle();
        hipsparse::pointer_mode_guard pm_guard(handle);
        auto a_descr = hipsparse::create_mat_descr();
        auto b_descr = hipsparse::create_mat_descr();
        auto c_descr = hipsparse::create_mat_descr();
        auto d_descr = hipsparse::create_mat_descr();
        auto info = hipsparse::create_spgemm_info();

        auto a_nnz = static_cast<IndexType>(a->get_num_stored_elements());
        auto a_vals = a->get_const_values();
        auto a_row_ptrs = a->get_const_row_ptrs();
        auto a_col_idxs = a->get_const_col_idxs();
        auto b_nnz = static_cast<IndexType>(b->get_num_stored_elements());
        auto b_vals = b->get_const_values();
        auto b_row_ptrs = b->get_const_row_ptrs();
        auto b_col_idxs = b->get_const_col_idxs();
        auto d_vals = d->get_const_values();
        auto d_row_ptrs = d->get_const_row_ptrs();
        auto d_col_idxs = d->get_const_col_idxs();
        auto null_value = static_cast<ValueType*>(nullptr);
        auto null_index = static_cast<IndexType*>(nullptr);
        auto one_value = one<ValueType>();
        auto m = static_cast<IndexType>(a->get_size()[0]);
        auto n = static_cast<IndexType>(b->get_size()[1]);
        auto k = static_cast<IndexType>(a->get_size()[1]);

        // allocate buffer
        size_type buffer_size{};
        hipsparse::spgemm_buffer_size(
            handle, m, n, k, &one_value, a_descr, a_nnz, a_row_ptrs, a_col_idxs,
            b_descr, b_nnz, b_row_ptrs, b_col_idxs, null_value, d_descr,
            IndexType{}, null_index, null_index, info, buffer_size);
        Array<char> buffer_array(exec, buffer_size);
        auto buffer = buffer_array.get_data();

        // count nnz
        Array<IndexType> c_tmp_row_ptrs_array(exec, m + 1);
        auto c_tmp_row_ptrs = c_tmp_row_ptrs_array.get_data();
        IndexType c_nnz{};
        hipsparse::spgemm_nnz(
            handle, m, n, k, a_descr, a_nnz, a_row_ptrs, a_col_idxs, b_descr,
            b_nnz, b_row_ptrs, b_col_idxs, d_descr, IndexType{}, null_index,
            null_index, c_descr, c_tmp_row_ptrs, &c_nnz, info, buffer);

        // accumulate non-zeros for A * B
        Array<IndexType> c_tmp_col_idxs_array(exec, c_nnz);
        Array<ValueType> c_tmp_vals_array(exec, c_nnz);
        auto c_tmp_col_idxs = c_tmp_col_idxs_array.get_data();
        auto c_tmp_vals = c_tmp_vals_array.get_data();
        hipsparse::spgemm(handle, m, n, k, &one_value, a_descr, a_nnz, a_vals,
                          a_row_ptrs, a_col_idxs, b_descr, b_nnz, b_vals,
                          b_row_ptrs, b_col_idxs, null_value, d_descr,
                          IndexType{}, null_value, null_index, null_index,
                          c_descr, c_tmp_vals, c_tmp_row_ptrs, c_tmp_col_idxs,
                          info, buffer);

        // destroy hipsparse context
        hipsparse::destroy_spgemm_info(info);
        hipsparse::destroy(d_descr);
        hipsparse::destroy(c_descr);
        hipsparse::destroy(b_descr);
        hipsparse::destroy(a_descr);

        auto total_nnz = c_nnz + d->get_num_stored_elements();
        auto nnz_per_row = total_nnz / m;
        select_spgeam(
            spgeam_kernels(),
            [&](int compiled_subwarp_size) {
                return compiled_subwarp_size >= nnz_per_row ||
                       compiled_subwarp_size == config::warp_size;
            },
            syn::value_list<int>(), syn::type_list<>(), exec,
            alpha->get_const_values(), c_tmp_row_ptrs, c_tmp_col_idxs,
            c_tmp_vals, beta->get_const_values(), d_row_ptrs, d_col_idxs,
            d_vals, c);
    } else {
        GKO_NOT_IMPLEMENTED;
    }
}

GKO_INSTANTIATE_FOR_EACH_VALUE_AND_INDEX_TYPE(
    GKO_DECLARE_CSR_ADVANCED_SPGEMM_KERNEL);


template <typename ValueType, typename IndexType>
void spgeam(std::shared_ptr<const DefaultExecutor> exec,
            const matrix::Dense<ValueType>* alpha,
            const matrix::Csr<ValueType, IndexType>* a,
            const matrix::Dense<ValueType>* beta,
            const matrix::Csr<ValueType, IndexType>* b,
            matrix::Csr<ValueType, IndexType>* c)
{
    auto total_nnz =
        a->get_num_stored_elements() + b->get_num_stored_elements();
    auto nnz_per_row = total_nnz / a->get_size()[0];
    select_spgeam(
        spgeam_kernels(),
        [&](int compiled_subwarp_size) {
            return compiled_subwarp_size >= nnz_per_row ||
                   compiled_subwarp_size == config::warp_size;
        },
        syn::value_list<int>(), syn::type_list<>(), exec,
        alpha->get_const_values(), a->get_const_row_ptrs(),
        a->get_const_col_idxs(), a->get_const_values(),
        beta->get_const_values(), b->get_const_row_ptrs(),
        b->get_const_col_idxs(), b->get_const_values(), c);
}

GKO_INSTANTIATE_FOR_EACH_VALUE_AND_INDEX_TYPE(GKO_DECLARE_CSR_SPGEAM_KERNEL);


template <typename ValueType, typename IndexType>
void fill_in_dense(std::shared_ptr<const HipExecutor> exec,
                   const matrix::Csr<ValueType, IndexType>* source,
                   matrix::Dense<ValueType>* result)
{
    const auto num_rows = result->get_size()[0];
    const auto num_cols = result->get_size()[1];
    const auto stride = result->get_stride();
    const auto row_ptrs = source->get_const_row_ptrs();
    const auto col_idxs = source->get_const_col_idxs();
    const auto vals = source->get_const_values();

    auto grid_dim = ceildiv(num_rows, default_block_size);
    hipLaunchKernelGGL(kernel::fill_in_dense, grid_dim, default_block_size, 0,
                       0, num_rows, as_hip_type(row_ptrs),
                       as_hip_type(col_idxs), as_hip_type(vals), stride,
                       as_hip_type(result->get_values()));
}

GKO_INSTANTIATE_FOR_EACH_VALUE_AND_INDEX_TYPE(
    GKO_DECLARE_CSR_FILL_IN_DENSE_KERNEL);


template <typename ValueType, typename IndexType>
void transpose(std::shared_ptr<const HipExecutor> exec,
               const matrix::Csr<ValueType, IndexType>* orig,
               matrix::Csr<ValueType, IndexType>* trans)
{
    if (hipsparse::is_supported<ValueType, IndexType>::value) {
        hipsparseAction_t copyValues = HIPSPARSE_ACTION_NUMERIC;
        hipsparseIndexBase_t idxBase = HIPSPARSE_INDEX_BASE_ZERO;

        hipsparse::transpose(
            exec->get_hipsparse_handle(), orig->get_size()[0],
            orig->get_size()[1], orig->get_num_stored_elements(),
            orig->get_const_values(), orig->get_const_row_ptrs(),
            orig->get_const_col_idxs(), trans->get_values(),
            trans->get_row_ptrs(), trans->get_col_idxs(), copyValues, idxBase);
    } else {
        GKO_NOT_IMPLEMENTED;
    }
}

GKO_INSTANTIATE_FOR_EACH_VALUE_AND_INDEX_TYPE(GKO_DECLARE_CSR_TRANSPOSE_KERNEL);


template <typename ValueType, typename IndexType>
void conj_transpose(std::shared_ptr<const HipExecutor> exec,
                    const matrix::Csr<ValueType, IndexType>* orig,
                    matrix::Csr<ValueType, IndexType>* trans)
{
    if (hipsparse::is_supported<ValueType, IndexType>::value) {
        const auto block_size = default_block_size;
        const auto grid_size =
            ceildiv(trans->get_num_stored_elements(), block_size);

        hipsparseAction_t copyValues = HIPSPARSE_ACTION_NUMERIC;
        hipsparseIndexBase_t idxBase = HIPSPARSE_INDEX_BASE_ZERO;

        hipsparse::transpose(
            exec->get_hipsparse_handle(), orig->get_size()[0],
            orig->get_size()[1], orig->get_num_stored_elements(),
            orig->get_const_values(), orig->get_const_row_ptrs(),
            orig->get_const_col_idxs(), trans->get_values(),
            trans->get_row_ptrs(), trans->get_col_idxs(), copyValues, idxBase);

        hipLaunchKernelGGL(conjugate_kernel, grid_size, block_size, 0, 0,
                           trans->get_num_stored_elements(),
                           as_hip_type(trans->get_values()));
    } else {
        GKO_NOT_IMPLEMENTED;
    }
}

GKO_INSTANTIATE_FOR_EACH_VALUE_AND_INDEX_TYPE(
    GKO_DECLARE_CSR_CONJ_TRANSPOSE_KERNEL);


template <typename ValueType, typename IndexType>
void inv_symm_permute(std::shared_ptr<const HipExecutor> exec,
                      const IndexType* perm,
                      const matrix::Csr<ValueType, IndexType>* orig,
                      matrix::Csr<ValueType, IndexType>* permuted)
{
    auto num_rows = orig->get_size()[0];
    auto count_num_blocks = ceildiv(num_rows, default_block_size);
    hipLaunchKernelGGL(HIP_KERNEL_NAME(inv_row_ptr_permute_kernel),
                       count_num_blocks, default_block_size, 0, 0, num_rows,
                       perm, orig->get_const_row_ptrs(),
                       permuted->get_row_ptrs());
    components::prefix_sum(exec, permuted->get_row_ptrs(), num_rows + 1);
    auto copy_num_blocks =
        ceildiv(num_rows, default_block_size / config::warp_size);
    hipLaunchKernelGGL(
        HIP_KERNEL_NAME(inv_symm_permute_kernel<config::warp_size>),
        copy_num_blocks, default_block_size, 0, 0, num_rows, perm,
        orig->get_const_row_ptrs(), orig->get_const_col_idxs(),
        as_hip_type(orig->get_const_values()), permuted->get_row_ptrs(),
        permuted->get_col_idxs(), as_hip_type(permuted->get_values()));
}

GKO_INSTANTIATE_FOR_EACH_VALUE_AND_INDEX_TYPE(
    GKO_DECLARE_CSR_INV_SYMM_PERMUTE_KERNEL);


template <typename ValueType, typename IndexType>
void row_permute(std::shared_ptr<const HipExecutor> exec, const IndexType* perm,
                 const matrix::Csr<ValueType, IndexType>* orig,
                 matrix::Csr<ValueType, IndexType>* row_permuted)
{
    auto num_rows = orig->get_size()[0];
    auto count_num_blocks = ceildiv(num_rows, default_block_size);
    hipLaunchKernelGGL(HIP_KERNEL_NAME(row_ptr_permute_kernel),
                       count_num_blocks, default_block_size, 0, 0, num_rows,
                       perm, orig->get_const_row_ptrs(),
                       row_permuted->get_row_ptrs());
    components::prefix_sum(exec, row_permuted->get_row_ptrs(), num_rows + 1);
    auto copy_num_blocks =
        ceildiv(num_rows, default_block_size / config::warp_size);
    hipLaunchKernelGGL(
        HIP_KERNEL_NAME(row_permute_kernel<config::warp_size>), copy_num_blocks,
        default_block_size, 0, 0, num_rows, perm, orig->get_const_row_ptrs(),
        orig->get_const_col_idxs(), as_hip_type(orig->get_const_values()),
        row_permuted->get_row_ptrs(), row_permuted->get_col_idxs(),
        as_hip_type(row_permuted->get_values()));
}

GKO_INSTANTIATE_FOR_EACH_VALUE_AND_INDEX_TYPE(
    GKO_DECLARE_CSR_ROW_PERMUTE_KERNEL);


template <typename ValueType, typename IndexType>
void inverse_row_permute(std::shared_ptr<const HipExecutor> exec,
                         const IndexType* perm,
                         const matrix::Csr<ValueType, IndexType>* orig,
                         matrix::Csr<ValueType, IndexType>* row_permuted)
{
    auto num_rows = orig->get_size()[0];
    auto count_num_blocks = ceildiv(num_rows, default_block_size);
    hipLaunchKernelGGL(HIP_KERNEL_NAME(inv_row_ptr_permute_kernel),
                       count_num_blocks, default_block_size, 0, 0, num_rows,
                       perm, orig->get_const_row_ptrs(),
                       row_permuted->get_row_ptrs());
    components::prefix_sum(exec, row_permuted->get_row_ptrs(), num_rows + 1);
    auto copy_num_blocks =
        ceildiv(num_rows, default_block_size / config::warp_size);
    hipLaunchKernelGGL(
        HIP_KERNEL_NAME(inv_row_permute_kernel<config::warp_size>),
        copy_num_blocks, default_block_size, 0, 0, num_rows, perm,
        orig->get_const_row_ptrs(), orig->get_const_col_idxs(),
        as_hip_type(orig->get_const_values()), row_permuted->get_row_ptrs(),
        row_permuted->get_col_idxs(), as_hip_type(row_permuted->get_values()));
}

GKO_INSTANTIATE_FOR_EACH_VALUE_AND_INDEX_TYPE(
    GKO_DECLARE_CSR_INVERSE_ROW_PERMUTE_KERNEL);


template <typename ValueType, typename IndexType>
void calculate_nonzeros_per_row_in_span(
    std::shared_ptr<const DefaultExecutor> exec,
    const matrix::Csr<ValueType, IndexType>* source, const span& row_span,
    const span& col_span, Array<IndexType>* row_nnz)
{
    const auto num_rows = source->get_size()[0];
    auto row_ptrs = source->get_const_row_ptrs();
    auto col_idxs = source->get_const_col_idxs();
    auto grid_dim = ceildiv(row_span.length(), default_block_size);

    hipLaunchKernelGGL(kernel::calculate_nnz_per_row_in_span, grid_dim,
                       default_block_size, 0, 0, row_span, col_span,
                       as_hip_type(row_ptrs), as_hip_type(col_idxs),
                       as_hip_type(row_nnz->get_data()));
}

GKO_INSTANTIATE_FOR_EACH_VALUE_AND_INDEX_TYPE(
    GKO_DECLARE_CSR_CALC_NNZ_PER_ROW_IN_SPAN_KERNEL);


template <typename ValueType, typename IndexType>
void compute_submatrix(std::shared_ptr<const DefaultExecutor> exec,
                       const matrix::Csr<ValueType, IndexType>* source,
                       gko::span row_span, gko::span col_span,
                       matrix::Csr<ValueType, IndexType>* result)
{
    auto row_offset = row_span.begin;
    auto col_offset = col_span.begin;
    auto num_rows = result->get_size()[0];
    auto num_cols = result->get_size()[1];
    auto row_ptrs = source->get_const_row_ptrs();
    auto grid_dim = ceildiv(num_rows, default_block_size);

    auto num_nnz = source->get_num_stored_elements();
    grid_dim = ceildiv(num_nnz, default_block_size);
    hipLaunchKernelGGL(
        kernel::compute_submatrix_idxs_and_vals, grid_dim, default_block_size,
        0, 0, num_rows, num_cols, num_nnz, row_offset, col_offset,
        as_hip_type(source->get_const_row_ptrs()),
        as_hip_type(source->get_const_col_idxs()),
        as_hip_type(source->get_const_values()),
        as_hip_type(result->get_const_row_ptrs()),
        as_hip_type(result->get_col_idxs()), as_hip_type(result->get_values()));
}

GKO_INSTANTIATE_FOR_EACH_VALUE_AND_INDEX_TYPE(
    GKO_DECLARE_CSR_COMPUTE_SUB_MATRIX_KERNEL);


template <typename ValueType, typename IndexType>
void sort_by_column_index(std::shared_ptr<const HipExecutor> exec,
                          matrix::Csr<ValueType, IndexType>* to_sort)
{
    if (hipsparse::is_supported<ValueType, IndexType>::value) {
        auto handle = exec->get_hipsparse_handle();
        auto descr = hipsparse::create_mat_descr();
        auto m = IndexType(to_sort->get_size()[0]);
        auto n = IndexType(to_sort->get_size()[1]);
        auto nnz = IndexType(to_sort->get_num_stored_elements());
        auto row_ptrs = to_sort->get_const_row_ptrs();
        auto col_idxs = to_sort->get_col_idxs();
        auto vals = to_sort->get_values();

        // copy values
        Array<ValueType> tmp_vals_array(exec, nnz);
        exec->copy(nnz, vals, tmp_vals_array.get_data());
        auto tmp_vals = tmp_vals_array.get_const_data();

        // init identity permutation
        Array<IndexType> permutation_array(exec, nnz);
        auto permutation = permutation_array.get_data();
        hipsparse::create_identity_permutation(handle, nnz, permutation);

        // allocate buffer
        size_type buffer_size{};
        hipsparse::csrsort_buffer_size(handle, m, n, nnz, row_ptrs, col_idxs,
                                       buffer_size);
        Array<char> buffer_array{exec, buffer_size};
        auto buffer = buffer_array.get_data();

        // sort column indices
        hipsparse::csrsort(handle, m, n, nnz, descr, row_ptrs, col_idxs,
                           permutation, buffer);

        // sort values
        hipsparse::gather(handle, nnz, tmp_vals, vals, permutation);

        hipsparse::destroy(descr);
    } else {
        GKO_NOT_IMPLEMENTED;
    }
}

GKO_INSTANTIATE_FOR_EACH_VALUE_AND_INDEX_TYPE(
    GKO_DECLARE_CSR_SORT_BY_COLUMN_INDEX);


template <typename ValueType, typename IndexType>
void is_sorted_by_column_index(
    std::shared_ptr<const HipExecutor> exec,
    const matrix::Csr<ValueType, IndexType>* to_check, bool* is_sorted)
{
    *is_sorted = true;
    auto cpu_array = Array<bool>::view(exec->get_master(), 1, is_sorted);
    auto gpu_array = Array<bool>{exec, cpu_array};
    auto block_size = default_block_size;
    auto num_rows = static_cast<IndexType>(to_check->get_size()[0]);
    auto num_blocks = ceildiv(num_rows, block_size);
    hipLaunchKernelGGL(HIP_KERNEL_NAME(kernel::check_unsorted), num_blocks,
                       block_size, 0, 0, to_check->get_const_row_ptrs(),
                       to_check->get_const_col_idxs(), num_rows,
                       gpu_array.get_data());
    cpu_array = gpu_array;
}

GKO_INSTANTIATE_FOR_EACH_VALUE_AND_INDEX_TYPE(
    GKO_DECLARE_CSR_IS_SORTED_BY_COLUMN_INDEX);


template <typename ValueType, typename IndexType>
void extract_diagonal(std::shared_ptr<const HipExecutor> exec,
                      const matrix::Csr<ValueType, IndexType>* orig,
                      matrix::Diagonal<ValueType>* diag)
{
    const auto nnz = orig->get_num_stored_elements();
    const auto diag_size = diag->get_size()[0];
    const auto num_blocks =
        ceildiv(config::warp_size * diag_size, default_block_size);

    const auto orig_values = orig->get_const_values();
    const auto orig_row_ptrs = orig->get_const_row_ptrs();
    const auto orig_col_idxs = orig->get_const_col_idxs();
    auto diag_values = diag->get_values();

    hipLaunchKernelGGL(HIP_KERNEL_NAME(kernel::extract_diagonal), num_blocks,
                       default_block_size, 0, 0, diag_size, nnz,
                       as_hip_type(orig_values), as_hip_type(orig_row_ptrs),
                       as_hip_type(orig_col_idxs), as_hip_type(diag_values));
}

GKO_INSTANTIATE_FOR_EACH_VALUE_AND_INDEX_TYPE(GKO_DECLARE_CSR_EXTRACT_DIAGONAL);


}  // namespace csr
}  // namespace hip
}  // namespace kernels
}  // namespace gko
