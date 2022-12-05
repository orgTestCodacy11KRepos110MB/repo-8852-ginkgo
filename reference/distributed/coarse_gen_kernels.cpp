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

#include "core/distributed/coarse_gen_kernels.hpp"


#include <iterator>
#include <memory>
#include <tuple>


#include <ginkgo/core/base/exception_helpers.hpp>
#include <ginkgo/core/base/math.hpp>
#include <ginkgo/core/base/types.hpp>
#include <ginkgo/core/distributed/partition.hpp>
#include <ginkgo/core/matrix/csr.hpp>
#include <ginkgo/core/matrix/dense.hpp>
#include <ginkgo/core/matrix/diagonal.hpp>


#include "core/base/allocator.hpp"
#include "core/components/prefix_sum_kernels.hpp"
#include "core/matrix/csr_builder.hpp"


namespace gko {
namespace kernels {
namespace reference {
/**
 * @brief The COARSE_GEN solver namespace.
 *
 * @ingroup coarse_gen
 */
namespace coarse_gen {


template <typename ValueType, typename IndexType>
void find_strongest_neighbor(
    std::shared_ptr<const ReferenceExecutor> exec,
    const matrix::Csr<ValueType, IndexType>* weight_mtx_diag,
    const matrix::Csr<ValueType, IndexType>* weight_mtx_offdiag,
    const matrix::Diagonal<ValueType>* diag, array<IndexType>& agg,
    array<IndexType>& strongest_neighbor)
{
    const auto wdiag_row_ptrs = weight_mtx_diag->get_const_row_ptrs();
    const auto wdiag_col_idxs = weight_mtx_diag->get_const_col_idxs();
    const auto wdiag_vals = weight_mtx_diag->get_const_values();
    const auto diag_vals = diag->get_const_values();
    for (size_type row = 0; row < agg.get_num_elems(); row++) {
        auto max_weight_unagg = zero<ValueType>();
        auto max_weight_agg = zero<ValueType>();
        IndexType strongest_unagg = -1;
        IndexType strongest_agg = -1;
        if (agg.get_const_data()[row] == -1) {
            for (auto idx = wdiag_row_ptrs[row]; idx < wdiag_row_ptrs[row + 1];
                 idx++) {
                auto col = wdiag_col_idxs[idx];
                if (col == row) {
                    continue;
                }
                auto weight = wdiag_vals[idx] /
                              max(abs(diag_vals[row]), abs(diag_vals[col]));
                if (agg.get_const_data()[col] == -1 &&
                    std::tie(weight, col) >
                        std::tie(max_weight_unagg, strongest_unagg)) {
                    max_weight_unagg = weight;
                    strongest_unagg = col;
                } else if (agg.get_const_data()[col] != -1 &&
                           std::tie(weight, col) >
                               std::tie(max_weight_agg, strongest_agg)) {
                    max_weight_agg = weight;
                    strongest_agg = col;
                }
            }

            if (strongest_unagg == -1 && strongest_agg != -1) {
                // all neighbor is agg, connect to the strongest agg
                agg.get_data()[row] = agg.get_data()[strongest_agg];
            } else if (strongest_unagg != -1) {
                // set the strongest neighbor in the unagg group
                strongest_neighbor.get_data()[row] = strongest_unagg;
            } else {
                // no neighbor
                strongest_neighbor.get_data()[row] = row;
            }
        }
    }
}

GKO_INSTANTIATE_FOR_EACH_NON_COMPLEX_VALUE_AND_INDEX_TYPE(
    GKO_DECLARE_COARSE_GEN_FIND_STRONGEST_NEIGHBOR);


template <typename ValueType, typename IndexType>
void assign_to_exist_agg(
    std::shared_ptr<const DefaultExecutor> exec,
    const matrix::Csr<ValueType, IndexType>* weight_mtx_diag,
    const matrix::Csr<ValueType, IndexType>* weight_mtx_offdiag,
    const matrix::Diagonal<ValueType>* diag, array<IndexType>& agg,
    array<IndexType>& intermediate_agg)
{
    const auto row_ptrs = weight_mtx_diag->get_const_row_ptrs();
    const auto col_idxs = weight_mtx_diag->get_const_col_idxs();
    const auto vals = weight_mtx_diag->get_const_values();
    const auto agg_const_val = agg.get_const_data();
    auto agg_val = (intermediate_agg.get_num_elems() > 0)
                       ? intermediate_agg.get_data()
                       : agg.get_data();
    const auto diag_vals = diag->get_const_values();
    for (IndexType row = 0; row < agg.get_num_elems(); row++) {
        if (agg_const_val[row] != -1) {
            continue;
        }
        auto max_weight_agg = zero<ValueType>();
        IndexType strongest_agg = -1;
        for (auto idx = row_ptrs[row]; idx < row_ptrs[row + 1]; idx++) {
            auto col = col_idxs[idx];
            if (col == row) {
                continue;
            }
            auto weight =
                vals[idx] / max(abs(diag_vals[row]), abs(diag_vals[col]));
            if (agg_const_val[col] != -1 &&
                std::tie(weight, col) >
                    std::tie(max_weight_agg, strongest_agg)) {
                max_weight_agg = weight;
                strongest_agg = col;
            }
        }
        if (strongest_agg != -1) {
            agg_val[row] = agg_const_val[strongest_agg];
        } else {
            agg_val[row] = row;
        }
    }

    if (intermediate_agg.get_num_elems() > 0) {
        // Copy the intermediate_agg to agg
        agg = intermediate_agg;
    }
}

GKO_INSTANTIATE_FOR_EACH_NON_COMPLEX_VALUE_AND_INDEX_TYPE(
    GKO_DECLARE_COARSE_GEN_ASSIGN_TO_EXIST_AGG);


template <typename ValueType, typename IndexType>
void fill_coarse(
    std::shared_ptr<const DefaultExecutor> exec,
    const device_matrix_data<ValueType, IndexType>& fine_matrix_data,
    const array<IndexType>& fine_row_ptrs,
    device_matrix_data<ValueType, IndexType>& coarse_data,
    device_matrix_data<ValueType, IndexType>& restrict_data,
    device_matrix_data<ValueType, IndexType>& prolong_data,
    array<IndexType>& coarse_indices)
{
    const auto global_size = fine_matrix_data.get_size();
    const auto global_nnz = fine_matrix_data.get_num_elems();
    const auto coarse_size = coarse_data.get_size();
    const auto f_row_idxs = fine_matrix_data.get_const_row_idxs();
    const auto f_col_idxs = fine_matrix_data.get_const_col_idxs();
    auto c_matrix_data =
        matrix_assembly_data<ValueType, IndexType>(coarse_size);
    auto r_matrix_data = matrix_assembly_data<ValueType, IndexType>(
        dim<2>(coarse_size[0], global_size[0]));
    auto p_matrix_data = matrix_assembly_data<ValueType, IndexType>(
        dim<2>(global_size[0], coarse_size[0]));

    // Get coarse data with global fine matrix indexing.
    int nnz = 0;
    int ridx = 0;
    for (auto i = 0; i < coarse_size[0]; ++i) {
        if (std::find(f_row_idxs, f_row_idxs + global_nnz,
                      coarse_indices.get_data()[i]) !=
            f_row_idxs + global_nnz) {
            int cidx = 0;
            for (auto j = 0; j < coarse_size[0]; ++j) {
                if (std::find(f_col_idxs, f_col_idxs + global_nnz,
                              coarse_indices.get_data()[j]) !=
                    f_col_idxs + global_nnz) {
                    // Assume row major ordering
                    c_matrix_data.add_value(
                        ridx, cidx,
                        fine_matrix_data.get_const_values()
                            [fine_row_ptrs.get_const_data()
                                 [coarse_indices.get_data()[i]] +
                             coarse_indices.get_data()[j]]);
                    // c_matrix_data.get_row_idxs()[nnz] = ridx;
                    // coarse_data.get_col_idxs()[nnz] = cidx;
                    // coarse_data.get_values()[nnz] =
                    nnz++;
                }
                cidx++;
            }
            ridx++;
        }
    }
    coarse_data = device_matrix_data<ValueType, IndexType>::create_from_host(
        exec, c_matrix_data.get_ordered_data());

    nnz = 0;
    for (auto i = 0; i < coarse_size[0]; ++i) {
        if (std::find(f_row_idxs, f_row_idxs + global_nnz,
                      coarse_indices.get_data()[i]) !=
            f_row_idxs + global_nnz) {
            // Assume row major ordering
            r_matrix_data.add_value(i, coarse_indices.get_data()[i],
                                    one<ValueType>());
            // restrict_data.get_row_idxs()[nnz] = i;
            // restrict_data.get_col_idxs()[nnz] = coarse_indices.get_data()[i];
            // restrict_data.get_values()[nnz] = one<ValueType>();
            nnz++;
        }
    }
    restrict_data = device_matrix_data<ValueType, IndexType>::create_from_host(
        exec, r_matrix_data.get_ordered_data());

    nnz = 0;
    for (auto i = 0; i < coarse_size[0]; ++i) {
        if (std::find(f_col_idxs + global_nnz, f_col_idxs + global_nnz,
                      coarse_indices.get_data()[i]) !=
            f_col_idxs + global_nnz) {
            // Assume row major ordering
            p_matrix_data.add_value(coarse_indices.get_data()[i], i,
                                    one<ValueType>());
            // prolong_data.get_row_idxs()[nnz] = coarse_indices.get_data()[i];
            // prolong_data.get_col_idxs()[nnz] = i;
            // prolong_data.get_values()[nnz] = one<ValueType>();
            nnz++;
        }
    }
    prolong_data = device_matrix_data<ValueType, IndexType>::create_from_host(
        exec, p_matrix_data.get_ordered_data());
}

GKO_INSTANTIATE_FOR_EACH_VALUE_AND_INDEX_TYPE(
    GKO_DECLARE_COARSE_GEN_FILL_COARSE);


}  // namespace coarse_gen
}  // namespace reference
}  // namespace kernels
}  // namespace gko