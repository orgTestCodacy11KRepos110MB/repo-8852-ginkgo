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

#include "core/solver/batch_bicgstab_kernels.hpp"


#include <hip/hip_runtime.h>


#include <ginkgo/core/base/math.hpp>


#include "hip/base/config.hip.hpp"
#include "hip/base/exception.hip.hpp"
#include "hip/base/math.hip.hpp"
#include "hip/base/types.hip.hpp"
#include "hip/components/cooperative_groups.hip.hpp"
#include "hip/matrix/batch_struct.hip.hpp"


namespace gko {
namespace kernels {
namespace hip {


constexpr int default_block_size = 256;
constexpr int sm_multiplier = 4;

/**
 * @brief The batch Bicgstab solver namespace.
 *
 * @ingroup batch_bicgstab
 */
namespace batch_bicgstab {

#include "common/cuda_hip/components/uninitialized_array.hpp.inc"
// include all depedencies (note: do not remove this comment)
#include "common/cuda_hip/log/batch_logger.hpp.inc"
#include "common/cuda_hip/matrix/batch_csr_kernels.hpp.inc"
#include "common/cuda_hip/matrix/batch_vector_kernels.hpp.inc"
#include "common/cuda_hip/preconditioner/batch_identity.hpp.inc"
#include "common/cuda_hip/preconditioner/batch_jacobi.hpp.inc"
#include "common/cuda_hip/solver/batch_bicgstab_kernels.hpp.inc"
#include "common/cuda_hip/stop/batch_criteria.hpp.inc"


template <typename T>
using BatchBicgstabOptions =
    gko::kernels::batch_bicgstab::BatchBicgstabOptions<T>;

#define BATCH_BICGSTAB_KERNEL_LAUNCH(_stoppertype, _prectype)                  \
    hipLaunchKernelGGL(                                                        \
        HIP_KERNEL_NAME(apply_kernel<stop::_stoppertype<ValueType>>),          \
        dim3(nbatch), dim3(default_block_size), shared_size, 0,                \
        opts.num_sh_vecs, shared_gap, opts.max_its, opts.residual_tol, logger, \
        _prectype<ValueType>(), a, b.values, x.values, workspace.get_data())

template <typename BatchMatrixType, typename LogType, typename ValueType>
static void apply_impl(
    std::shared_ptr<const HipExecutor> exec,
    const BatchBicgstabOptions<remove_complex<ValueType>> opts, LogType logger,
    const BatchMatrixType& a,
    const gko::batch_dense::UniformBatch<const ValueType>& b,
    const gko::batch_dense::UniformBatch<ValueType>& x)
{
    using real_type = gko::remove_complex<ValueType>;
    const size_type nbatch = a.num_batch;
    const int shared_gap = ((a.num_rows - 1) / 32 + 1) * 32;
    static_assert(default_block_size >= 2 * config::warp_size,
                  "Need at least two warps!");
    int shared_size = opts.num_sh_vecs * shared_gap * sizeof(ValueType);

    int aux_size =
        gko::kernels::batch_bicgstab::local_memory_requirement<ValueType>(
            shared_gap, b.num_rhs);
    auto workspace = gko::Array<ValueType>(exec);

    if (opts.preconditioner == gko::preconditioner::batch::type::none) {
        aux_size +=
            BatchIdentity<ValueType>::dynamic_work_size(a.num_rows, a.num_nnz) *
            sizeof(ValueType);

        if (opts.num_sh_vecs > 0) {
            workspace = gko::Array<ValueType>(
                exec, static_cast<size_type>(std::abs(aux_size - shared_size) *
                                             nbatch / sizeof(ValueType)));
        }
        if (opts.tol_type == gko::stop::batch::ToleranceType::absolute) {
            BATCH_BICGSTAB_KERNEL_LAUNCH(SimpleAbsResidual, BatchIdentity);
        } else {
            BATCH_BICGSTAB_KERNEL_LAUNCH(SimpleRelResidual, BatchIdentity);
        }
    } else if (opts.preconditioner ==
               gko::preconditioner::batch::type::jacobi) {
        aux_size +=
            BatchJacobi<ValueType>::dynamic_work_size(shared_gap, a.num_nnz) *
            sizeof(ValueType);
        if (opts.num_sh_vecs > 0) {
            workspace = gko::Array<ValueType>(
                exec, static_cast<size_type>(std::abs(aux_size - shared_size) *
                                             nbatch / sizeof(ValueType)));
        }
        if (opts.tol_type == gko::stop::batch::ToleranceType::absolute) {
            BATCH_BICGSTAB_KERNEL_LAUNCH(SimpleAbsResidual, BatchJacobi);
        } else {
            BATCH_BICGSTAB_KERNEL_LAUNCH(SimpleRelResidual, BatchJacobi);
        }
    } else {
        GKO_NOT_IMPLEMENTED;
    }
    GKO_HIP_LAST_IF_ERROR_THROW;
}


template <typename ValueType>
void apply(std::shared_ptr<const HipExecutor> exec,
           const BatchBicgstabOptions<remove_complex<ValueType>>& opts,
           const BatchLinOp* const a,
           const matrix::BatchDense<ValueType>* const b,
           matrix::BatchDense<ValueType>* const x,
           log::BatchLogData<ValueType>& logdata)
{
    using hip_value_type = hip_type<ValueType>;

    batch_log::SimpleFinalLogger<remove_complex<ValueType>> logger(
        logdata.res_norms->get_values(), logdata.iter_counts.get_data());

    const gko::batch_dense::UniformBatch<hip_value_type> x_b =
        get_batch_struct(x);

    if (auto amat = dynamic_cast<const matrix::BatchCsr<ValueType>*>(a)) {
        auto m_b = get_batch_struct(amat);
        auto b_b = get_batch_struct(b);
        apply_impl(exec, opts, logger, m_b, b_b, x_b);
    } else {
        GKO_NOT_SUPPORTED(a);
    }
}


GKO_INSTANTIATE_FOR_EACH_VALUE_TYPE(GKO_DECLARE_BATCH_BICGSTAB_APPLY_KERNEL);


}  // namespace batch_bicgstab
}  // namespace hip
}  // namespace kernels
}  // namespace gko
