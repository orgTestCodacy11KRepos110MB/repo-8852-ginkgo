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

#ifndef GKO_PUBLIC_CORE_SOLVER_UPPER_TRS_HPP_
#define GKO_PUBLIC_CORE_SOLVER_UPPER_TRS_HPP_


#include <memory>
#include <utility>


#include <ginkgo/core/base/abstract_factory.hpp>
#include <ginkgo/core/base/array.hpp>
#include <ginkgo/core/base/dim.hpp>
#include <ginkgo/core/base/exception_helpers.hpp>
#include <ginkgo/core/base/lin_op.hpp>
#include <ginkgo/core/base/polymorphic_object.hpp>
#include <ginkgo/core/base/types.hpp>
#include <ginkgo/core/base/utils.hpp>
#include <ginkgo/core/log/logger.hpp>
#include <ginkgo/core/matrix/csr.hpp>
#include <ginkgo/core/matrix/identity.hpp>
#include <ginkgo/core/solver/solver_base.hpp>


namespace gko {
namespace solver {


struct SolveStruct;


template <typename ValueType, typename IndexType>
class LowerTrs;


/**
 * UpperTrs is the triangular solver which solves the system U x = b, when U is
 * an upper triangular matrix. It works best when passing in a matrix in CSR
 * format. If the matrix is not in CSR, then the generate step converts it into
 * a CSR matrix. The generation fails if the matrix is not convertible to CSR.
 *
 * @note As the constructor uses the copy and convert functionality, it is not
 *       possible to create a empty solver or a solver with a matrix in any
 *       other format other than CSR, if none of the executor modules are being
 *       compiled with.
 *
 * @tparam ValueType  precision of matrix elements
 * @tparam IndexType  precision of matrix indices
 *
 * @ingroup solvers
 * @ingroup LinOp
 */
template <typename ValueType = default_precision, typename IndexType = int32>
class UpperTrs : public EnableLinOp<UpperTrs<ValueType, IndexType>>,
                 public EnableSolverBase<UpperTrs<ValueType, IndexType>,
                                         matrix::Csr<ValueType, IndexType>>,
                 public Transposable {
    friend class EnableLinOp<UpperTrs>;
    friend class EnablePolymorphicObject<UpperTrs, LinOp>;
    friend class LowerTrs<ValueType, IndexType>;

public:
    using value_type = ValueType;
    using index_type = IndexType;
    using transposed_type = LowerTrs<ValueType, IndexType>;

    std::unique_ptr<LinOp> transpose() const override;

    std::unique_ptr<LinOp> conj_transpose() const override;

    GKO_CREATE_FACTORY_PARAMETERS(parameters, Factory)
    {
        /**
         * Number of right hand sides.
         *
         * @note This value is currently a dummy value which is not used by the
         *       analysis step. It is possible that future algorithms (cusparse
         *       csrsm2) make use of the number of right hand sides for a more
         *       sophisticated implementation. Hence this parameter is left
         *       here. But currently, there is no need to use it.
         */
        gko::size_type GKO_FACTORY_PARAMETER_SCALAR(num_rhs, 1u);

        /**
         * Should the solver use the values on the diagonal of the system matrix
         * (false) or should it assume they are 1.0 (true)?
         */
        bool GKO_FACTORY_PARAMETER_SCALAR(unit_diagonal, false);

        /**
         * Select the implementation which is supposed to be used for
         * the triangular solver. This only matters for the Cuda
         * executor where the choice is between the Ginkgo and the
         * Cusparse implementation. Default is Ginkgo.
         */
        trisolve_algorithm GKO_FACTORY_PARAMETER_SCALAR(
            algorithm, trisolve_algorithm::sparselib);
    };
    GKO_ENABLE_LIN_OP_FACTORY(UpperTrs, parameters, Factory);
    GKO_ENABLE_BUILD_METHOD(Factory);

    /**
     * Copy-assigns a triangular solver. Preserves the executor, shallow-copies
     * the system matrix. If the executors mismatch, clones system matrix onto
     * this executor. Solver analysis information will be regenerated.
     */
    UpperTrs(const UpperTrs&);

    /**
     * Move-assigns a triangular solver. Preserves the executor, moves
     * the system matrix. If the executors mismatch, clones system matrix onto
     * this executor and regenerates solver analysis information. Moved-from
     * object is empty (0x0 and nullptr system matrix)
     */
    UpperTrs(UpperTrs&&);

    /**
     * Copy-constructs a triangular solver. Preserves the executor,
     * shallow-copies the system matrix. Solver analysis information will be
     * regenerated.
     */
    UpperTrs& operator=(const UpperTrs&);

    /**
     * Move-constructs a triangular solver. Preserves the executor, moves
     * the system matrix and solver analysis information. Moved-from
     * object is empty (0x0 and nullptr system matrix)
     */
    UpperTrs& operator=(UpperTrs&&);

protected:
    using CsrMatrix = matrix::Csr<ValueType, IndexType>;

    void apply_impl(const LinOp* b, LinOp* x) const override;

    void apply_impl(const LinOp* alpha, const LinOp* b, const LinOp* beta,
                    LinOp* x) const override;

    /**
     * Generates the analysis structure from the system matrix and the right
     * hand side(only dimensional info needed) needed for the level solver.
     */
    void generate();

    explicit UpperTrs(std::shared_ptr<const Executor> exec)
        : EnableLinOp<UpperTrs>(std::move(exec))
    {}

    explicit UpperTrs(const Factory* factory,
                      std::shared_ptr<const LinOp> system_matrix)
        : EnableLinOp<UpperTrs>(factory->get_executor(),
                                gko::transpose(system_matrix->get_size())),
          EnableSolverBase<UpperTrs<ValueType, IndexType>, CsrMatrix>{
              copy_and_convert_to<CsrMatrix>(factory->get_executor(),
                                             system_matrix)},
          parameters_{factory->get_parameters()}
    {
        this->generate();
    }

private:
    std::shared_ptr<solver::SolveStruct> solve_struct_;
};


template <typename ValueType, typename IndexType>
struct workspace_traits<UpperTrs<ValueType, IndexType>> {
    using Solver = UpperTrs<ValueType, IndexType>;
    // number of vectors used by this workspace
    static int num_vectors(const Solver&);
    // number of arrays used by this workspace
    static int num_arrays(const Solver&);
    // array containing the num_vectors names for the workspace vectors
    static std::vector<std::string> op_names(const Solver&);
    // array containing the num_arrays names for the workspace vectors
    static std::vector<std::string> array_names(const Solver&);
    // array containing all varying scalar vectors (independent of problem size)
    static std::vector<int> scalars(const Solver&);
    // array containing all varying vectors (dependent on problem size)
    static std::vector<int> vectors(const Solver&);

    // transposed input vector
    constexpr static int transposed_b = 0;
    // transposed output vector
    constexpr static int transposed_x = 1;
};


}  // namespace solver
}  // namespace gko


#endif  // GKO_PUBLIC_CORE_SOLVER_UPPER_TRS_HPP_
