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

#include "core/matrix/ell_kernels.hpp"


#include <ginkgo/core/base/exception_helpers.hpp>
#include <ginkgo/core/base/math.hpp>
#include <ginkgo/core/matrix/csr.hpp>
#include <ginkgo/core/matrix/dense.hpp>


#include "accessor/reduced_row_major.hpp"
#include "core/base/mixed_precision_types.hpp"


namespace gko {
namespace kernels {
namespace reference {
/**
 * @brief The ELL matrix format namespace.
 * @ref Ell
 * @ingroup ell
 */
namespace ell {


template <typename InputValueType, typename MatrixValueType,
          typename OutputValueType, typename IndexType>
void spmv(std::shared_ptr<const ReferenceExecutor> exec,
          const matrix::Ell<MatrixValueType, IndexType>* a,
          const matrix::Dense<InputValueType>* b,
          matrix::Dense<OutputValueType>* c)
{
    using arithmetic_type =
        highest_precision<InputValueType, OutputValueType, MatrixValueType>;
    using a_accessor =
        gko::acc::reduced_row_major<1, arithmetic_type, const MatrixValueType>;
    using b_accessor =
        gko::acc::reduced_row_major<2, arithmetic_type, const InputValueType>;

    const auto num_stored_elements_per_row =
        a->get_num_stored_elements_per_row();
    const auto stride = a->get_stride();
    const auto a_vals = gko::acc::range<a_accessor>(
        std::array<acc::size_type, 1>{
            static_cast<acc::size_type>(num_stored_elements_per_row * stride)},
        a->get_const_values());
    const auto b_vals = gko::acc::range<b_accessor>(
        std::array<acc::size_type, 2>{
            {static_cast<acc::size_type>(b->get_size()[0]),
             static_cast<acc::size_type>(b->get_size()[1])}},
        b->get_const_values(),
        std::array<acc::size_type, 1>{
            {static_cast<acc::size_type>(b->get_stride())}});

    for (size_type j = 0; j < c->get_size()[1]; j++) {
        for (size_type row = 0; row < a->get_size()[0]; row++) {
            arithmetic_type result{};
            for (size_type i = 0; i < num_stored_elements_per_row; i++) {
                auto val = a_vals(row + i * stride);
                auto col = a->col_at(row, i);
                result += val * b_vals(col, j);
            }
            c->at(row, j) = result;
        }
    }
}

GKO_INSTANTIATE_FOR_EACH_MIXED_VALUE_AND_INDEX_TYPE(
    GKO_DECLARE_ELL_SPMV_KERNEL);


template <typename InputValueType, typename MatrixValueType,
          typename OutputValueType, typename IndexType>
void advanced_spmv(std::shared_ptr<const ReferenceExecutor> exec,
                   const matrix::Dense<MatrixValueType>* alpha,
                   const matrix::Ell<MatrixValueType, IndexType>* a,
                   const matrix::Dense<InputValueType>* b,
                   const matrix::Dense<OutputValueType>* beta,
                   matrix::Dense<OutputValueType>* c)
{
    using arithmetic_type =
        highest_precision<InputValueType, OutputValueType, MatrixValueType>;
    using a_accessor =
        gko::acc::reduced_row_major<1, arithmetic_type, const MatrixValueType>;
    using b_accessor =
        gko::acc::reduced_row_major<2, arithmetic_type, const InputValueType>;

    const auto num_stored_elements_per_row =
        a->get_num_stored_elements_per_row();
    const auto stride = a->get_stride();
    const auto a_vals = gko::acc::range<a_accessor>(
        std::array<acc::size_type, 1>{
            static_cast<acc::size_type>(num_stored_elements_per_row * stride)},
        a->get_const_values());
    const auto b_vals = gko::acc::range<b_accessor>(
        std::array<acc::size_type, 2>{
            {static_cast<acc::size_type>(b->get_size()[0]),
             static_cast<acc::size_type>(b->get_size()[1])}},
        b->get_const_values(),
        std::array<acc::size_type, 1>{
            {static_cast<acc::size_type>(b->get_stride())}});
    const auto alpha_val = arithmetic_type{alpha->at(0, 0)};
    const auto beta_val = arithmetic_type{beta->at(0, 0)};

    for (size_type j = 0; j < c->get_size()[1]; j++) {
        for (size_type row = 0; row < a->get_size()[0]; row++) {
            arithmetic_type result = c->at(row, j);
            result *= beta_val;
            for (size_type i = 0; i < num_stored_elements_per_row; i++) {
                auto val = a_vals(row + i * stride);
                auto col = a->col_at(row, i);
                result += alpha_val * val * b_vals(col, j);
            }
            c->at(row, j) = result;
        }
    }
}

GKO_INSTANTIATE_FOR_EACH_MIXED_VALUE_AND_INDEX_TYPE(
    GKO_DECLARE_ELL_ADVANCED_SPMV_KERNEL);


template <typename IndexType>
void compute_max_row_nnz(std::shared_ptr<const DefaultExecutor> exec,
                         const Array<IndexType>& row_ptrs, size_type& max_nnz)
{
    max_nnz = 0;
    const auto ptrs = row_ptrs.get_const_data();
    for (size_type i = 1; i < row_ptrs.get_num_elems(); i++) {
        max_nnz = std::max<size_type>(max_nnz, ptrs[i] - ptrs[i - 1]);
    }
}

GKO_INSTANTIATE_FOR_EACH_INDEX_TYPE(GKO_DECLARE_ELL_COMPUTE_MAX_ROW_NNZ_KERNEL);


template <typename ValueType, typename IndexType>
void fill_in_matrix_data(
    std::shared_ptr<const DefaultExecutor> exec,
    const Array<matrix_data_entry<ValueType, IndexType>>& nonzeros,
    const int64* row_ptrs, matrix::Ell<ValueType, IndexType>* output)
{
    for (size_type row = 0; row < output->get_size()[0]; row++) {
        const auto row_begin = row_ptrs[row];
        const auto row_end = row_ptrs[row + 1];
        size_type col_idx = 0;
        for (auto i = row_begin; i < row_end; i++) {
            const auto entry = nonzeros.get_const_data()[i];
            output->col_at(row, col_idx) = entry.column;
            output->val_at(row, col_idx) = entry.value;
            col_idx++;
        }
        for (; col_idx < output->get_num_stored_elements_per_row(); col_idx++) {
            output->col_at(row, col_idx) = 0;
            output->val_at(row, col_idx) = zero<ValueType>();
        }
    }
}

GKO_INSTANTIATE_FOR_EACH_VALUE_AND_INDEX_TYPE(
    GKO_DECLARE_ELL_FILL_IN_MATRIX_DATA_KERNEL);


template <typename ValueType, typename IndexType>
void fill_in_dense(std::shared_ptr<const ReferenceExecutor> exec,
                   const matrix::Ell<ValueType, IndexType>* source,
                   matrix::Dense<ValueType>* result)
{
    auto num_rows = source->get_size()[0];
    auto num_cols = source->get_size()[1];
    auto num_stored_elements_per_row =
        source->get_num_stored_elements_per_row();

    for (size_type row = 0; row < num_rows; row++) {
        for (size_type i = 0; i < num_stored_elements_per_row; i++) {
            result->at(row, source->col_at(row, i)) += source->val_at(row, i);
        }
    }
}

GKO_INSTANTIATE_FOR_EACH_VALUE_AND_INDEX_TYPE(
    GKO_DECLARE_ELL_FILL_IN_DENSE_KERNEL);


template <typename ValueType, typename IndexType>
void convert_to_csr(std::shared_ptr<const ReferenceExecutor> exec,
                    const matrix::Ell<ValueType, IndexType>* source,
                    matrix::Csr<ValueType, IndexType>* result)
{
    const auto num_rows = source->get_size()[0];
    const auto max_nnz_per_row = source->get_num_stored_elements_per_row();

    auto row_ptrs = result->get_row_ptrs();
    auto col_idxs = result->get_col_idxs();
    auto values = result->get_values();

    size_type cur_ptr = 0;
    row_ptrs[0] = 0;
    for (size_type row = 0; row < num_rows; row++) {
        for (size_type i = 0; i < max_nnz_per_row; i++) {
            const auto val = source->val_at(row, i);
            const auto col = source->col_at(row, i);
            if (val != zero<ValueType>()) {
                values[cur_ptr] = val;
                col_idxs[cur_ptr] = col;
                cur_ptr++;
            }
        }
        row_ptrs[row + 1] = cur_ptr;
    }
}

GKO_INSTANTIATE_FOR_EACH_VALUE_AND_INDEX_TYPE(
    GKO_DECLARE_ELL_CONVERT_TO_CSR_KERNEL);


template <typename ValueType, typename IndexType>
void count_nonzeros(std::shared_ptr<const ReferenceExecutor> exec,
                    const matrix::Ell<ValueType, IndexType>* source,
                    size_type* result)
{
    size_type nonzeros = 0;
    const auto num_rows = source->get_size()[0];
    const auto max_nnz_per_row = source->get_num_stored_elements_per_row();
    const auto stride = source->get_stride();

    for (size_type row = 0; row < num_rows; row++) {
        for (size_type i = 0; i < max_nnz_per_row; i++) {
            nonzeros += (source->val_at(row, i) != zero<ValueType>());
        }
    }

    *result = nonzeros;
}

GKO_INSTANTIATE_FOR_EACH_VALUE_AND_INDEX_TYPE(
    GKO_DECLARE_ELL_COUNT_NONZEROS_KERNEL);


template <typename ValueType, typename IndexType>
void calculate_nonzeros_per_row(std::shared_ptr<const ReferenceExecutor> exec,
                                const matrix::Ell<ValueType, IndexType>* source,
                                Array<size_type>* result)
{
    const auto num_rows = source->get_size()[0];
    const auto max_nnz_per_row = source->get_num_stored_elements_per_row();
    const auto stride = source->get_stride();

    auto row_nnz_val = result->get_data();

    for (size_type row = 0; row < num_rows; row++) {
        size_type nonzeros_in_this_row = 0;
        for (size_type i = 0; i < max_nnz_per_row; i++) {
            nonzeros_in_this_row +=
                (source->val_at(row, i) != zero<ValueType>());
        }
        row_nnz_val[row] = nonzeros_in_this_row;
    }
}

GKO_INSTANTIATE_FOR_EACH_VALUE_AND_INDEX_TYPE(
    GKO_DECLARE_ELL_CALCULATE_NONZEROS_PER_ROW_KERNEL);


template <typename ValueType, typename IndexType>
void extract_diagonal(std::shared_ptr<const ReferenceExecutor> exec,
                      const matrix::Ell<ValueType, IndexType>* orig,
                      matrix::Diagonal<ValueType>* diag)
{
    const auto col_idxs = orig->get_const_col_idxs();
    const auto values = orig->get_const_values();
    const auto diag_size = diag->get_size()[0];
    const auto max_nnz_per_row = orig->get_num_stored_elements_per_row();
    auto diag_values = diag->get_values();

    for (size_type row = 0; row < diag_size; row++) {
        for (size_type i = 0; i < max_nnz_per_row; i++) {
            if (orig->col_at(row, i) == row) {
                diag_values[row] = orig->val_at(row, i);
                break;
            }
        }
    }
}

GKO_INSTANTIATE_FOR_EACH_VALUE_AND_INDEX_TYPE(
    GKO_DECLARE_ELL_EXTRACT_DIAGONAL_KERNEL);


}  // namespace ell
}  // namespace reference
}  // namespace kernels
}  // namespace gko
