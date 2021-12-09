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

#include "core/matrix/sellp_kernels.hpp"


#include <hip/hip_runtime.h>


#include <ginkgo/core/base/exception_helpers.hpp>
#include <ginkgo/core/base/math.hpp>
#include <ginkgo/core/base/types.hpp>
#include <ginkgo/core/matrix/csr.hpp>
#include <ginkgo/core/matrix/dense.hpp>


#include "core/components/prefix_sum_kernels.hpp"
#include "hip/base/config.hip.hpp"
#include "hip/base/hipsparse_bindings.hip.hpp"
#include "hip/base/types.hip.hpp"
#include "hip/components/reduction.hip.hpp"
#include "hip/components/thread_ids.hip.hpp"


namespace gko {
namespace kernels {
namespace hip {
/**
 * @brief The SELL-P matrix format namespace.
 *
 * @ingroup sellp
 */
namespace sellp {


constexpr int default_block_size = 512;


#include "common/cuda_hip/matrix/sellp_kernels.hpp.inc"


template <typename ValueType, typename IndexType>
void spmv(std::shared_ptr<const HipExecutor> exec,
          const matrix::Sellp<ValueType, IndexType>* a,
          const matrix::Dense<ValueType>* b, matrix::Dense<ValueType>* c)
{
    const auto blockSize = default_block_size;
    const dim3 gridSize(ceildiv(a->get_size()[0], default_block_size),
                        b->get_size()[1]);

    hipLaunchKernelGGL(
        spmv_kernel, gridSize, blockSize, 0, 0, a->get_size()[0],
        b->get_size()[1], b->get_stride(), c->get_stride(), a->get_slice_size(),
        a->get_const_slice_sets(), as_hip_type(a->get_const_values()),
        a->get_const_col_idxs(), as_hip_type(b->get_const_values()),
        as_hip_type(c->get_values()));
}

GKO_INSTANTIATE_FOR_EACH_VALUE_AND_INDEX_TYPE(GKO_DECLARE_SELLP_SPMV_KERNEL);


template <typename ValueType, typename IndexType>
void advanced_spmv(std::shared_ptr<const HipExecutor> exec,
                   const matrix::Dense<ValueType>* alpha,
                   const matrix::Sellp<ValueType, IndexType>* a,
                   const matrix::Dense<ValueType>* b,
                   const matrix::Dense<ValueType>* beta,
                   matrix::Dense<ValueType>* c)
{
    const auto blockSize = default_block_size;
    const dim3 gridSize(ceildiv(a->get_size()[0], default_block_size),
                        b->get_size()[1]);

    hipLaunchKernelGGL(
        advanced_spmv_kernel, gridSize, blockSize, 0, 0, a->get_size()[0],
        b->get_size()[1], b->get_stride(), c->get_stride(), a->get_slice_size(),
        a->get_const_slice_sets(), as_hip_type(alpha->get_const_values()),
        as_hip_type(a->get_const_values()), a->get_const_col_idxs(),
        as_hip_type(b->get_const_values()),
        as_hip_type(beta->get_const_values()), as_hip_type(c->get_values()));
}

GKO_INSTANTIATE_FOR_EACH_VALUE_AND_INDEX_TYPE(
    GKO_DECLARE_SELLP_ADVANCED_SPMV_KERNEL);


}  // namespace sellp
}  // namespace hip
}  // namespace kernels
}  // namespace gko
