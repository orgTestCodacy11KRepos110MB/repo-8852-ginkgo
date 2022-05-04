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

#include <algorithm>
#include <memory>
#include <vector>


#include <gtest/gtest-typed-test.h>
#include <gtest/gtest.h>


#include <ginkgo/core/base/device_matrix_data.hpp>
#include <ginkgo/core/base/executor.hpp>
#include <ginkgo/core/base/matrix_data.hpp>
#include <ginkgo/core/matrix/csr.hpp>


#include "core/distributed/matrix_kernels.hpp"
#include "core/test/utils.hpp"


namespace {


using comm_index_type = gko::distributed::comm_index_type;


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
    using Mtx = gko::matrix::Csr<value_type, local_index_type>;

    Matrix()
        : ref(gko::ReferenceExecutor::create()),
          mapping{ref},
          diag_row_idxs{ref},
          diag_col_idxs{ref},
          diag_values{ref},
          offdiag_row_idxs{ref},
          offdiag_col_idxs{ref},
          offdiag_values{ref},
          gather_idxs{ref},
          recv_offsets{ref},
          local_to_global_ghost{ref}
    {}

    void validate(
        gko::dim<2> size,
        const gko::distributed::Partition<local_index_type, global_index_type>*
            row_partition,
        const gko::distributed::Partition<local_index_type, global_index_type>*
            col_partition,
        std::initializer_list<global_index_type> input_rows,
        std::initializer_list<global_index_type> input_cols,
        std::initializer_list<value_type> input_vals,
        std::initializer_list<
            std::tuple<gko::dim<2>, std::initializer_list<global_index_type>,
                       std::initializer_list<global_index_type>,
                       std::initializer_list<value_type>>>
            diag_entries,
        std::initializer_list<
            std::tuple<gko::dim<2>, std::initializer_list<global_index_type>,
                       std::initializer_list<global_index_type>,
                       std::initializer_list<value_type>>>
            offdiag_entries,
        std::initializer_list<std::initializer_list<local_index_type>>
            gather_idx_entries,
        std::initializer_list<std::initializer_list<comm_index_type>>
            recv_offset_entries)
    {
        using local_d_md_type =
            gko::device_matrix_data<value_type, local_index_type>;
        using md_type = typename local_d_md_type::host_type;
        std::vector<gko::device_matrix_data<value_type, local_index_type>>
            ref_diags;
        std::vector<gko::device_matrix_data<value_type, local_index_type>>
            ref_offdiags;
        std::vector<gko::array<local_index_type>> ref_gather_idxs;
        std::vector<gko::array<comm_index_type>> ref_recv_offsets;

        auto input = gko::device_matrix_data<value_type, global_index_type>{
            ref, size, input_rows, input_cols, input_vals};
        this->recv_offsets.resize_and_reset(
            static_cast<gko::size_type>(row_partition->get_num_parts() + 1));
        for (auto entry : diag_entries) {
            ref_diags.emplace_back(ref, std::get<0>(entry), std::get<1>(entry),
                                   std::get<2>(entry), std::get<3>(entry));
        }
        for (auto entry : offdiag_entries) {
            ref_offdiags.emplace_back(ref, std::get<0>(entry),
                                      std::get<1>(entry), std::get<2>(entry),
                                      std::get<3>(entry));
        }
        for (auto entry : gather_idx_entries) {
            ref_gather_idxs.emplace_back(ref, entry);
        }
        for (auto entry : recv_offset_entries) {
            ref_recv_offsets.emplace_back(ref, entry);
        }

        for (comm_index_type part = 0; part < row_partition->get_num_parts();
             ++part) {
            gko::kernels::reference::distributed_matrix::build_diag_offdiag(
                ref, input, row_partition, col_partition, part, diag_row_idxs,
                diag_col_idxs, diag_values, offdiag_row_idxs, offdiag_col_idxs,
                offdiag_values, gather_idxs, recv_offsets.get_data(),
                local_to_global_ghost);

            assert_device_matrix_data_equal(diag_row_idxs, diag_col_idxs,
                                            diag_values, ref_diags[part]);
            assert_device_matrix_data_equal(offdiag_row_idxs, offdiag_col_idxs,
                                            offdiag_values, ref_offdiags[part]);
            GKO_ASSERT_ARRAY_EQ(gather_idxs, ref_gather_idxs[part]);
            GKO_ASSERT_ARRAY_EQ(recv_offsets, ref_recv_offsets[part]);
        }
    }

    template <typename A1, typename A2, typename A3, typename Data2>
    void assert_device_matrix_data_equal(A1& row_idxs, A2& col_idxs, A3& values,
                                         Data2& second)
    {
        auto array_second = second.empty_out();

        GKO_ASSERT_ARRAY_EQ(row_idxs, array_second.row_idxs);
        GKO_ASSERT_ARRAY_EQ(col_idxs, array_second.col_idxs);
        GKO_ASSERT_ARRAY_EQ(values, array_second.values);
    }

    gko::device_matrix_data<value_type, global_index_type>
    create_input_not_full_rank()
    {
        return gko::device_matrix_data<value_type, global_index_type>{
            this->ref, gko::dim<2>{7, 7},
            I<global_index_type>{0, 0, 2, 3, 3, 4, 4, 5, 5, 6},
            I<global_index_type>{0, 3, 2, 0, 3, 4, 6, 4, 5, 5},
            I<value_type>{1, 2, 5, 6, 7, 8, 9, 10, 11, 12}};
    }

    gko::device_matrix_data<value_type, global_index_type>
    create_input_full_rank()
    {
        return gko::device_matrix_data<value_type, global_index_type>{
            this->ref, gko::dim<2>{7, 7},
            I<global_index_type>{0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6},
            I<global_index_type>{0, 3, 1, 2, 2, 0, 3, 4, 6, 4, 5, 5},
            I<value_type>{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}};
    }

    std::shared_ptr<const gko::ReferenceExecutor> ref;
    gko::array<comm_index_type> mapping;
    gko::array<local_index_type> diag_row_idxs;
    gko::array<local_index_type> diag_col_idxs;
    gko::array<value_type> diag_values;
    gko::array<local_index_type> offdiag_row_idxs;
    gko::array<local_index_type> offdiag_col_idxs;
    gko::array<value_type> offdiag_values;
    gko::array<local_index_type> gather_idxs;
    gko::array<comm_index_type> recv_offsets;
    gko::array<global_index_type> local_to_global_ghost;
};

TYPED_TEST_SUITE(Matrix, gko::test::ValueLocalGlobalIndexTypes);


TYPED_TEST(Matrix, BuildsDiagOffdiagEmpty)
{
    using lit = typename TestFixture::local_index_type;
    using git = typename TestFixture::global_index_type;
    using vt = typename TestFixture::value_type;
    this->mapping = {this->ref, {1, 0, 2, 2, 0, 1, 1, 2}};
    comm_index_type num_parts = 3;
    auto partition = gko::distributed::Partition<lit, git>::build_from_mapping(
        this->ref, this->mapping, num_parts);

    this->validate(
        gko::dim<2>{8, 8}, partition.get(), partition.get(), {}, {}, {},
        {std::make_tuple(gko::dim<2>{2, 2}, I<git>{}, I<git>{}, I<vt>{}),
         std::make_tuple(gko::dim<2>{3, 3}, I<git>{}, I<git>{}, I<vt>{}),
         std::make_tuple(gko::dim<2>{3, 3}, I<git>{}, I<git>{}, I<vt>{})},
        {std::make_tuple(gko::dim<2>{2, 0}, I<git>{}, I<git>{}, I<vt>{}),
         std::make_tuple(gko::dim<2>{3, 0}, I<git>{}, I<git>{}, I<vt>{}),
         std::make_tuple(gko::dim<2>{3, 0}, I<git>{}, I<git>{}, I<vt>{})},
        {{}, {}, {}}, {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}});
}


TYPED_TEST(Matrix, BuildsDiagOffdiagSmall)
{
    using lit = typename TestFixture::local_index_type;
    using git = typename TestFixture::global_index_type;
    using vt = typename TestFixture::value_type;
    this->mapping = {this->ref, {1, 0}};
    comm_index_type num_parts = 2;
    auto partition = gko::distributed::Partition<lit, git>::build_from_mapping(
        this->ref, this->mapping, num_parts);

    this->validate(
        gko::dim<2>{2, 2}, partition.get(), partition.get(), {0, 0, 1, 1},
        {0, 1, 0, 1}, {1, 2, 3, 4},
        {std::make_tuple(gko::dim<2>{1, 1}, I<git>{0}, I<git>{0}, I<vt>{4}),
         std::make_tuple(gko::dim<2>{1, 1}, I<git>{0}, I<git>{0}, I<vt>{1})},
        {std::make_tuple(gko::dim<2>{1, 1}, I<git>{0}, I<git>{0}, I<vt>{3}),
         std::make_tuple(gko::dim<2>{1, 1}, I<git>{0}, I<git>{0}, I<vt>{2})},
        {{0}, {0}}, {{0, 0, 1}, {0, 1, 1}});
}


TYPED_TEST(Matrix, BuildsDiagOffdiagNoOffdiag)
{
    using lit = typename TestFixture::local_index_type;
    using git = typename TestFixture::global_index_type;
    using vt = typename TestFixture::value_type;
    this->mapping = {this->ref, {1, 2, 0, 0, 2, 1}};
    comm_index_type num_parts = 3;
    auto partition = gko::distributed::Partition<lit, git>::build_from_mapping(
        this->ref, this->mapping, num_parts);

    this->validate(
        gko::dim<2>{6, 6}, partition.get(), partition.get(),
        {0, 0, 1, 1, 2, 3, 4, 5}, {0, 5, 1, 4, 3, 2, 4, 0},
        {1, 2, 3, 4, 5, 6, 7, 8},
        {std::make_tuple(gko::dim<2>{2, 2}, I<git>{0, 1}, I<git>{1, 0},
                         I<vt>{5, 6}),
         std::make_tuple(gko::dim<2>{2, 2}, I<git>{0, 0, 1}, I<git>{0, 1, 0},
                         I<vt>{1, 2, 8}),
         std::make_tuple(gko::dim<2>{2, 2}, I<git>{0, 0, 1}, I<git>{0, 1, 1},
                         I<vt>{3, 4, 7})},
        {std::make_tuple(gko::dim<2>{2, 0}, I<git>{}, I<git>{}, I<vt>{}),
         std::make_tuple(gko::dim<2>{2, 0}, I<git>{}, I<git>{}, I<vt>{}),
         std::make_tuple(gko::dim<2>{2, 0}, I<git>{}, I<git>{}, I<vt>{})},
        {{}, {}, {}}, {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}});
}


TYPED_TEST(Matrix, BuildsDiagOffdiagNoDiag)
{
    using lit = typename TestFixture::local_index_type;
    using git = typename TestFixture::global_index_type;
    using vt = typename TestFixture::value_type;
    this->mapping = {this->ref, {1, 2, 0, 0, 2, 1}};
    comm_index_type num_parts = 3;
    auto partition = gko::distributed::Partition<lit, git>::build_from_mapping(
        this->ref, this->mapping, num_parts);

    this->validate(
        gko::dim<2>{6, 6}, partition.get(), partition.get(), {0, 0, 1, 3, 4, 5},
        {1, 3, 5, 1, 3, 2}, {1, 2, 5, 6, 7, 8},
        {std::make_tuple(gko::dim<2>{2, 2}, I<git>{}, I<git>{}, I<vt>{}),
         std::make_tuple(gko::dim<2>{2, 2}, I<git>{}, I<git>{}, I<vt>{}),
         std::make_tuple(gko::dim<2>{2, 2}, I<git>{}, I<git>{}, I<vt>{})},
        {std::make_tuple(gko::dim<2>{2, 1}, I<git>{1}, I<git>{0}, I<vt>{6}),
         std::make_tuple(gko::dim<2>{2, 3}, I<git>{0, 0, 1}, I<git>{2, 1, 0},
                         I<vt>{1, 2, 8}),
         std::make_tuple(gko::dim<2>{2, 2}, I<git>{0, 1}, I<git>{1, 0},
                         I<vt>{5, 7})},
        {{0}, {0, 1, 0}, {1, 1}}, {{0, 0, 0, 1}, {0, 2, 2, 3}, {0, 1, 2, 2}});
}


TYPED_TEST(Matrix, BuildsDiagOffdiagMixed)
{
    using lit = typename TestFixture::local_index_type;
    using git = typename TestFixture::global_index_type;
    using vt = typename TestFixture::value_type;
    this->mapping = {this->ref, {1, 2, 0, 0, 2, 1}};
    comm_index_type num_parts = 3;
    auto partition = gko::distributed::Partition<lit, git>::build_from_mapping(
        this->ref, this->mapping, num_parts);

    this->validate(
        gko::dim<2>{6, 6}, partition.get(), partition.get(),
        {0, 0, 0, 0, 1, 1, 1, 2, 3, 3, 4, 4, 5, 5},
        {0, 1, 3, 5, 1, 4, 5, 3, 1, 2, 3, 4, 0, 2},
        {11, 1, 2, 12, 13, 14, 5, 15, 6, 16, 7, 17, 18, 8},

        {std::make_tuple(gko::dim<2>{2, 2}, I<git>{0, 1}, I<git>{1, 0},
                         I<vt>{15, 16}),
         std::make_tuple(gko::dim<2>{2, 2}, I<git>{0, 0, 1}, I<git>{0, 1, 0},
                         I<vt>{11, 12, 18}),
         std::make_tuple(gko::dim<2>{2, 2}, I<git>{0, 0, 1}, I<git>{0, 1, 1},
                         I<vt>{13, 14, 17})},
        {std::make_tuple(gko::dim<2>{2, 1}, I<git>{1}, I<git>{0}, I<vt>{6}),
         std::make_tuple(gko::dim<2>{2, 3}, I<git>{0, 0, 1}, I<git>{2, 1, 0},
                         I<vt>{1, 2, 8}),
         std::make_tuple(gko::dim<2>{2, 2}, I<git>{0, 1}, I<git>{1, 0},
                         I<vt>{5, 7})},
        {{0}, {0, 1, 0}, {1, 1}}, {{0, 0, 0, 1}, {0, 2, 2, 3}, {0, 1, 2, 2}});
}


TYPED_TEST(Matrix, BuildsDiagOffdiagEmptyWithColPartition)
{
    using lit = typename TestFixture::local_index_type;
    using git = typename TestFixture::global_index_type;
    using vt = typename TestFixture::value_type;
    this->mapping = {this->ref, {1, 0, 2, 2, 0, 1, 1, 2}};
    comm_index_type num_parts = 3;
    auto partition = gko::distributed::Partition<lit, git>::build_from_mapping(
        this->ref, this->mapping, num_parts);
    gko::array<comm_index_type> col_mapping{this->ref,
                                            {0, 0, 2, 2, 2, 1, 1, 1}};
    auto col_partition =
        gko::distributed::Partition<lit, git>::build_from_mapping(
            this->ref, col_mapping, num_parts);

    this->validate(
        gko::dim<2>{8, 8}, partition.get(), col_partition.get(), {}, {}, {},
        {std::make_tuple(gko::dim<2>{2, 2}, I<git>{}, I<git>{}, I<vt>{}),
         std::make_tuple(gko::dim<2>{3, 3}, I<git>{}, I<git>{}, I<vt>{}),
         std::make_tuple(gko::dim<2>{3, 3}, I<git>{}, I<git>{}, I<vt>{})},
        {std::make_tuple(gko::dim<2>{2, 0}, I<git>{}, I<git>{}, I<vt>{}),
         std::make_tuple(gko::dim<2>{3, 0}, I<git>{}, I<git>{}, I<vt>{}),
         std::make_tuple(gko::dim<2>{3, 0}, I<git>{}, I<git>{}, I<vt>{})},
        {{}, {}, {}}, {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}});
}


TYPED_TEST(Matrix, BuildsDiagOffdiagSmallWithColPartition)
{
    using lit = typename TestFixture::local_index_type;
    using git = typename TestFixture::global_index_type;
    using vt = typename TestFixture::value_type;
    this->mapping = {this->ref, {1, 0}};
    comm_index_type num_parts = 2;
    auto partition = gko::distributed::Partition<lit, git>::build_from_mapping(
        this->ref, this->mapping, num_parts);
    gko::array<comm_index_type> col_mapping{this->ref, {0, 1}};
    auto col_partition =
        gko::distributed::Partition<lit, git>::build_from_mapping(
            this->ref, col_mapping, num_parts);

    this->validate(
        gko::dim<2>{2, 2}, partition.get(), col_partition.get(), {0, 0, 1, 1},
        {0, 1, 0, 1}, {1, 2, 3, 4},
        {std::make_tuple(gko::dim<2>{1, 1}, I<git>{0}, I<git>{0}, I<vt>{3}),
         std::make_tuple(gko::dim<2>{1, 1}, I<git>{0}, I<git>{0}, I<vt>{2})},
        {std::make_tuple(gko::dim<2>{1, 1}, I<git>{0}, I<git>{0}, I<vt>{4}),
         std::make_tuple(gko::dim<2>{1, 1}, I<git>{0}, I<git>{0}, I<vt>{1})},
        {{0}, {0}}, {{0, 0, 1}, {0, 1, 1}});
}

TYPED_TEST(Matrix, BuildsDiagOffdiagNoOffdiagWithColPartition)
{
    using lit = typename TestFixture::local_index_type;
    using git = typename TestFixture::global_index_type;
    using vt = typename TestFixture::value_type;
    this->mapping = {this->ref, {1, 2, 0, 0, 2, 1}};
    comm_index_type num_parts = 3;
    auto partition = gko::distributed::Partition<lit, git>::build_from_mapping(
        this->ref, this->mapping, num_parts);
    gko::array<comm_index_type> col_mapping{this->ref, {0, 0, 2, 2, 1, 1}};
    auto col_partition =
        gko::distributed::Partition<lit, git>::build_from_mapping(
            this->ref, col_mapping, num_parts);

    this->validate(
        gko::dim<2>{6, 6}, partition.get(), col_partition.get(),
        {3, 0, 5, 1, 1, 4}, {1, 4, 5, 2, 3, 3}, {1, 2, 3, 4, 5, 6},
        {std::make_tuple(gko::dim<2>{2, 2}, I<git>{1}, I<git>{1}, I<vt>{1}),
         std::make_tuple(gko::dim<2>{2, 2}, I<git>{0, 1}, I<git>{0, 1},
                         I<vt>{2, 3}),
         std::make_tuple(gko::dim<2>{2, 2}, I<git>{0, 0, 1}, I<git>{0, 1, 1},
                         I<vt>{4, 5, 6})},
        {std::make_tuple(gko::dim<2>{2, 0}, I<git>{}, I<git>{}, I<vt>{}),
         std::make_tuple(gko::dim<2>{2, 0}, I<git>{}, I<git>{}, I<vt>{}),
         std::make_tuple(gko::dim<2>{2, 0}, I<git>{}, I<git>{}, I<vt>{})},
        {{}, {}, {}}, {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0, 0, 0}});
}


TYPED_TEST(Matrix, BuildsDiagOffdiagNoDiagWithColPartition)
{
    using lit = typename TestFixture::local_index_type;
    using git = typename TestFixture::global_index_type;
    using vt = typename TestFixture::value_type;
    this->mapping = {this->ref, {1, 2, 0, 0, 2, 1}};
    comm_index_type num_parts = 3;
    auto partition = gko::distributed::Partition<lit, git>::build_from_mapping(
        this->ref, this->mapping, num_parts);
    gko::array<comm_index_type> col_mapping{this->ref, {0, 0, 2, 2, 1, 1}};
    auto col_partition =
        gko::distributed::Partition<lit, git>::build_from_mapping(
            this->ref, col_mapping, num_parts);

    this->validate(
        gko::dim<2>{6, 6}, partition.get(), col_partition.get(),
        {2, 3, 2, 0, 5, 1, 1}, {2, 3, 5, 0, 1, 1, 4}, {1, 2, 3, 4, 5, 6, 7},
        {std::make_tuple(gko::dim<2>{2, 2}, I<git>{}, I<git>{}, I<vt>{}),
         std::make_tuple(gko::dim<2>{2, 2}, I<git>{}, I<git>{}, I<vt>{}),
         std::make_tuple(gko::dim<2>{2, 2}, I<git>{}, I<git>{}, I<vt>{})},
        {std::make_tuple(gko::dim<2>{2, 3}, I<git>{0, 1, 0}, I<git>{1, 2, 0},
                         I<vt>{1, 2, 3}),
         std::make_tuple(gko::dim<2>{2, 2}, I<git>{0, 1}, I<git>{0, 1},
                         I<vt>{4, 5}),
         std::make_tuple(gko::dim<2>{2, 2}, I<git>{0, 0}, I<git>{0, 1},
                         I<vt>{6, 7})},
        {{1, 0, 1}, {0, 1}, {1, 0}},
        {{0, 0, 1, 3}, {0, 2, 2, 2}, {0, 1, 2, 2}});
}


TYPED_TEST(Matrix, BuildsDiagOffdiagMixedWithColPartition)
{
    using lit = typename TestFixture::local_index_type;
    using git = typename TestFixture::global_index_type;
    using vt = typename TestFixture::value_type;
    this->mapping = {this->ref, {1, 2, 0, 0, 2, 1}};
    comm_index_type num_parts = 3;
    auto partition = gko::distributed::Partition<lit, git>::build_from_mapping(
        this->ref, this->mapping, num_parts);
    gko::array<comm_index_type> col_mapping{this->ref, {0, 0, 2, 2, 1, 1}};
    auto col_partition =
        gko::distributed::Partition<lit, git>::build_from_mapping(
            this->ref, col_mapping, num_parts);

    this->validate(gko::dim<2>{6, 6}, partition.get(), col_partition.get(),
                   {2, 3, 3, 0, 5, 1, 4, 2, 3, 2, 0, 0, 1, 1, 4, 4},
                   {0, 0, 1, 5, 4, 2, 2, 3, 2, 4, 1, 2, 4, 5, 0, 5},
                   {11, 12, 13, 14, 15, 16, 17, 1, 2, 3, 4, 5, 6, 7, 8, 9},
                   {std::make_tuple(gko::dim<2>{2, 2}, I<git>{0, 1, 1},
                                    I<git>{0, 0, 1}, I<vt>{11, 12, 13}),
                    std::make_tuple(gko::dim<2>{2, 2}, I<git>{0, 1},
                                    I<git>{1, 0}, I<vt>{14, 15}),
                    std::make_tuple(gko::dim<2>{2, 2}, I<git>{0, 1},
                                    I<git>{0, 0}, I<vt>{16, 17})},
                   {std::make_tuple(gko::dim<2>{2, 3}, I<git>{0, 1, 0},
                                    I<git>{2, 1, 0}, I<vt>{1, 2, 3}),
                    std::make_tuple(gko::dim<2>{2, 2}, I<git>{0, 0},
                                    I<git>{0, 1}, I<vt>{4, 5}),
                    std::make_tuple(gko::dim<2>{2, 3}, I<git>{0, 0, 1, 1},
                                    I<git>{1, 2, 0, 2}, I<vt>{6, 7, 8, 9})},
                   {{0, 0, 1}, {1, 0}, {0, 0, 1}},
                   {{0, 0, 1, 3}, {0, 1, 1, 2}, {0, 1, 3, 3}});
}


TYPED_TEST(Matrix, BuildsDiagOffdiagNonSquare)
{
    using lit = typename TestFixture::local_index_type;
    using git = typename TestFixture::global_index_type;
    using vt = typename TestFixture::value_type;
    gko::array<comm_index_type> row_mapping{this->ref, {1, 2, 0, 0, 2, 1}};
    comm_index_type num_parts = 3;
    auto partition = gko::distributed::Partition<lit, git>::build_from_mapping(
        this->ref, row_mapping, num_parts);
    gko::array<comm_index_type> col_mapping{this->ref, {0, 2, 2, 1}};
    auto col_partition =
        gko::distributed::Partition<lit, git>::build_from_mapping(
            this->ref, col_mapping, num_parts);

    this->validate(
        gko::dim<2>{6, 4}, partition.get(), col_partition.get(),
        {2, 3, 0, 1, 4, 3, 3, 0, 1, 4}, {0, 0, 3, 2, 1, 2, 3, 0, 3, 3},
        {1, 2, 3, 4, 5, 6, 7, 8, 9, 10},
        {std::make_tuple(gko::dim<2>{2, 1}, I<git>{0, 1}, I<git>{0, 0},
                         I<vt>{1, 2}),
         std::make_tuple(gko::dim<2>{2, 1}, I<git>{0}, I<git>{0}, I<vt>{3}),
         std::make_tuple(gko::dim<2>{2, 2}, I<git>{0, 1}, I<git>{1, 0},
                         I<vt>{4, 5})},
        {std::make_tuple(gko::dim<2>{2, 2}, I<git>{1, 1}, I<git>{1, 0},
                         I<vt>{6, 7}),
         std::make_tuple(gko::dim<2>{2, 1}, I<git>{0}, I<git>{0}, I<vt>{8}),
         std::make_tuple(gko::dim<2>{2, 1}, I<git>{0, 1}, I<git>{0, 0},
                         I<vt>{9, 10})},
        {{0, 1}, {0}, {0}}, {{0, 0, 1, 2}, {0, 1, 1, 1}, {0, 0, 1, 1}});
}


TYPED_TEST(Matrix, BuildGhostMapContinuous)
{
    using value_type = typename TestFixture::value_type;
    using local_index_type = typename TestFixture::local_index_type;
    using global_index_type = typename TestFixture::global_index_type;
    this->mapping = {this->ref, {0, 0, 0, 1, 1, 2, 2}};
    constexpr comm_index_type num_parts = 3;
    auto partition = gko::distributed::Partition<
        local_index_type, global_index_type>::build_from_mapping(this->ref,
                                                                 this->mapping,
                                                                 num_parts);
    this->recv_offsets.resize_and_reset(num_parts + 1);
    gko::array<global_index_type> result[num_parts] = {
        {this->ref, {3}}, {this->ref, {0, 6}}, {this->ref, {4}}};

    for (int local_id = 0; local_id < num_parts; ++local_id) {
        gko::kernels::reference::distributed_matrix::build_diag_offdiag(
            this->ref, this->create_input_full_rank(), partition.get(),
            partition.get(), local_id, this->diag_row_idxs, this->diag_col_idxs,
            this->diag_values, this->offdiag_row_idxs, this->offdiag_col_idxs,
            this->offdiag_values, this->gather_idxs,
            this->recv_offsets.get_data(), this->local_to_global_ghost);

        GKO_ASSERT_ARRAY_EQ(result[local_id], this->local_to_global_ghost);
    }
}

TYPED_TEST(Matrix, BuildGhostMapScattered)
{
    using value_type = typename TestFixture::value_type;
    using local_index_type = typename TestFixture::local_index_type;
    using global_index_type = typename TestFixture::global_index_type;
    this->mapping = {this->ref, {0, 1, 2, 0, 1, 2, 0}};
    constexpr comm_index_type num_parts = 3;
    auto partition = gko::distributed::Partition<
        local_index_type, global_index_type>::build_from_mapping(this->ref,
                                                                 this->mapping,
                                                                 num_parts);
    this->recv_offsets.resize_and_reset(num_parts + 1);
    gko::array<global_index_type> result[num_parts] = {
        {this->ref, {5}},
        {this->ref, {6, 2}},
        {this->ref, {4}}};  // the columns are sorted by their part_id

    for (int local_id = 0; local_id < num_parts; ++local_id) {
        gko::kernels::reference::distributed_matrix::build_diag_offdiag(
            this->ref, this->create_input_full_rank(), partition.get(),
            partition.get(), local_id, this->diag_row_idxs, this->diag_col_idxs,
            this->diag_values, this->offdiag_row_idxs, this->offdiag_col_idxs,
            this->offdiag_values, this->gather_idxs,
            this->recv_offsets.get_data(), this->local_to_global_ghost);

        GKO_ASSERT_ARRAY_EQ(result[local_id], this->local_to_global_ghost);
    }
}

}  // namespace
