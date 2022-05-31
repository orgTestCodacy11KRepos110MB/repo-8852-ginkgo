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

#include <gtest/gtest.h>


#include <ginkgo/config.hpp>
#include <ginkgo/core/distributed/matrix.hpp>
#include <ginkgo/core/distributed/partition.hpp>
#include <ginkgo/core/distributed/vector.hpp>
#include <ginkgo/core/matrix/coo.hpp>
#include <ginkgo/core/matrix/csr.hpp>
#include <ginkgo/core/matrix/dense.hpp>
#include <ginkgo/core/matrix/ell.hpp>
#include <ginkgo/core/matrix/fbcsr.hpp>
#include <ginkgo/core/matrix/hybrid.hpp>
#include <ginkgo/core/matrix/sellp.hpp>


#include "core/test/utils.hpp"


namespace {


using comm_index_type = gko::distributed::comm_index_type;


template <typename ValueType, typename IndexType>
class CustomLinOp
    : public gko::EnableLinOp<CustomLinOp<ValueType, IndexType>>,
      public gko::ReadableFromMatrixData<ValueType, IndexType>,
      public gko::EnableCreateMethod<CustomLinOp<ValueType, IndexType>> {
public:
    void read(const gko::matrix_data<ValueType, IndexType>& data) override {}

    explicit CustomLinOp(std::shared_ptr<const gko::Executor> exec)
        : gko::EnableLinOp<CustomLinOp>(exec)
    {}

protected:
    void apply_impl(const gko::LinOp* b, gko::LinOp* x) const override {}

    void apply_impl(const gko::LinOp* alpha, const gko::LinOp* b,
                    const gko::LinOp* beta, gko::LinOp* x) const override
    {}
};


template <typename ValueLocalGlobalIndexType>
class MatrixBuilder : public ::testing::Test {
protected:
    using value_type =
        typename std::tuple_element<0, decltype(
                                           ValueLocalGlobalIndexType())>::type;
    using local_index_type =
        typename std::tuple_element<1, decltype(
                                           ValueLocalGlobalIndexType())>::type;
    using global_index_type =
        typename std::tuple_element<2, decltype(
                                           ValueLocalGlobalIndexType())>::type;
    using dist_mtx_type = gko::distributed::Matrix<value_type, local_index_type,
                                                   global_index_type>;
    using dist_vec_type = gko::distributed::Vector<value_type>;

    MatrixBuilder()
        : ref(gko::ReferenceExecutor::create()),
          comm(gko::mpi::communicator(MPI_COMM_WORLD))
    {}

    void SetUp() override { ASSERT_EQ(comm.size(), 3); }

    template <typename F>
    void forall_matrix_types(F&& f)
    {
        using namespace gko::matrix;
        auto empty_test = [](const gko::LinOp*) {};
        {
            SCOPED_TRACE("With Coo");
            f(gko::with_matrix_type<Coo>(),
              Coo<value_type, local_index_type>::create(this->ref), empty_test);
        }
        {
            SCOPED_TRACE("With Csr");
            f(gko::with_matrix_type<Csr>(),
              Csr<value_type, local_index_type>::create(this->ref), empty_test);
        }
        {
            SCOPED_TRACE("With Csr with strategy");
            using ConcreteCsr = Csr<value_type, local_index_type>;
            f(gko::with_matrix_type<Csr>(
                  std::make_shared<typename ConcreteCsr::classical>()),
              ConcreteCsr::create(this->ref), [](const gko::LinOp* local_mat) {
                  auto local_csr = gko::as<ConcreteCsr>(local_mat);

                  ASSERT_NO_THROW(gko::as<typename ConcreteCsr::classical>(
                      local_csr->get_strategy()));
              });
        }
        {
            SCOPED_TRACE("With Ell");
            f(gko::with_matrix_type<Ell>(),
              Ell<value_type, local_index_type>::create(this->ref), empty_test);
        }
        {
            SCOPED_TRACE("With Fbcsr");
            f(gko::with_matrix_type<Fbcsr>(),
              Fbcsr<value_type, local_index_type>::create(this->ref),
              empty_test);
        }
        {
            SCOPED_TRACE("With Fbcsr with block_size");
            f(gko::with_matrix_type<Fbcsr>(5),
              Fbcsr<value_type, local_index_type>::create(this->ref),
              [](const gko::LinOp* local_mat) {
                  auto local_fbcsr =
                      gko::as<Fbcsr<value_type, local_index_type>>(local_mat);

                  ASSERT_EQ(local_fbcsr->get_block_size(), 5);
              });
        }
        {
            SCOPED_TRACE("With Hybrid");
            f(gko::with_matrix_type<Hybrid>(),
              Hybrid<value_type, local_index_type>::create(this->ref),
              empty_test);
        }
        {
            SCOPED_TRACE("With Hybrid with strategy");
            using Concrete = Hybrid<value_type, local_index_type>;
            f(gko::with_matrix_type<Hybrid>(
                  std::make_shared<typename Concrete::column_limit>(11)),
              Concrete::create(this->ref), [](const gko::LinOp* local_mat) {
                  auto local_hy = gko::as<Concrete>(local_mat);

                  ASSERT_NO_THROW(gko::as<typename Concrete::column_limit>(
                      local_hy->get_strategy()));
                  ASSERT_EQ(gko::as<typename Concrete::column_limit>(
                                local_hy->get_strategy())
                                ->get_num_columns(),
                            11);
              });
        }
        {
            SCOPED_TRACE("With Sellp");
            f(gko::with_matrix_type<Sellp>(),
              Sellp<value_type, local_index_type>::create(this->ref),
              empty_test);
        }
    }

    template <typename InnerMatrixType, typename OuterMatrixType>
    void expected_interface_no_throw(dist_mtx_type* mat,
                                     InnerMatrixType inner_matrix_type,
                                     OuterMatrixType outer_matrix_type)
    {
        auto num_rows = mat->get_size()[0];
        auto a = dist_vec_type::create(ref, comm);
        auto b = dist_vec_type::create(ref, comm);
        auto convert_result = dist_mtx_type::create(
            ref, comm, inner_matrix_type, outer_matrix_type);
        auto move_result = dist_mtx_type::create(ref, comm, inner_matrix_type,
                                                 outer_matrix_type);
        gko::matrix_data<value_type, global_index_type> md{mat->get_size()};
        auto part = gko::distributed::Partition<local_index_type,
                                                global_index_type>::create(ref);

        ASSERT_NO_THROW(mat->apply(a.get(), b.get()));
        ASSERT_NO_THROW(mat->convert_to(convert_result.get()));
        ASSERT_NO_THROW(mat->move_to(move_result.get()));
    }


    std::shared_ptr<const gko::ReferenceExecutor> ref;
    gko::mpi::communicator comm;
};

TYPED_TEST_SUITE(MatrixBuilder, gko::test::ValueLocalGlobalIndexTypes);


TYPED_TEST(MatrixBuilder, BuildWithInner)
{
    using value_type = typename TestFixture::value_type;
    using index_type = typename TestFixture::local_index_type;
    using dist_mat_type = typename TestFixture::dist_mtx_type;
    this->template forall_matrix_types([this](auto with_matrix_type,
                                              auto expected_type_ptr,
                                              auto additional_test) {
        using expected_type = typename std::remove_pointer<decltype(
            expected_type_ptr.get())>::type;

        auto mat =
            dist_mat_type ::create(this->ref, this->comm, with_matrix_type);

        ASSERT_NO_THROW(gko::as<expected_type>(mat->get_const_local_diag()));
        additional_test(mat->get_const_local_diag().get());
        additional_test(mat->get_const_local_offdiag().get());
        this->expected_interface_no_throw(mat.get(), with_matrix_type,
                                          with_matrix_type);
    });
}


TYPED_TEST(MatrixBuilder, BuildWithInnerAndGhost)
{
    using value_type = typename TestFixture::value_type;
    using index_type = typename TestFixture::local_index_type;
    using dist_mat_type = typename TestFixture::dist_mtx_type;
    this->template forall_matrix_types([this](auto with_inner_matrix_type,
                                              auto expected_inner_type_ptr,
                                              auto additional_inner_test) {
        using expected_inner_type = typename std::remove_pointer<decltype(
            expected_inner_type_ptr.get())>::type;
        this->forall_matrix_types([=](auto with_ghost_matrix_type,
                                      auto expected_ghost_type_ptr,
                                      auto additional_ghost_test) {
            using expected_ghost_type = typename std::remove_pointer<decltype(
                expected_ghost_type_ptr.get())>::type;

            auto mat = dist_mat_type ::create(this->ref, this->comm,
                                              with_inner_matrix_type,
                                              with_ghost_matrix_type);

            ASSERT_NO_THROW(
                gko::as<expected_inner_type>(mat->get_const_local_diag()));
            ASSERT_NO_THROW(
                gko::as<expected_ghost_type>(mat->get_const_local_offdiag()));
            additional_inner_test(mat->get_const_local_diag().get());
            additional_ghost_test(mat->get_const_local_offdiag().get());
            this->expected_interface_no_throw(mat.get(), with_inner_matrix_type,
                                              with_ghost_matrix_type);
        });
    });
}


TYPED_TEST(MatrixBuilder, BuildWithCustomLinOp)
{
    using value_type = typename TestFixture::value_type;
    using index_type = typename TestFixture::local_index_type;
    using dist_mat_type = typename TestFixture::dist_mtx_type;
    using custom_type = CustomLinOp<value_type, index_type>;

    auto mat = dist_mat_type::create(this->ref, this->comm,
                                     gko::with_matrix_type<CustomLinOp>());

    ASSERT_NO_THROW(gko::as<custom_type>(mat->get_const_local_diag()));
    this->expected_interface_no_throw(mat.get(),
                                      gko::with_matrix_type<CustomLinOp>(),
                                      gko::with_matrix_type<CustomLinOp>());
}


template <typename ValueLocalGlobalIndexType>
class Matrix : public ::testing::Test {
protected:
    using value_type =
        typename std::tuple_element<0, decltype(
                                           ValueLocalGlobalIndexType())>::type;
    using local_index_type =
        typename std::tuple_element<1, decltype(
                                           ValueLocalGlobalIndexType())>::type;
    using global_index_type =
        typename std::tuple_element<2, decltype(
                                           ValueLocalGlobalIndexType())>::type;
    using dist_mtx_type = gko::distributed::Matrix<value_type, local_index_type,
                                                   global_index_type>;
    using csr_mtx_type = gko::matrix::Csr<value_type, global_index_type>;
    using dist_vec_type = gko::distributed::Vector<value_type>;
    using dense_vec_type = gko::matrix::Dense<value_type>;
    using Partition =
        gko::distributed::Partition<local_index_type, global_index_type>;
    using matrix_data = gko::matrix_data<value_type, global_index_type>;


    Matrix()
        : ref(gko::ReferenceExecutor::create()),
          size{5, 5},
          comm(gko::mpi::communicator(MPI_COMM_WORLD)),
          row_part{Partition::build_from_contiguous(
              ref, gko::array<global_index_type>(
                       ref, I<global_index_type>{0, 2, 4, 5}))},
          col_part{Partition::build_from_mapping(
              ref,
              gko::array<comm_index_type>(ref,
                                          I<comm_index_type>{1, 1, 2, 0, 0}),
              3)},
          mat_input{size,
                    {{0, 1, 1},
                     {0, 3, 2},
                     {1, 1, 3},
                     {1, 2, 4},
                     {2, 1, 5},
                     {2, 2, 6},
                     {3, 3, 8},
                     {3, 4, 7},
                     {4, 0, 9},
                     {4, 4, 10}}},
          dist_input{{{size, {{0, 1, 1}, {0, 3, 2}, {1, 1, 3}, {1, 2, 4}}},
                      {size, {{2, 1, 5}, {2, 2, 6}, {3, 3, 8}, {3, 4, 7}}},
                      {size, {{4, 0, 9}, {4, 4, 10}}}}},
          dist_mat(dist_mtx_type::create(ref, comm)),
          csr_mat(csr_mtx_type::create(ref)),
          x(dist_vec_type::create(ref, comm)),
          y(dist_vec_type::create(ref, comm)),
          dense_x(dense_vec_type::create(ref)),
          dense_y(dense_vec_type::create(ref)),
          engine(42)
    {}

    void SetUp() override { ASSERT_EQ(comm.size(), 3); }

    void assert_local_vector_equal_to_global_vector(const dist_vec_type* dist,
                                                    const dense_vec_type* dense,
                                                    const Partition* part,
                                                    int rank)
    {
        auto range_bounds = part->get_range_bounds();
        auto range_starting_indices = part->get_range_starting_indices();
        auto part_ids = part->get_part_ids();
        std::vector<global_index_type> gather_idxs;
        for (gko::size_type range_id = 0; range_id < part->get_num_ranges();
             ++range_id) {
            if (part_ids[range_id] == rank) {
                for (global_index_type global_row = range_bounds[range_id];
                     global_row < range_bounds[range_id + 1]; ++global_row) {
                    gather_idxs.push_back(global_row);
                }
            }
        }
        auto gather_idxs_view = gko::array<global_index_type>::view(
            this->ref, gather_idxs.size(), gather_idxs.data());
        auto gathered_local = dense->row_gather(&gather_idxs_view);

        GKO_ASSERT_MTX_NEAR(dist->get_local_vector(), gathered_local.get(),
                            r<value_type>::value);
    }

    void init_large(gko::size_type num_rows, gko::size_type num_cols)
    {
        auto rank = this->comm.rank();
        int num_parts = this->comm.size();
        auto vec_md = gko::test::generate_random_matrix_data<value_type,
                                                             global_index_type>(
            num_rows, num_cols,
            std::uniform_int_distribution<int>(static_cast<int>(num_cols),
                                               static_cast<int>(num_cols)),
            std::normal_distribution<gko::remove_complex<value_type>>(),
            this->engine);
        auto mat_md = gko::test::generate_random_matrix_data<value_type,
                                                             global_index_type>(
            num_rows, num_rows,
            std::uniform_int_distribution<int>(0,
                                               static_cast<int>(num_rows) - 1),
            std::normal_distribution<gko::remove_complex<value_type>>(),
            this->engine);

        auto row_mapping = gko::test::generate_random_array<comm_index_type>(
            num_rows, std::uniform_int_distribution<int>(0, num_parts - 1),
            this->engine, this->ref);
        auto col_mapping = gko::test::generate_random_array<comm_index_type>(
            num_rows, std::uniform_int_distribution<int>(0, num_parts - 1),
            this->engine, this->ref);
        row_part_large = gko::share(
            gko::distributed::Partition<local_index_type, global_index_type>::
                build_from_mapping(this->ref, row_mapping, num_parts));
        col_part_large = gko::share(
            gko::distributed::Partition<local_index_type, global_index_type>::
                build_from_mapping(this->ref, col_mapping, num_parts));

        dist_mat->read_distributed(mat_md, row_part_large.get(),
                                   col_part_large.get());
        csr_mat->read(mat_md);

        x->read_distributed(vec_md, col_part_large.get());
        dense_x->read(vec_md);

        y = dist_vec_type::create(
            this->ref, this->comm, gko::dim<2>{num_rows, num_cols},
            gko::dim<2>{static_cast<gko::size_type>(
                            row_part_large->get_part_size(rank)),
                        num_cols});
        dense_y = gko::matrix::Dense<value_type>::create(
            this->ref, gko::dim<2>{num_rows, num_cols});
    }


    std::shared_ptr<const gko::ReferenceExecutor> ref;
    gko::dim<2> size;
    gko::mpi::communicator comm;
    std::shared_ptr<Partition> row_part;
    std::shared_ptr<Partition> col_part;
    std::shared_ptr<Partition> row_part_large;
    std::shared_ptr<Partition> col_part_large;

    gko::matrix_data<value_type, global_index_type> mat_input;
    std::array<matrix_data, 3> dist_input;

    std::unique_ptr<dist_mtx_type> dist_mat;
    std::unique_ptr<csr_mtx_type> csr_mat;

    std::unique_ptr<dist_vec_type> x;
    std::unique_ptr<dist_vec_type> y;
    std::unique_ptr<dense_vec_type> dense_x;
    std::unique_ptr<dense_vec_type> dense_y;

    std::default_random_engine engine;
};

TYPED_TEST_SUITE(Matrix, gko::test::ValueLocalGlobalIndexTypes);


TYPED_TEST(Matrix, ReadsDistributedGlobalData)
{
    using value_type = typename TestFixture::value_type;
    using csr = typename TestFixture::dist_mtx_type::local_matrix_type;
    auto dist_mat = TestFixture::dist_mtx_type::create(this->ref, this->comm);
    I<I<value_type>> res_diag[] = {{{0, 1}, {0, 3}}, {{6, 0}, {0, 8}}, {{10}}};
    I<I<value_type>> res_offdiag[] = {
        {{0, 2}, {4, 0}}, {{5, 0}, {0, 7}}, {{9}}};
    auto rank = dist_mat->get_communicator().rank();

    dist_mat->read_distributed(this->mat_input, this->row_part.get());

    GKO_ASSERT_MTX_NEAR(gko::as<csr>(dist_mat->get_const_local_diag()),
                        res_diag[rank], 0);
    GKO_ASSERT_MTX_NEAR(gko::as<csr>(dist_mat->get_const_local_offdiag()),
                        res_offdiag[rank], 0);
}


TYPED_TEST(Matrix, ReadsDistributedLocalData)
{
    using value_type = typename TestFixture::value_type;
    using csr = typename TestFixture::dist_mtx_type::local_matrix_type;
    auto dist_mat = TestFixture::dist_mtx_type::create(this->ref, this->comm);
    I<I<value_type>> res_diag[] = {{{0, 1}, {0, 3}}, {{6, 0}, {0, 8}}, {{10}}};
    I<I<value_type>> res_offdiag[] = {
        {{0, 2}, {4, 0}}, {{5, 0}, {0, 7}}, {{9}}};
    auto rank = dist_mat->get_communicator().rank();

    dist_mat->read_distributed(this->dist_input[rank], this->row_part.get());

    GKO_ASSERT_MTX_NEAR(gko::as<csr>(dist_mat->get_const_local_diag()),
                        res_diag[rank], 0);
    GKO_ASSERT_MTX_NEAR(gko::as<csr>(dist_mat->get_const_local_offdiag()),
                        res_offdiag[rank], 0);
}


TYPED_TEST(Matrix, ReadsDistributedWithColPartition)
{
    using value_type = typename TestFixture::value_type;
    using csr = typename TestFixture::dist_mtx_type::local_matrix_type;
    auto dist_mat = TestFixture::dist_mtx_type::create(this->ref, this->comm);
    I<I<value_type>> res_diag[] = {{{2, 0}, {0, 0}}, {{0, 5}, {0, 0}}, {{0}}};
    I<I<value_type>> res_offdiag[] = {
        {{1, 0}, {3, 4}}, {{0, 0, 6}, {8, 7, 0}}, {{10, 9}}};
    auto rank = dist_mat->get_communicator().rank();

    dist_mat->read_distributed(this->mat_input, this->row_part.get(),
                               this->col_part.get());

    GKO_ASSERT_MTX_NEAR(gko::as<csr>(dist_mat->get_const_local_diag()),
                        res_diag[rank], 0);
    GKO_ASSERT_MTX_NEAR(gko::as<csr>(dist_mat->get_const_local_offdiag()),
                        res_offdiag[rank], 0);
}


TYPED_TEST(Matrix, CanApplyToSingleVector)
{
    using value_type = typename TestFixture::value_type;
    using index_type = typename TestFixture::global_index_type;
    auto vec_md = gko::matrix_data<value_type, index_type>{
        I<I<value_type>>{{1}, {2}, {3}, {4}, {5}}};
    auto dist_mat = TestFixture::dist_mtx_type ::create(this->ref, this->comm);
    auto x = TestFixture::dist_vec_type ::create(this->ref, this->comm);
    auto y = TestFixture::dist_vec_type ::create(this->ref, this->comm);
    I<I<value_type>> result[3] = {{{10}, {18}}, {{28}, {67}}, {{59}}};
    auto rank = this->comm.rank();
    dist_mat->read_distributed(this->mat_input, this->row_part.get(),
                               this->col_part.get());
    x->read_distributed(vec_md, this->col_part.get());
    y->read_distributed(vec_md, this->row_part.get());
    y->fill(gko::zero<value_type>());

    dist_mat->apply(x.get(), y.get());

    GKO_ASSERT_MTX_NEAR(y->get_local_vector(), result[rank], 0);
}


TYPED_TEST(Matrix, CanApplyToSingleVectorLarge)
{
    this->init_large(100, 1);

    this->dist_mat->apply(this->x.get(), this->y.get());
    this->csr_mat->apply(this->dense_x.get(), this->dense_y.get());

    this->assert_local_vector_equal_to_global_vector(
        this->y.get(), this->dense_y.get(), this->row_part_large.get(),
        this->comm.rank());
}


TYPED_TEST(Matrix, CanApplyToMultipleVectors)
{
    using value_type = typename TestFixture::value_type;
    using index_type = typename TestFixture::global_index_type;
    auto vec_md = gko::matrix_data<value_type, index_type>{
        I<I<value_type>>{{1, 11}, {2, 22}, {3, 33}, {4, 44}, {5, 55}}};
    auto dist_mat = TestFixture::dist_mtx_type ::create(this->ref, this->comm);
    auto x = TestFixture::dist_vec_type ::create(this->ref, this->comm);
    auto y = TestFixture::dist_vec_type ::create(this->ref, this->comm);
    I<I<value_type>> result[3] = {
        {{10, 110}, {18, 198}}, {{28, 308}, {67, 737}}, {{59, 649}}};
    auto rank = this->comm.rank();
    dist_mat->read_distributed(this->mat_input, this->row_part.get(),
                               this->col_part.get());
    x->read_distributed(vec_md, this->col_part.get());
    y->read_distributed(vec_md, this->row_part.get());
    y->fill(gko::zero<value_type>());

    dist_mat->apply(x.get(), y.get());

    GKO_ASSERT_MTX_NEAR(y->get_local_vector(), result[rank], 0);
}


TYPED_TEST(Matrix, CanApplyToMultipleVectorsLarge)
{
    this->init_large(100, 17);

    this->dist_mat->apply(this->x.get(), this->y.get());
    this->csr_mat->apply(this->dense_x.get(), this->dense_y.get());

    this->assert_local_vector_equal_to_global_vector(
        this->y.get(), this->dense_y.get(), this->row_part_large.get(),
        this->comm.rank());
}


TYPED_TEST(Matrix, CanConvertToNextPrecision)
{
    using T = typename TestFixture::value_type;
    using csr = typename TestFixture::dist_mtx_type::local_matrix_type;
    using local_index_type = typename TestFixture::local_index_type;
    using global_index_type = typename TestFixture::global_index_type;
    using OtherT = typename gko::next_precision<T>;
    using OtherDist =
        typename gko::distributed::Matrix<OtherT, local_index_type,
                                          global_index_type>;
    auto tmp = OtherDist::create(this->ref, this->comm);
    auto res = TestFixture::dist_mtx_type::create(this->ref, this->comm);
    this->dist_mat->read_distributed(this->mat_input, this->row_part.get());
    // If OtherT is more precise: 0, otherwise r
    auto residual = r<OtherT>::value < r<T>::value
                        ? gko::remove_complex<T>{0}
                        : gko::remove_complex<T>{r<OtherT>::value};

    this->dist_mat->convert_to(tmp.get());
    tmp->convert_to(res.get());

    GKO_ASSERT_MTX_NEAR(gko::as<csr>(this->dist_mat->get_const_local_diag()),
                        gko::as<csr>(res->get_const_local_diag()), residual);
    GKO_ASSERT_MTX_NEAR(gko::as<csr>(this->dist_mat->get_const_local_offdiag()),
                        gko::as<csr>(res->get_const_local_offdiag()), residual);
}


TYPED_TEST(Matrix, CanMoveToNextPrecision)
{
    using T = typename TestFixture::value_type;
    using csr = typename TestFixture::dist_mtx_type::local_matrix_type;
    using local_index_type = typename TestFixture::local_index_type;
    using global_index_type = typename TestFixture::global_index_type;
    using OtherT = typename gko::next_precision<T>;
    using OtherDist =
        typename gko::distributed::Matrix<OtherT, local_index_type,
                                          global_index_type>;
    auto tmp = OtherDist::create(this->ref, this->comm);
    auto res = TestFixture::dist_mtx_type::create(this->ref, this->comm);
    this->dist_mat->read_distributed(this->mat_input, this->row_part.get());
    auto clone_dist_mat = gko::clone(this->dist_mat);
    // If OtherT is more precise: 0, otherwise r
    auto residual = r<OtherT>::value < r<T>::value
                        ? gko::remove_complex<T>{0}
                        : gko::remove_complex<T>{r<OtherT>::value};

    this->dist_mat->move_to(tmp.get());
    tmp->convert_to(res.get());

    GKO_ASSERT_MTX_NEAR(gko::as<csr>(clone_dist_mat->get_const_local_diag()),
                        gko::as<csr>(res->get_const_local_diag()), residual);
    GKO_ASSERT_MTX_NEAR(gko::as<csr>(clone_dist_mat->get_const_local_offdiag()),
                        gko::as<csr>(res->get_const_local_offdiag()), residual);
}


}  // namespace
