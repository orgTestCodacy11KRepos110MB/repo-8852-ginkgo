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

#include <ginkgo/core/matrix/block_matrix.hpp>
#include <ginkgo/core/matrix/dense.hpp>

namespace gko {
namespace matrix {


void BlockMatrix::apply_impl(const LinOp* b, LinOp* x) const
{
    auto block_b = dynamic_cast<const BlockMatrix*>(b);
    auto block_x = dynamic_cast<BlockMatrix*>(x);

    auto one = gko::initialize<Dense<double>>({1}, this->get_executor());
    auto zero = gko::initialize<Dense<double>>({0}, this->get_executor());

    for (size_t block_row = 0; block_row < block_size_[0]; ++block_row) {
        for (size_t block_col = 0; block_col < block_size_[1]; ++block_col) {
            if (block_col == 0) {
                blocks_[block_row][block_col]->apply(
                    block_b->blocks_[block_col][0].get(),
                    block_x->blocks_[block_row][0].get());
            } else {
                blocks_[block_row][block_col]->apply(
                    one.get(), block_b->blocks_[block_col][0].get(), one.get(),
                    block_x->blocks_[block_row][0].get());
            }
        }
    }
}
void BlockMatrix::apply_impl(const LinOp* alpha, const LinOp* b,
                             const LinOp* beta, LinOp* x) const
{
    auto block_b = dynamic_cast<const BlockMatrix*>(b);
    auto block_x = dynamic_cast<BlockMatrix*>(x);

    auto one = gko::initialize<Dense<double>>({1}, this->get_executor());
    auto zero = gko::initialize<Dense<double>>({0}, this->get_executor());

    for (size_t block_row = 0; block_row < block_size_[0]; ++block_row) {
        for (size_t block_col = 0; block_col < block_size_[1]; ++block_col) {
            if (block_col == 0) {
                blocks_[block_row][block_col]->apply(
                    alpha, block_b->blocks_[block_col][0].get(), beta,
                    block_x->blocks_[block_row][0].get());
            } else {
                blocks_[block_row][block_col]->apply(
                    alpha, block_b->blocks_[block_col][0].get(), one.get(),
                    block_x->blocks_[block_row][0].get());
            }
        }
    }
}

}  // namespace matrix
}  // namespace gko
