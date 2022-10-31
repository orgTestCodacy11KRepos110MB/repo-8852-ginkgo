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

#include <random>


#include <gtest/gtest.h>


#include <ginkgo/core/base/exception.hpp>
#include <ginkgo/core/base/exception_helpers.hpp>
#include <ginkgo/core/base/executor.hpp>
#include <ginkgo/core/matrix/csr.hpp>
#include <ginkgo/core/matrix/dense.hpp>
#include <ginkgo/core/matrix/diagonal.hpp>


#include "core/matrix/sellp_kernels.hpp"
#include "core/test/utils.hpp"


namespace {


class Sellp : public ::testing::Test {
protected:
#if GINKGO_DPCPP_SINGLE_MODE
    using vtype = float;
#else
    using vtype = double;
#endif  // GINKGO_DPCPP_SINGLE_MODE
    using Mtx = gko::matrix::Sellp<vtype>;
    using Vec = gko::matrix::Dense<vtype>;
    using ComplexVec = gko::matrix::Dense<std::complex<vtype>>;

    Sellp() : rand_engine(42) {}

    void SetUp()
    {
        ASSERT_GT(gko::DpcppExecutor::get_num_devices("all"), 0);
        ref = gko::ReferenceExecutor::create();
        dpcpp = gko::DpcppExecutor::create(0, ref);
    }

    void TearDown()
    {
        if (dpcpp != nullptr) {
            ASSERT_NO_THROW(dpcpp->synchronize());
        }
    }

    template <typename MtxType = Vec>
    std::unique_ptr<MtxType> gen_mtx(int num_rows, int num_cols)
    {
        return gko::test::generate_random_matrix<MtxType>(
            num_rows, num_cols, std::uniform_int_distribution<>(1, num_cols),
            std::normal_distribution<vtype>(-1.0, 1.0), rand_engine, ref);
    }

    void set_up_apply_matrix(
        int total_cols = 1, int slice_size = gko::matrix::default_slice_size,
        int stride_factor = gko::matrix::default_stride_factor)
    {
        mtx = gen_mtx<Mtx>(532, 231);
        empty = Mtx::create(ref);
        expected = gen_mtx(532, total_cols);
        y = gen_mtx(231, total_cols);
        alpha = gko::initialize<Vec>({2.0}, ref);
        beta = gko::initialize<Vec>({-1.0}, ref);
        dmtx = gko::clone(dpcpp, mtx);
        dempty = Mtx::create(dpcpp);
        dresult = gko::clone(dpcpp, expected);
        dy = gko::clone(dpcpp, y);
        dalpha = gko::clone(dpcpp, alpha);
        dbeta = gko::clone(dpcpp, beta);
    }

    std::shared_ptr<gko::ReferenceExecutor> ref;
    std::shared_ptr<const gko::DpcppExecutor> dpcpp;

    std::default_random_engine rand_engine;

    std::unique_ptr<Mtx> mtx;
    std::unique_ptr<Mtx> empty;
    std::unique_ptr<Vec> expected;
    std::unique_ptr<Vec> y;
    std::unique_ptr<Vec> alpha;
    std::unique_ptr<Vec> beta;

    std::unique_ptr<Mtx> dmtx;
    std::unique_ptr<Mtx> dempty;
    std::unique_ptr<Vec> dresult;
    std::unique_ptr<Vec> dy;
    std::unique_ptr<Vec> dalpha;
    std::unique_ptr<Vec> dbeta;
};


TEST_F(Sellp, SimpleApplyIsEquivalentToRef)
{
    set_up_apply_matrix();

    mtx->apply(y.get(), expected.get());
    dmtx->apply(dy.get(), dresult.get());

    GKO_ASSERT_MTX_NEAR(dresult, expected, r<vtype>::value);
}


TEST_F(Sellp, AdvancedApplyIsEquivalentToRef)
{
    set_up_apply_matrix();

    mtx->apply(alpha.get(), y.get(), beta.get(), expected.get());
    dmtx->apply(dalpha.get(), dy.get(), dbeta.get(), dresult.get());

    GKO_ASSERT_MTX_NEAR(dresult, expected, r<vtype>::value);
}


TEST_F(Sellp, SimpleApplyWithSliceSizeAndStrideFactorIsEquivalentToRef)
{
    set_up_apply_matrix(1, 32, 2);

    mtx->apply(y.get(), expected.get());
    dmtx->apply(dy.get(), dresult.get());

    GKO_ASSERT_MTX_NEAR(dresult, expected, r<vtype>::value);
}


TEST_F(Sellp, AdvancedApplyWithSliceSizeAndStrideFActorIsEquivalentToRef)
{
    set_up_apply_matrix(1, 32, 2);

    mtx->apply(alpha.get(), y.get(), beta.get(), expected.get());
    dmtx->apply(dalpha.get(), dy.get(), dbeta.get(), dresult.get());

    GKO_ASSERT_MTX_NEAR(dresult, expected, r<vtype>::value);
}


TEST_F(Sellp, SimpleApplyMultipleRHSIsEquivalentToRef)
{
    set_up_apply_matrix(64);

    mtx->apply(y.get(), expected.get());
    dmtx->apply(dy.get(), dresult.get());

    GKO_ASSERT_MTX_NEAR(dresult, expected, r<vtype>::value);
}


TEST_F(Sellp, AdvancedApplyMultipleRHSIsEquivalentToRef)
{
    set_up_apply_matrix(64);

    mtx->apply(alpha.get(), y.get(), beta.get(), expected.get());
    dmtx->apply(dalpha.get(), dy.get(), dbeta.get(), dresult.get());

    GKO_ASSERT_MTX_NEAR(dresult, expected, r<vtype>::value);
}


TEST_F(Sellp,
       SimpleApplyMultipleRHSWithSliceSizeAndStrideFactorIsEquivalentToRef)
{
    set_up_apply_matrix(32, 2);

    mtx->apply(y.get(), expected.get());
    dmtx->apply(dy.get(), dresult.get());

    GKO_ASSERT_MTX_NEAR(dresult, expected, r<vtype>::value);
}


TEST_F(Sellp,
       AdvancedApplyMultipleRHSWithSliceSizeAndStrideFActorIsEquivalentToRef)
{
    set_up_apply_matrix(32, 2);

    mtx->apply(alpha.get(), y.get(), beta.get(), expected.get());
    dmtx->apply(dalpha.get(), dy.get(), dbeta.get(), dresult.get());

    GKO_ASSERT_MTX_NEAR(dresult, expected, r<vtype>::value);
}


TEST_F(Sellp, ApplyToComplexIsEquivalentToRef)
{
    set_up_apply_matrix(64);
    auto complex_b = gen_mtx<ComplexVec>(231, 3);
    auto dcomplex_b = gko::clone(dpcpp, complex_b);
    auto complex_x = gen_mtx<ComplexVec>(532, 3);
    auto dcomplex_x = gko::clone(dpcpp, complex_x);

    mtx->apply(complex_b.get(), complex_x.get());
    dmtx->apply(dcomplex_b.get(), dcomplex_x.get());

    GKO_ASSERT_MTX_NEAR(dcomplex_x, complex_x, r<vtype>::value);
}


TEST_F(Sellp, AdvancedApplyToComplexIsEquivalentToRef)
{
    set_up_apply_matrix(64);
    auto complex_b = gen_mtx<ComplexVec>(231, 3);
    auto dcomplex_b = gko::clone(dpcpp, complex_b);
    auto complex_x = gen_mtx<ComplexVec>(532, 3);
    auto dcomplex_x = gko::clone(dpcpp, complex_x);

    mtx->apply(alpha.get(), complex_b.get(), beta.get(), complex_x.get());
    dmtx->apply(dalpha.get(), dcomplex_b.get(), dbeta.get(), dcomplex_x.get());

    GKO_ASSERT_MTX_NEAR(dcomplex_x, complex_x, r<vtype>::value);
}


TEST_F(Sellp, ConvertToDenseIsEquivalentToRef)
{
    set_up_apply_matrix(64);
    auto dense_mtx = gko::matrix::Dense<vtype>::create(ref);
    auto ddense_mtx = gko::matrix::Dense<vtype>::create(dpcpp);

    mtx->convert_to(dense_mtx.get());
    dmtx->convert_to(ddense_mtx.get());

    GKO_ASSERT_MTX_NEAR(dense_mtx.get(), ddense_mtx.get(), 0);
}


TEST_F(Sellp, ConvertToCsrIsEquivalentToRef)
{
    set_up_apply_matrix(64);
    auto csr_mtx = gko::matrix::Csr<vtype>::create(ref);
    auto dcsr_mtx = gko::matrix::Csr<vtype>::create(dpcpp);

    mtx->convert_to(csr_mtx.get());
    dmtx->convert_to(dcsr_mtx.get());

    GKO_ASSERT_MTX_NEAR(csr_mtx.get(), dcsr_mtx.get(), 0);
}


TEST_F(Sellp, ConvertEmptyToDenseIsEquivalentToRef)
{
    set_up_apply_matrix(64);
    auto dense_mtx = gko::matrix::Dense<vtype>::create(ref);
    auto ddense_mtx = gko::matrix::Dense<vtype>::create(dpcpp);

    empty->convert_to(dense_mtx.get());
    dempty->convert_to(ddense_mtx.get());

    GKO_ASSERT_MTX_NEAR(dense_mtx.get(), ddense_mtx.get(), 0);
}


TEST_F(Sellp, ConvertEmptyToCsrIsEquivalentToRef)
{
    set_up_apply_matrix(64);
    auto csr_mtx = gko::matrix::Csr<vtype>::create(ref);
    auto dcsr_mtx = gko::matrix::Csr<vtype>::create(dpcpp);

    empty->convert_to(csr_mtx.get());
    dempty->convert_to(dcsr_mtx.get());

    GKO_ASSERT_MTX_NEAR(csr_mtx.get(), dcsr_mtx.get(), 0);
}


TEST_F(Sellp, ExtractDiagonalIsEquivalentToRef)
{
    set_up_apply_matrix(64);

    auto diag = mtx->extract_diagonal();
    auto ddiag = dmtx->extract_diagonal();

    GKO_ASSERT_MTX_NEAR(diag.get(), ddiag.get(), 0);
}


TEST_F(Sellp, ExtractDiagonalWithSliceSizeAndStrideFactorIsEquivalentToRef)
{
    set_up_apply_matrix(64, 32, 2);

    auto diag = mtx->extract_diagonal();
    auto ddiag = dmtx->extract_diagonal();

    GKO_ASSERT_MTX_NEAR(diag.get(), ddiag.get(), 0);
}


TEST_F(Sellp, InplaceAbsoluteMatrixIsEquivalentToRef)
{
    set_up_apply_matrix(64, 32, 2);

    mtx->compute_absolute_inplace();
    dmtx->compute_absolute_inplace();

    GKO_ASSERT_MTX_NEAR(mtx, dmtx, r<vtype>::value);
}


TEST_F(Sellp, OutplaceAbsoluteMatrixIsEquivalentToRef)
{
    set_up_apply_matrix(64, 32, 2);

    auto abs_mtx = mtx->compute_absolute();
    auto dabs_mtx = dmtx->compute_absolute();

    GKO_ASSERT_MTX_NEAR(abs_mtx, dabs_mtx, r<vtype>::value);
}


}  // namespace