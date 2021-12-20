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

#include <ginkgo/core/distributed/matrix.hpp>


#include <ginkgo/core/distributed/vector.hpp>
#include <ginkgo/core/matrix/csr.hpp>
#include <ginkgo/core/matrix/diagonal.hpp>
#include <utility>


#include "core/distributed/matrix_kernels.hpp"


namespace gko {
namespace distributed {
namespace matrix {
GKO_REGISTER_OPERATION(build_diag_offdiag,
                       distributed_matrix::build_diag_offdiag);
GKO_REGISTER_OPERATION(map_to_global_idxs,
                       distributed_matrix::map_to_global_idxs);
GKO_REGISTER_OPERATION(merge_diag_offdiag,
                       distributed_matrix::merge_diag_offdiag);
GKO_REGISTER_OPERATION(combine_local_mtxs,
                       distributed_matrix::combine_local_mtxs);
GKO_REGISTER_OPERATION(check_column_index_exists,
                       distributed_matrix::check_column_index_exists);
GKO_REGISTER_OPERATION(build_recv_sizes, distributed_matrix::build_recv_sizes);
GKO_REGISTER_OPERATION(compress_offdiag_data,
                       distributed_matrix::compress_offdiag_data);
GKO_REGISTER_OPERATION(map_local_col_idxs_to_span,
                       distributed_matrix::map_local_col_idxs_to_span);
GKO_REGISTER_OPERATION(zero_out_invalid_columns,
                       distributed_matrix::zero_out_invalid_columns);
GKO_REGISTER_OPERATION(add_to_array, distributed_matrix::add_to_array);
}  // namespace matrix


template <typename ValueType, typename LocalIndexType>
Matrix<ValueType, LocalIndexType>::Matrix(
    std::shared_ptr<const Executor> exec,
    std::shared_ptr<mpi::communicator> comm)
    : EnableLinOp<Matrix<value_type, local_index_type>>{exec},
      DistributedBase{comm},
      send_offsets_(comm->size() + 1),
      send_sizes_(comm->size()),
      recv_offsets_(comm->size() + 1),
      recv_sizes_(comm->size()),
      gather_idxs_{exec},
      local_to_global_row{exec},
      local_to_global_offdiag_col{exec},
      one_scalar_{exec, dim<2>{1, 1}},
      diag_mtx_{exec},
      offdiag_mtx_{exec}
{
    auto one_val = one<ValueType>();
    exec->copy_from(exec->get_master().get(), 1, &one_val,
                    one_scalar_.get_values());
}


template <typename ValueType, typename LocalIndexType>
void Matrix<ValueType, LocalIndexType>::read_distributed(
    const matrix_data<ValueType, global_index_type>& data,
    std::shared_ptr<const Partition<LocalIndexType>> partition)
{
    this->read_distributed(
        Array<matrix_data_entry<ValueType, global_index_type>>::view(
            this->get_executor()->get_master(), data.nonzeros.size(),
            const_cast<matrix_data_entry<ValueType, global_index_type>*>(
                data.nonzeros.data())),
        data.size, partition);
}


template <typename ValueType, typename LocalIndexType>
void Matrix<ValueType, LocalIndexType>::read_distributed(
    const Array<matrix_data_entry<ValueType, global_index_type>>& data,
    dim<2> size, std::shared_ptr<const Partition<LocalIndexType>> partition)
{
    this->partition_ = partition;
    const auto comm = this->get_communicator();
    // GKO_ASSERT_IS_SQUARE_MATRIX(size);
    GKO_ASSERT(size[0] <= partition->get_size());
    GKO_ASSERT(size[1] <= partition->get_size());
    GKO_ASSERT_EQ(comm->size(), partition->get_num_parts());
    using nonzero_type = matrix_data_entry<ValueType, LocalIndexType>;
    auto exec = this->get_executor();
    auto local_data = make_temporary_clone(exec, &data);
    auto local_part = comm->rank();

    // set up LinOp sizes
    auto num_parts = static_cast<size_type>(partition->get_num_parts());
    auto global_size = partition->get_size();
    auto local_size =
        static_cast<size_type>(partition->get_part_size(local_part));
    dim<2> global_dim{global_size, global_size};
    dim<2> diag_dim{local_size, local_size};
    this->set_size(global_dim);

    // temporary storage for the output
    Array<nonzero_type> diag_data{exec};
    Array<nonzero_type> offdiag_data{exec};
    Array<local_index_type> recv_gather_idxs{exec};
    Array<comm_index_type> recv_offsets_array{exec, num_parts + 1};

    // build diagonal, off-diagonal matrix and communication structures
    exec->run(matrix::make_build_diag_offdiag(
        *local_data, partition.get(), local_part, diag_data, offdiag_data,
        recv_gather_idxs, recv_offsets_array.get_data(), local_to_global_row,
        local_to_global_offdiag_col, ValueType{}));

    dim<2> offdiag_dim{local_size, recv_gather_idxs.get_num_elems()};
    this->diag_mtx_.read(diag_data, diag_dim);
    this->offdiag_mtx_.read(offdiag_data, offdiag_dim);

    // exchange step 1: determine recv_sizes, send_sizes, send_offsets
    exec->get_master()->copy_from(exec.get(), num_parts + 1,
                                  recv_offsets_array.get_data(),
                                  recv_offsets_.data());
    // TODO clean this up a bit
    for (size_type i = 0; i < num_parts; i++) {
        recv_sizes_[i] = recv_offsets_[i + 1] - recv_offsets_[i];
    }
    mpi::all_to_all(recv_sizes_.data(), 1, send_sizes_.data(), 1,
                    this->get_communicator());
    std::partial_sum(send_sizes_.begin(), send_sizes_.end(),
                     send_offsets_.begin() + 1);
    send_offsets_[0] = 0;

    // exchange step 2: exchange gather_idxs from receivers to senders
    auto use_host_buffer =
        exec->get_master() != exec /* || comm.is_gpu_aware() */;
    if (use_host_buffer) {
        recv_gather_idxs.set_executor(exec->get_master());
        gather_idxs_.clear();
        gather_idxs_.set_executor(exec->get_master());
    }
    gather_idxs_.resize_and_reset(send_offsets_.back());
    mpi::all_to_all(recv_gather_idxs.get_const_data(), recv_sizes_.data(),
                    recv_offsets_.data(), gather_idxs_.get_data(),
                    send_sizes_.data(), send_offsets_.data(), 1,
                    this->get_communicator());
    if (use_host_buffer) {
        gather_idxs_.set_executor(exec);
    }
}


template <typename ValueType, typename LocalIndexType>
void Matrix<ValueType, LocalIndexType>::communicate(
    const LocalVec* local_b) const
{
    auto exec = this->get_executor();
    const auto comm = this->get_communicator();
    auto num_cols = local_b->get_size()[1];
    auto send_size = send_offsets_.back();
    auto recv_size = recv_offsets_.back();
    auto send_dim = dim<2>{static_cast<size_type>(send_size), num_cols};
    auto recv_dim = dim<2>{static_cast<size_type>(recv_size), num_cols};
    recv_buffer_.init(exec, recv_dim);
    send_buffer_.init(exec, send_dim);
    auto use_host_buffer =
        exec->get_master() != exec /* || comm->is_gpu_aware() */;
    if (use_host_buffer) {
        host_recv_buffer_.init(exec->get_master(), recv_dim);
        host_send_buffer_.init(exec->get_master(), send_dim);
    }
    local_b->row_gather(&gather_idxs_, send_buffer_.get());
    if (use_host_buffer) {
        host_send_buffer_->copy_from(send_buffer_.get());
        mpi::all_to_all(host_send_buffer_->get_const_values(),
                        send_sizes_.data(), send_offsets_.data(),
                        host_recv_buffer_->get_values(), recv_sizes_.data(),
                        recv_offsets_.data(), num_cols,
                        this->get_communicator());
        recv_buffer_->copy_from(host_recv_buffer_.get());
    } else {
        mpi::all_to_all(send_buffer_->get_const_values(), send_sizes_.data(),
                        send_offsets_.data(), recv_buffer_->get_values(),
                        recv_sizes_.data(), recv_offsets_.data(), num_cols,
                        this->get_communicator());
    }
}


template <typename ValueType, typename LocalIndexType>
void Matrix<ValueType, LocalIndexType>::apply_impl(const LinOp* b,
                                                   LinOp* x) const
{
    auto dense_b = as<GlobalVec>(b);
    auto dense_x = as<GlobalVec>(x);
    diag_mtx_.apply(dense_b->get_local(), dense_x->get_local());
    if (offdiag_mtx_.get_size()) {
        this->communicate(dense_b->get_local());
        offdiag_mtx_.apply(&one_scalar_, recv_buffer_.get(), &one_scalar_,
                           dense_x->get_local());
    }
}


template <typename ValueType, typename LocalIndexType>
void Matrix<ValueType, LocalIndexType>::apply_impl(const LinOp* alpha,
                                                   const LinOp* b,
                                                   const LinOp* beta,
                                                   LinOp* x) const
{
    auto vec_b = as<GlobalVec>(b);
    auto vec_x = as<GlobalVec>(x);
    auto local_alpha = as<LocalVec>(alpha);
    auto local_beta = as<LocalVec>(beta);
    diag_mtx_.apply(local_alpha, vec_b->get_local(), local_beta,
                    vec_x->get_local());
    if (offdiag_mtx_.get_size()) {
        this->communicate(vec_b->get_local());
        offdiag_mtx_.apply(local_alpha, recv_buffer_.get(), &one_scalar_,
                           vec_x->get_local());
    }
}


template <typename ValueType, typename LocalIndexType>
void Matrix<ValueType, LocalIndexType>::validate_data() const
{
    LinOp::validate_data();
    one_scalar_.validate_data();
    diag_mtx_.validate_data();
    offdiag_mtx_.validate_data();
    const auto exec = this->get_executor();
    const auto host_exec = exec->get_master();
    const auto comm = this->get_communicator();
    // executors
    GKO_VALIDATION_CHECK(one_scalar_.get_executor() == exec);
    GKO_VALIDATION_CHECK(diag_mtx_.get_executor() == exec);
    GKO_VALIDATION_CHECK(offdiag_mtx_.get_executor() == exec);
    GKO_VALIDATION_CHECK(gather_idxs_.get_executor() == exec);
    GKO_VALIDATION_CHECK(host_send_buffer_.get() == nullptr ||
                         host_send_buffer_->get_executor() == host_exec);
    GKO_VALIDATION_CHECK(host_recv_buffer_.get() == nullptr ||
                         host_recv_buffer_->get_executor() == host_exec);
    GKO_VALIDATION_CHECK(send_buffer_.get() == nullptr ||
                         send_buffer_->get_executor() == exec);
    GKO_VALIDATION_CHECK(recv_buffer_.get() == nullptr ||
                         recv_buffer_->get_executor() == exec);
    // sizes are matching
    const auto num_local_rows = diag_mtx_.get_size()[0];
    const auto num_offdiag_cols = offdiag_mtx_.get_size()[1];
    const auto num_gather_rows = gather_idxs_.get_num_elems();
    GKO_VALIDATION_CHECK(num_local_rows == diag_mtx_.get_size()[1]);
    GKO_VALIDATION_CHECK(num_local_rows == offdiag_mtx_.get_size()[0]);
    auto num_local_rows_sum = diag_mtx_.get_size()[0];
    mpi::all_reduce(&num_local_rows_sum, 1, mpi::op_type::sum,
                    this->get_communicator());
    GKO_VALIDATION_CHECK(num_local_rows_sum == this->get_size()[0]);
    const auto num_parts = comm->rank();
    GKO_VALIDATION_CHECK(num_parts == send_sizes_.size());
    GKO_VALIDATION_CHECK(num_parts == recv_sizes_.size());
    GKO_VALIDATION_CHECK(num_parts + 1 == send_offsets_.size());
    GKO_VALIDATION_CHECK(num_parts + 1 == recv_offsets_.size());
    // communication data structures are consistent
    auto send_copy = send_sizes_;
    auto recv_copy = recv_sizes_;
    for (comm_index_type i = 0; i < num_parts; i++) {
        GKO_VALIDATION_CHECK(send_sizes_[i] ==
                             send_offsets_[i + 1] - send_offsets_[i]);
        GKO_VALIDATION_CHECK(recv_sizes_[i] ==
                             recv_offsets_[i + 1] - recv_offsets_[i]);
    }
    mpi::all_to_all(send_copy.data(), 1, this->get_communicator());
    mpi::all_to_all(recv_copy.data(), 1, this->get_communicator());
    GKO_VALIDATION_CHECK(send_copy == recv_sizes_);
    GKO_VALIDATION_CHECK(recv_copy == send_sizes_);
    // gather indices are in bounds
    Array<local_index_type> host_gather_idxs(host_exec, gather_idxs_);
    const auto host_gather_idx_ptr = host_gather_idxs.get_const_data();
    GKO_VALIDATION_CHECK_NAMED(
        "gather indices need to be in range",
        std::all_of(
            host_gather_idx_ptr, host_gather_idx_ptr + num_gather_rows,
            [&](auto row) { return row >= 0 && row < num_local_rows; }));
}


template <typename ValueType, typename SourceIndexType,
          typename TargetIndexType>
std::unique_ptr<gko::matrix::Csr<ValueType, TargetIndexType>>
convert_csr_index_type(
    const gko::matrix::Csr<ValueType, SourceIndexType>* source,
    TargetIndexType deduction_helper)
{
    auto exec = source->get_executor();
    gko::Array<TargetIndexType> row_ptrs;
    gko::Array<TargetIndexType> col_idxs;
    gko::Array<ValueType> values;

    row_ptrs = gko::Array<SourceIndexType>::view(
        exec, source->get_size()[0] + 1,
        const_cast<SourceIndexType*>(source->get_const_row_ptrs()));
    col_idxs = gko::Array<SourceIndexType>::view(
        exec, source->get_num_stored_elements(),
        const_cast<SourceIndexType*>(source->get_const_col_idxs()));
    values = gko::Array<ValueType>::view(
        exec, source->get_num_stored_elements(),
        const_cast<ValueType*>(source->get_const_values()));
    return gko::matrix::Csr<ValueType, TargetIndexType>::create(
        exec, source->get_size(), values, col_idxs, row_ptrs);
}


template <typename LocalIndexType>
void gather_contiguous_rows(
    std::shared_ptr<const Executor> exec, const LocalIndexType* local_row_ptrs,
    size_type local_num_rows, LocalIndexType* global_row_ptrs,
    size_type global_num_rows,
    std::shared_ptr<const Partition<LocalIndexType>> part,
    std::shared_ptr<const mpi::communicator> comm)
{
    std::vector<comm_index_type> local_row_counts(part->get_num_parts());
    std::vector<comm_index_type> local_row_offsets(local_row_counts.size() + 1,
                                                   0);

    gko::Array<comm_index_type> part_sizes;
    part_sizes = gko::Array<LocalIndexType>::view(
        part->get_executor(), part->get_num_parts(),
        const_cast<LocalIndexType*>(part->get_part_sizes()));
    exec->get_master()->copy_from(exec.get(), local_row_counts.size(),
                                  part_sizes.get_data(),
                                  local_row_counts.data());
    std::partial_sum(local_row_counts.begin(), local_row_counts.end(),
                     local_row_offsets.begin() + 1);

    if (comm->rank() == 0) {
        Array<LocalIndexType>::view(exec, global_num_rows + 1, global_row_ptrs)
            .fill(0);
    }
    mpi::gather(local_row_ptrs + 1, local_num_rows, global_row_ptrs + 1,
                local_row_counts.data(), local_row_offsets.data(), 0, comm);
    if (comm->rank() == 0) {
        comm_index_type row = 1;
        for (comm_index_type pid = 0; pid < part->get_num_parts(); ++pid) {
            comm_index_type global_part_offset =
                static_cast<comm_index_type>(global_row_ptrs[row - 1]);
            for (comm_index_type j = 0; j < part->get_part_size(pid); ++j) {
                global_row_ptrs[row] += global_part_offset;
                row++;
            }
        }
    }
}


template <typename ValueType, typename LocalIndexType>
void Matrix<ValueType, LocalIndexType>::convert_to(
    gko::matrix::Csr<ValueType, LocalIndexType>* result) const
{
    using GMtx = gko::matrix::Csr<ValueType, LocalIndexType>;
    // already have total size
    auto exec = this->get_executor();

    dim<2> local_size{this->get_local_diag()->get_size()[0],
                      this->get_size()[1]};
    auto diag_nnz = this->get_local_diag()->get_num_stored_elements();
    auto offdiag_nnz = this->get_local_offdiag()->get_num_stored_elements();
    auto local_nnz = diag_nnz + offdiag_nnz;

    auto mapped_diag =
        convert_csr_index_type(this->get_local_diag(), global_index_type{});
    auto mapped_offdiag =
        convert_csr_index_type(this->get_local_offdiag(), global_index_type{});

    exec->run(matrix::make_map_to_global_idxs(
        this->get_local_diag()->get_const_col_idxs(), diag_nnz,
        mapped_diag->get_col_idxs(),
        this->local_to_global_row.get_const_data()));
    exec->run(matrix::make_map_to_global_idxs(
        this->get_local_offdiag()->get_const_col_idxs(), offdiag_nnz,
        mapped_offdiag->get_col_idxs(),
        this->local_to_global_offdiag_col.get_const_data()));
    mapped_diag->sort_by_column_index();
    mapped_offdiag->sort_by_column_index();

    auto merged_local = GMtx::create(exec, local_size, local_nnz);
    exec->run(matrix::make_merge_diag_offdiag(
        convert_csr_index_type(mapped_diag.get(), LocalIndexType{}).get(),
        convert_csr_index_type(mapped_offdiag.get(), LocalIndexType{}).get(),
        merged_local.get()));

    // build gather counts + offsets
    auto comm = this->get_communicator();
    auto rank = comm->rank();
    auto local_count = static_cast<comm_index_type>(local_nnz);
    std::vector<comm_index_type> recv_counts(rank == 0 ? comm->size() : 0, 0);
    mpi::gather(&local_count, 1, recv_counts.data(), 1, 0, comm);
    std::vector<comm_index_type> recv_offsets(recv_counts.size() + 1, 0);
    std::partial_sum(recv_counts.begin(), recv_counts.end(),
                     recv_offsets.begin() + 1);
    auto global_nnz = static_cast<size_type>(recv_offsets.back());

    auto tmp = rank == 0 ? GMtx::create(exec, this->get_size(), global_nnz)
                         : GMtx::create(exec);

    auto global_row_ptrs = tmp->get_row_ptrs();
    auto global_col_idxs = tmp->get_col_idxs();
    auto global_values = tmp->get_values();

    gather_contiguous_rows(exec, merged_local->get_const_row_ptrs(),
                           merged_local->get_size()[0], global_row_ptrs,
                           tmp->get_size()[0], this->partition_,
                           this->get_communicator());
    mpi::gather(merged_local->get_const_col_idxs(), local_nnz, global_col_idxs,
                recv_counts.data(), recv_offsets.data(), 0, comm);
    mpi::gather(merged_local->get_const_values(), local_nnz, global_values,
                recv_counts.data(), recv_offsets.data(), 0, comm);

    if (rank != 0 || is_ordered(partition_.get())) {
        tmp->move_to(result);
    } else {
        auto row_permutation = partition_->get_block_gather_permutation();
        gko::as<GMtx>(tmp->row_permute(&row_permutation))->move_to(result);
    }
}

template <typename ValueType, typename LocalIndexType>
void Matrix<ValueType, LocalIndexType>::move_to(
    gko::matrix::Csr<ValueType, LocalIndexType>* result) GKO_NOT_IMPLEMENTED;


template <typename ValueType, typename LocalIndexType>
std::unique_ptr<gko::matrix::Diagonal<ValueType>>
Matrix<ValueType, LocalIndexType>::extract_diagonal() const
{
    return diag_mtx_.extract_diagonal();
}


template <typename FirstIt, typename SecondIt, typename OutputIt, typename Cmp>
void merge_sorted(FirstIt begin_first, FirstIt end_first, SecondIt begin_second,
                  SecondIt end_second, OutputIt out, Cmp&& cmp)
{
    FirstIt it_first = begin_first;
    SecondIt it_second = begin_second;

    while (it_first != end_first || it_second != end_second) {
        if (it_first == end_first)
            *out++ = *it_second++;
        else if (it_second == end_second)
            *out++ = *it_first++;
        else
            *out++ = cmp(*it_first, *it_second) ? *it_first++ : *it_second++;
    }
}


template <typename ValueType, typename LocalIndexType>
void Matrix<ValueType, LocalIndexType>::write_local(
    matrix_data<ValueType, global_index_type>& data) const
{
    using md_local = matrix_data<ValueType, LocalIndexType>;
    using md_global = matrix_data<ValueType, global_index_type>;
    auto exec = this->get_executor();

    md_local diag_md;
    md_local offdiag_md;
    this->get_local_diag()->write(diag_md);
    this->get_local_offdiag()->write(offdiag_md);

    md_global global_diag_md(this->get_size());
    md_global global_offdiag_md(this->get_size());

    Array<global_index_type> host_local_to_global_row{exec,
                                                      local_to_global_row};
    Array<global_index_type> host_local_to_global_col{
        exec, local_to_global_offdiag_col};

    auto map_row = [&](const auto i) {
        return host_local_to_global_row.get_const_data()[i];
    };
    auto map_col = [&](const auto i) {
        return host_local_to_global_col.get_const_data()[i];
    };

    std::transform(diag_md.nonzeros.cbegin(), diag_md.nonzeros.cend(),
                   std::back_inserter(global_diag_md.nonzeros), [&](auto nnz) {
                       return typename md_global::nonzero_type{
                           map_row(nnz.row), map_row(nnz.column), nnz.value};
                   });

    std::transform(offdiag_md.nonzeros.cbegin(), offdiag_md.nonzeros.cend(),
                   std::back_inserter(global_offdiag_md.nonzeros),
                   [&](auto nnz) {
                       return typename md_global::nonzero_type{
                           map_row(nnz.row), map_col(nnz.column), nnz.value};
                   });

    md_global tmp(this->get_size());
    tmp.nonzeros.resize(global_diag_md.nonzeros.size() +
                        global_offdiag_md.nonzeros.size());
    merge_sorted(
        global_diag_md.nonzeros.cbegin(), global_diag_md.nonzeros.cend(),
        global_offdiag_md.nonzeros.cbegin(), global_offdiag_md.nonzeros.cend(),
        tmp.nonzeros.begin(), [](const auto& a, const auto& b) {
            return std::tie(a.row, a.column) < std::tie(b.row, b.column);
        });
    data = std::move(tmp);
}


template <typename ValueType, typename LocalIndexType>
auto create_submatrix_diag(
    const gko::matrix::Csr<ValueType, LocalIndexType>* mat,
    const gko::span rows, const gko::span columns,
    const global_index_type* map_to_global)
{
    auto exec = mat->get_executor();
    auto global_starting_row = exec->copy_val_to_host(&map_to_global[0]);
    gko::span local_rows{rows.begin - global_starting_row,
                         rows.end - global_starting_row};
    gko::span local_cols{columns.begin - global_starting_row,
                         columns.end - global_starting_row};

    return mat->create_submatrix(local_rows, local_cols);
}


template <typename ValueType, typename LocalIndexType>
auto create_submatrix_offdiag(
    const gko::matrix::Csr<ValueType, LocalIndexType>* mat,
    const gko::span rows, const global_index_type* map_to_global)
{
    auto exec = mat->get_executor();
    auto global_starting_row = exec->copy_val_to_host(&map_to_global[0]);
    gko::span local_rows{rows.begin - global_starting_row,
                         rows.end - global_starting_row};
    gko::span local_cols{0, rows.length() == 0 ? 0 : mat->get_size()[1]};

    return mat->create_submatrix(local_rows, local_cols);
}


gko::Array<global_index_type> update_diag_map(
    const Array<global_index_type>& old_inner, gko::span rows,
    std::shared_ptr<const mpi::communicator> comm)
{
    auto exec = old_inner.get_executor();
    auto global_starting_row =
        exec->copy_val_to_host(&old_inner.get_const_data()[0]);
    Array<global_index_type> new_inner{exec, rows.length()};
    exec->copy(rows.length(),
               old_inner.get_const_data() + rows.begin - global_starting_row,
               new_inner.get_data());

    // TODO: this only works for compact partitions
    auto local_size = rows.length();
    auto local_offset = zero(local_size);
    mpi::exscan(&local_size, &local_offset, 1, mpi::op_type::sum, comm);
    exec->run(matrix::make_add_to_array(
        new_inner,
        -static_cast<global_index_type>(global_starting_row - local_offset +
                                        rows.begin - global_starting_row)));

    return new_inner;
}


template <typename LocalIndexType>
Array<global_index_type> communicate_invalid_columns(
    const Array<LocalIndexType>& send_indices,
    const Partition<LocalIndexType>* col_part, const gko::span valid_columns,
    const Array<global_index_type>& to_global,
    const std::vector<comm_index_type>& send_offsets,
    const std::vector<comm_index_type>& send_sizes,
    const std::vector<comm_index_type>& recv_offsets,
    const std::vector<comm_index_type>& recv_sizes,
    std::shared_ptr<const mpi::communicator> comm)
{
    // invalidate entries outside of span
    auto exec = send_indices.get_executor();

    auto offset = col_part->get_global_offset(comm->rank());
    gko::Array<global_index_type> local_col_map{send_indices.get_executor(),
                                                send_indices.get_num_elems()};
    exec->run(matrix::make_map_local_col_idxs_to_span(
        send_indices, to_global, valid_columns, offset, local_col_map));

    gko::Array<global_index_type> remote_col_map{
        exec, static_cast<size_type>(recv_offsets.back())};
    mpi::all_to_all(local_col_map.get_data(), send_sizes.data(),
                    send_offsets.data(), remote_col_map.get_data(),
                    recv_sizes.data(), recv_offsets.data(), 1, std::move(comm));
    return remote_col_map;
}

// TODO: only works for compact partitions
template <typename IndexType>
std::shared_ptr<Partition<IndexType>> build_sub_partition(
    std::shared_ptr<const Partition<IndexType>> part, gko::span rows,
    std::shared_ptr<const mpi::communicator> comm)
{
    auto exec = part->get_executor();

    auto num_global_rows = rows.length();
    mpi::all_reduce(&num_global_rows, 1, mpi::op_type::sum, comm);

    auto local_size = static_cast<comm_index_type>(rows.length());
    auto local_offset = zero(local_size);
    mpi::exscan(&local_size, &local_offset, 1, mpi::op_type::sum, comm);

    Array<comm_index_type> sub_mapping{exec, num_global_rows};
    auto local_sub_mapping = Array<comm_index_type>::view(
        exec, local_size, sub_mapping.get_data() + local_offset);
    local_sub_mapping.fill(comm->rank());

    std::vector<comm_index_type> recv_sizes(comm->size(), local_size);
    std::vector<comm_index_type> recv_offsets(comm->size() + 1, 0);
    mpi::all_gather(&local_size, 1, recv_sizes.data(), 1, comm);
    std::partial_sum(recv_sizes.begin(), recv_sizes.end(),
                     recv_offsets.begin() + 1);

    Array<comm_index_type> local_sub_mapping_copy(
        exec, local_sub_mapping.get_const_data(),
        local_sub_mapping.get_const_data() + local_size);
    mpi::all_gather(local_sub_mapping_copy.get_data(), local_size,
                    sub_mapping.get_data(), recv_sizes.data(),
                    recv_offsets.data(), comm);

    return Partition<IndexType>::build_from_mapping(exec, sub_mapping,
                                                    comm->size());
}


template <typename ValueType, typename LocalIndexType>
std::unique_ptr<LinOp> Matrix<ValueType, LocalIndexType>::create_submatrix_impl(
    const gko::span& rows, const gko::span& columns) const
{
    auto exec = this->get_executor();
    auto comm = this->get_communicator();

    if (!is_ordered(this->get_partition())) {
        GKO_NOT_IMPLEMENTED;
    }

    auto sub_row_partition = build_sub_partition(this->partition_, rows, comm);
    auto sub_col_partition =
        build_sub_partition(this->partition_, columns, comm);

    // create the submatrix, by
    // 1. extracting the submatrices from the diag/off-diag matrices
    // 2. adapting the off-diag submatrix
    // 3. rebuild communication info
    // An alternative way could be:
    // 2'. write submatrices to matrix data
    // 3'. combine matrix data and map to global indices
    // 4'. read distributed with matrix data (this would require new partitions
    // + handling of non-square matrices)

    auto sub_diag =
        create_submatrix_diag(this->get_local_diag(), rows, columns,
                              this->local_to_global_row.get_const_data());

    auto sub_offdiag =
        create_submatrix_offdiag(this->get_local_offdiag(), rows,
                                 this->local_to_global_row.get_const_data());

    // set entries from other processes to zero, if they are not in their span
    device_matrix_data<ValueType, LocalIndexType> md{exec};
    sub_offdiag->write(md);
    md.remove_zeros();
    // basically collect all idx s.t. col_id_valid[idx] = true
    // and then md[idx].value = 0
    auto new_inner_map = update_diag_map(this->local_to_global_row, rows, comm);
    auto new_ghost_map = communicate_invalid_columns(
        this->gather_idxs_, sub_col_partition.get(), columns,
        this->local_to_global_row, this->send_offsets_, this->send_sizes_,
        this->recv_offsets_, this->recv_sizes_, this->get_communicator());
    exec->run(matrix::make_compress_offdiag_data(md, new_ghost_map));
    sub_offdiag->read(md);

    // update recv communication info
    Array<comm_index_type> recv_sizes_array{exec};
    Array<comm_index_type> recv_offsets_array{exec};
    Array<LocalIndexType> recv_indices{exec};
    exec->run(matrix::make_build_recv_sizes(
        sub_offdiag->get_const_col_idxs(),
        sub_offdiag->get_num_stored_elements(), sub_col_partition.get(),
        new_ghost_map.get_const_data(), recv_sizes_array, recv_offsets_array,
        recv_indices));
    std::vector<comm_index_type> recv_sizes(recv_sizes_array.get_num_elems());
    std::vector<comm_index_type> recv_offsets(
        recv_offsets_array.get_num_elems());
    exec->get_master()->copy_from(exec.get(), recv_sizes_array.get_num_elems(),
                                  recv_sizes_array.get_const_data(),
                                  recv_sizes.data());
    exec->get_master()->copy_from(
        exec.get(), recv_offsets_array.get_num_elems(),
        recv_offsets_array.get_const_data(), recv_offsets.data());

    // update send communication info
    std::vector<comm_index_type> send_sizes(recv_sizes.size());
    std::vector<comm_index_type> send_offsets(recv_sizes.size() + 1);
    mpi::all_to_all(recv_sizes.data(), 1, send_sizes.data(), 1, comm);
    std::partial_sum(begin(send_sizes), end(send_sizes),
                     begin(send_offsets) + 1);
    Array<LocalIndexType> send_indices{
        exec, static_cast<size_type>(send_offsets.back())};
    if (exec->get_master() != exec) {
        recv_indices.set_executor(exec->get_master());
        send_indices.set_executor(exec->get_master());
    }
    mpi::all_to_all(recv_indices.get_data(), recv_sizes.data(),
                    recv_offsets.data(), send_indices.get_data(),
                    send_sizes.data(), send_offsets.data(), 1,
                    this->get_communicator());
    if (exec->get_master() != exec) {
        recv_indices.set_executor(exec);
        send_indices.set_executor(exec);
    }
    // re-map send_indicies
    // currently they are local w.r.t the full local matrix
    // need to constrain them to the column span
    //    auto global_starting_row =
    //        exec->copy_val_to_host(&local_to_global_row.get_const_data()[0]);
    //    exec->run(matrix::make_add_to_array(
    //        send_indices,
    //        -static_cast<LocalIndexType>(columns.begin -
    //        global_starting_row)));

    auto sub = Matrix<ValueType, LocalIndexType>::create(
        exec, this->get_communicator());

    auto num_global_rows = rows.length();
    auto num_global_cols = columns.length();
    mpi::all_reduce(&num_global_rows, 1, mpi::op_type::sum, comm);
    mpi::all_reduce(&num_global_cols, 1, mpi::op_type::sum, comm);
    sub->set_size(gko::dim<2>{num_global_rows, num_global_cols});
    sub->send_offsets_ = std::move(send_offsets);
    sub->send_sizes_ = std::move(send_sizes);
    sub->recv_offsets_ = std::move(recv_offsets);
    sub->recv_sizes_ = std::move(recv_sizes);
    sub->gather_idxs_ = std::move(send_indices);
    sub->local_to_global_row = std::move(new_inner_map);
    sub->local_to_global_offdiag_col = std::move(new_ghost_map);
    sub->diag_mtx_ = std::move(*sub_diag.get());
    sub->offdiag_mtx_ = std::move(*sub_offdiag.get());
    sub->partition_ = std::move(sub_row_partition);

    return sub;
}


#define GKO_DECLARE_DISTRIBUTED_MATRIX(ValueType, LocalIndexType) \
    class Matrix<ValueType, LocalIndexType>
GKO_INSTANTIATE_FOR_EACH_VALUE_AND_INDEX_TYPE(GKO_DECLARE_DISTRIBUTED_MATRIX);


}  // namespace distributed
}  // namespace gko
