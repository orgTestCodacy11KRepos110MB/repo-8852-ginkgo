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

#ifndef GKO_PUBLIC_CORE_PRECONDITIONER_SCHWARZ_HPP_
#define GKO_PUBLIC_CORE_PRECONDITIONER_SCHWARZ_HPP_


#include <ginkgo/core/base/array.hpp>
#include <ginkgo/core/base/lin_op.hpp>
#include <ginkgo/core/matrix/csr.hpp>
#include <ginkgo/core/matrix/dense.hpp>
#include <ginkgo/core/matrix/diagonal.hpp>
#include <ginkgo/core/multigrid/amgx_pgm.hpp>
#include <ginkgo/core/multigrid/multigrid_level.hpp>


namespace gko {
/**
 * @brief The Preconditioner namespace.
 *
 * @ingroup precond
 */
namespace preconditioner {


/**
 * A Schwarz preconditioner is a simple domain decomposition preconditioner that
 * generalizes the Block Jacobi preconditioner, incorporating options for
 * different local subdomain solvers and overlaps between the subdomains.
 *
 * See Iterative Methods for Sparse Linear Systems (Y. Saad) for a general
 * treatment and variations of the method.
 *
 * @tparam ValueType  precision of matrix elements
 * @tparam IndexType  integral type of the preconditioner
 *
 * @ingroup schwarz
 * @ingroup precond
 * @ingroup LinOp
 */
template <typename ValueType = default_precision, typename IndexType = int32>
class Schwarz : public EnableLinOp<Schwarz<ValueType, IndexType>>,
                public WritableToMatrixData<ValueType, IndexType>,
                public Transposable {
    friend class EnableLinOp<Schwarz>;
    friend class EnablePolymorphicObject<Schwarz, LinOp>;

public:
    using EnableLinOp<Schwarz>::convert_to;
    using EnableLinOp<Schwarz>::move_to;
    using value_type = ValueType;
    using index_type = IndexType;
    using mat_data = matrix_data<ValueType, IndexType>;
    using transposed_type = Schwarz<ValueType, IndexType>;

    /**
     * Returns the number of blocks of the operator.
     *
     * @return the number of blocks of the operator
     */
    size_type get_num_subdomains() const noexcept { return num_subdomains_; }

    void write(mat_data& data) const override;

    std::unique_ptr<LinOp> transpose() const override;

    std::unique_ptr<LinOp> conj_transpose() const override;

    /**
     * Returns the subdomain matrices.
     *
     * @return the subdomain matrices
     */
    std::vector<std::shared_ptr<LinOp>> get_subdomain_matrices() const
    {
        return subdomain_matrices_;
    }


    GKO_CREATE_FACTORY_PARAMETERS(parameters, Factory)
    {
        /**
         * Array of subdomain sizes.
         */
        std::vector<size_type> GKO_FACTORY_PARAMETER_VECTOR(
            subdomain_sizes, std::vector<size_type>{});

        /**
         * Number of subdomains.
         */
        size_type GKO_FACTORY_PARAMETER_SCALAR(num_subdomains, 1);

        /**
         * @brief `true` means it is known that the matrix given to this
         *        factory will be sorted first by row, then by column index,
         *        `false` means it is unknown or not sorted, so an additional
         *        sorting step will be performed during the preconditioner
         *        generation (it will not change the matrix given).
         *        The matrix must be sorted for this preconditioner to work.
         */
        bool GKO_FACTORY_PARAMETER_SCALAR(skip_sorting, false);

        /**
         * Inner solver factory.
         */
        std::shared_ptr<const LinOpFactory> GKO_FACTORY_PARAMETER_SCALAR(
            inner_solver, nullptr);

        /**
         * Generated Inner solvers.
         */
        std::vector<std::shared_ptr<const LinOp>> GKO_FACTORY_PARAMETER_VECTOR(
            generated_inner_solvers, nullptr);

        /**
         * Coarse Operators as MultigridLevel
         */
        std::vector<std::shared_ptr<const multigrid::MultigridLevel>>
            GKO_FACTORY_PARAMETER_VECTOR(coarse_operators, nullptr);

        /**
         * Coarse Solvers
         */
        std::vector<std::shared_ptr<const LinOpFactory>>
            GKO_FACTORY_PARAMETER_VECTOR(coarse_factories, nullptr);
    };
    GKO_ENABLE_LIN_OP_FACTORY(Schwarz, parameters, Factory);
    GKO_ENABLE_BUILD_METHOD(Factory);

protected:
    /**
     * Creates an empty Schwarz preconditioner.
     *
     * @param exec  the executor this object is assigned to
     */
    explicit Schwarz(std::shared_ptr<const Executor> exec)
        : EnableLinOp<Schwarz>(std::move(exec)), num_subdomains_{}
    {}

    /**
     * Creates a Schwarz preconditioner from a matrix using a Schwarz::Factory.
     *
     * @param factory  the factory to use to create the preconditoner
     * @param system_matrix  the matrix this preconditioner should be created
     *                       from
     */
    explicit Schwarz(const Factory* factory,
                     std::shared_ptr<const LinOp> system_matrix)
        : EnableLinOp<Schwarz>(factory->get_executor(),
                               gko::transpose(system_matrix->get_size())),
          parameters_{factory->get_parameters()},
          num_subdomains_{parameters_.subdomain_sizes.size() > 0
                              ? parameters_.subdomain_sizes.size()
                              : parameters_.num_subdomains},
          system_matrix_{std::move(system_matrix)},
          coarse_operators_{parameters_.coarse_operators}
    {
        if (coarse_operators_[0] != nullptr) {
            GKO_ASSERT(coarse_operators_.size() ==
                       parameters_.coarse_factories.size());
            for (auto i = 0; i < coarse_operators_.size(); ++i) {
                coarse_solvers_.emplace_back(
                    parameters_.coarse_factories[i]->generate(
                        coarse_operators_[i]->get_coarse_op()));
            }
            GKO_ASSERT(coarse_operators_.size() == coarse_solvers_.size());
        }
        this->generate(lend(system_matrix_), parameters_.skip_sorting);
    }

    /**
     * Generates the preconditoner.
     *
     * @param system_matrix  the source matrix used to generate the
     *                       preconditioner
     * @param skip_sorting  determines if the sorting of system_matrix can be
     *                      skipped (therefore, marking that it is already
     *                      sorted)
     */
    void generate(const LinOp* system_matrix, bool skip_sorting);

    void apply_impl(const LinOp* b, LinOp* x) const override;

    void apply_dense_impl(const matrix::Dense<ValueType>* b,
                          matrix::Dense<ValueType>* x) const;

    void apply_impl(const LinOp* alpha, const LinOp* b, const LinOp* beta,
                    LinOp* x) const override;

private:
    size_type num_subdomains_;
    std::shared_ptr<const LinOp> system_matrix_{};
    std::vector<std::shared_ptr<LinOp>> subdomain_matrices_;
    std::vector<std::shared_ptr<const LinOp>> subdomain_solvers_;
    std::vector<std::shared_ptr<const LinOp>> coarse_solvers_;
    std::vector<std::shared_ptr<const multigrid::MultigridLevel>>
        coarse_operators_;
};


}  // namespace preconditioner
}  // namespace gko


#endif  // GKO_PUBLIC_CORE_PRECONDITIONER_SCHWARZ_HPP_
