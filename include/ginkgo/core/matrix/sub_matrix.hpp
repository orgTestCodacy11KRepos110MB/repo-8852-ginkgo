/*******************************<GINKGO LICENSE>******************************
Copyright (c) 2017-2021, the Ginkgo authors
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

#ifndef GKO_PUBLIC_CORE_MATRIX_SUBMATRIX_HPP_
#define GKO_PUBLIC_CORE_MATRIX_SUBMATRIX_HPP_


#include <vector>


#include <ginkgo/core/base/array.hpp>
#include <ginkgo/core/base/lin_op.hpp>


namespace gko {
namespace matrix {


template <class MatrixType>
class SubMatrix : public EnableLinOp<SubMatrix<MatrixType>>,
                  public EnableCreateMethod<SubMatrix<MatrixType>> {
    friend class EnableCreateMethod<SubMatrix>;
    friend class EnablePolymorphicObject<SubMatrix, LinOp>;

public:
    using value_type = typename MatrixType::value_type;
    using index_type = typename MatrixType::index_type;

    std::shared_ptr<MatrixType> get_submatrix() const { return sub_mtx_; }

    std::vector<std::shared_ptr<MatrixType>> get_overlap_mtxs() const
    {
        return overlap_mtxs_;
    }

private:
    inline dim<2> compute_size(const MatrixType *matrix,
                               const gko::span &row_span,
                               const gko::span &col_span,
                               const std::vector<gko::span> &overlap_row_span,
                               const std::vector<gko::span> &overlap_col_span)
    {
        auto mat_size = matrix->get_size();
        size_type num_ov_rows = 0;
        size_type num_ov_cols = 0;
        if (overlap_row_span.size() > 0) {
            for (const auto &i : overlap_row_span) {
                num_ov_rows += i.length();
            }
        }
        if (overlap_col_span.size() > 0) {
            for (const auto &i : overlap_col_span) {
                num_ov_cols += i.length();
            }
        }
        auto upd_size = gko::dim<2>(row_span.length() + num_ov_rows,
                                    col_span.length() + num_ov_cols);
        return upd_size;
    }

protected:
    SubMatrix(std::shared_ptr<const Executor> exec)
        : EnableLinOp<SubMatrix<MatrixType>>{exec, dim<2>{}},
          sub_mtx_{MatrixType::create(exec)},
          overlap_mtxs_{}
    {}

    SubMatrix(std::shared_ptr<const Executor> exec, const MatrixType *matrix,
              const gko::span &row_span, const gko::span &col_span,
              const std::vector<gko::span> &overlap_row_span = {},
              const std::vector<gko::span> &overlap_col_span = {})
        : EnableLinOp<SubMatrix<MatrixType>>{exec,
                                             compute_size(matrix, row_span,
                                                          col_span,
                                                          overlap_row_span,
                                                          overlap_col_span)},
          sub_mtx_{MatrixType::create(exec)},
          overlap_mtxs_{}
    {
        GKO_ASSERT(overlap_row_span.size() == overlap_col_span.size());
        this->generate(matrix, row_span, col_span, overlap_row_span,
                       overlap_col_span);
    }

    void apply_impl(const LinOp *b, LinOp *x) const override;

    void apply_impl(const LinOp *alpha, const LinOp *b, const LinOp *beta,
                    LinOp *x) const override;

    void generate(const MatrixType *matrix, const gko::span &row_span,
                  const gko::span &col_span,
                  const std::vector<gko::span> &overlap_row_span,
                  const std::vector<gko::span> &overlap_col_span);

private:
    std::shared_ptr<MatrixType> sub_mtx_;
    std::vector<std::shared_ptr<MatrixType>> overlap_mtxs_;
};


}  // namespace matrix
}  // namespace gko


#endif
