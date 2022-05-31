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

#include <ginkgo/core/distributed/matrix.hpp>


#include <ginkgo/core/base/precision_dispatch.hpp>
#include <ginkgo/core/distributed/vector.hpp>
#include <ginkgo/core/matrix/csr.hpp>


#include "core/distributed/matrix_kernels.hpp"


namespace gko {
namespace distributed {
namespace matrix {
namespace {


GKO_REGISTER_OPERATION(build_diag_offdiag,
                       distributed_matrix::build_diag_offdiag);


}  // namespace
}  // namespace matrix


template <typename ValueType, typename LocalIndexType, typename GlobalIndexType>
Matrix<ValueType, LocalIndexType, GlobalIndexType>::Matrix(
    std::shared_ptr<const Executor> exec)
    : Matrix(exec, mpi::communicator(MPI_COMM_WORLD, exec))
{}


template <typename ValueType, typename LocalIndexType, typename GlobalIndexType>
Matrix<ValueType, LocalIndexType, GlobalIndexType>::Matrix(
    std::shared_ptr<const Executor> exec, mpi::communicator comm)
    : Matrix(exec, comm, with_matrix_type<gko::matrix::Csr>())
{}


template <typename ValueType, typename LocalIndexType, typename GlobalIndexType>
Matrix<ValueType, LocalIndexType, GlobalIndexType>::Matrix(
    std::shared_ptr<const Executor> exec, mpi::communicator comm,
    const LinOp* inner_matrix_type)
    : Matrix(exec, comm, inner_matrix_type, inner_matrix_type)
{}


template <typename ValueType, typename LocalIndexType, typename GlobalIndexType>
Matrix<ValueType, LocalIndexType, GlobalIndexType>::Matrix(
    std::shared_ptr<const Executor> exec, mpi::communicator comm,
    const LinOp* inner_matrix_type, const LinOp* ghost_matrix_type)
    : EnableLinOp<
          Matrix<value_type, local_index_type, global_index_type>>{exec},
      DistributedBase{comm},
      send_offsets_(comm.size() + 1),
      send_sizes_(comm.size()),
      recv_offsets_(comm.size() + 1),
      recv_sizes_(comm.size()),
      gather_idxs_{exec},
      local_to_global_ghost_{exec},
      one_scalar_{},
      diag_mtx_{inner_matrix_type->clone(exec)},
      offdiag_mtx_{ghost_matrix_type->clone(exec)}
{
    GKO_ASSERT(
        (dynamic_cast<ReadableFromMatrixData<ValueType, LocalIndexType>*>(
            diag_mtx_.get())));
    GKO_ASSERT(
        (dynamic_cast<ReadableFromMatrixData<ValueType, LocalIndexType>*>(
            offdiag_mtx_.get())));
    one_scalar_.init(exec, dim<2>{1, 1});
    one_scalar_->fill(one<value_type>());
}


template <typename ValueType, typename LocalIndexType, typename GlobalIndexType>
void Matrix<ValueType, LocalIndexType, GlobalIndexType>::convert_to(
    Matrix<next_precision<value_type>, local_index_type, global_index_type>*
        result) const
{
    GKO_ASSERT(this->get_communicator().size() ==
               result->get_communicator().size());
    result->diag_mtx_->copy_from(this->diag_mtx_.get());
    result->offdiag_mtx_->copy_from(this->offdiag_mtx_.get());
    result->gather_idxs_ = this->gather_idxs_;
    result->send_offsets_ = this->send_offsets_;
    result->recv_offsets_ = this->recv_offsets_;
    result->recv_sizes_ = this->recv_sizes_;
    result->send_sizes_ = this->send_sizes_;
    result->local_to_global_ghost_ = this->local_to_global_ghost_;
    result->set_size(this->get_size());
}


template <typename ValueType, typename LocalIndexType, typename GlobalIndexType>
void Matrix<ValueType, LocalIndexType, GlobalIndexType>::move_to(
    Matrix<next_precision<value_type>, local_index_type, global_index_type>*
        result)
{
    GKO_ASSERT(this->get_communicator().size() ==
               result->get_communicator().size());
    result->diag_mtx_->move_from(this->diag_mtx_.get());
    result->offdiag_mtx_->move_from(this->offdiag_mtx_.get());
    result->gather_idxs_ = std::move(this->gather_idxs_);
    result->send_offsets_ = std::move(this->send_offsets_);
    result->recv_offsets_ = std::move(this->recv_offsets_);
    result->recv_sizes_ = std::move(this->recv_sizes_);
    result->send_sizes_ = std::move(this->send_sizes_);
    result->local_to_global_ghost_ = std::move(this->local_to_global_ghost_);
    result->set_size(this->get_size());
    this->set_size({});
}


template <typename ValueType, typename LocalIndexType, typename GlobalIndexType>
void Matrix<ValueType, LocalIndexType, GlobalIndexType>::read_distributed(
    const device_matrix_data<value_type, global_index_type>& data,
    const Partition<local_index_type, global_index_type>* row_partition,
    const Partition<local_index_type, global_index_type>* col_partition)
{
    const auto comm = this->get_communicator();
    GKO_ASSERT_EQ(data.get_size()[0], row_partition->get_size());
    GKO_ASSERT_EQ(data.get_size()[1], col_partition->get_size());
    GKO_ASSERT_EQ(comm.size(), row_partition->get_num_parts());
    GKO_ASSERT_EQ(comm.size(), col_partition->get_num_parts());
    auto exec = this->get_executor();
    auto local_part = comm.rank();

    // set up LinOp sizes
    auto num_parts = static_cast<size_type>(row_partition->get_num_parts());
    auto global_num_rows = row_partition->get_size();
    auto global_num_cols = col_partition->get_size();
    dim<2> global_dim{global_num_rows, global_num_cols};
    this->set_size(global_dim);

    // temporary storage for the output
    array<local_index_type> diag_row_idxs{exec};
    array<local_index_type> diag_col_idxs{exec};
    array<value_type> diag_values{exec};
    array<local_index_type> offdiag_row_idxs{exec};
    array<local_index_type> offdiag_col_idxs{exec};
    array<value_type> offdiag_values{exec};
    array<local_index_type> recv_gather_idxs{exec};
    array<comm_index_type> recv_sizes_array{exec, num_parts};

    // build diagonal, off-diagonal matrix and communication structures
    exec->run(matrix::make_build_diag_offdiag(
        data, make_temporary_clone(exec, row_partition).get(),
        make_temporary_clone(exec, col_partition).get(), local_part,
        diag_row_idxs, diag_col_idxs, diag_values, offdiag_row_idxs,
        offdiag_col_idxs, offdiag_values, recv_gather_idxs,
        recv_sizes_array.get_data(), local_to_global_ghost_));

    // read the local matrix data
    const auto num_diag_rows =
        static_cast<size_type>(row_partition->get_part_size(local_part));
    const auto num_diag_cols =
        static_cast<size_type>(col_partition->get_part_size(local_part));
    const auto num_ghost_cols = local_to_global_ghost_.get_num_elems();
    device_matrix_data<value_type, local_index_type> diag_data{
        exec, dim<2>{num_diag_rows, num_diag_cols}, std::move(diag_row_idxs),
        std::move(diag_col_idxs), std::move(diag_values)};
    device_matrix_data<value_type, local_index_type> offdiag_data{
        exec, dim<2>{num_diag_rows, num_ghost_cols},
        std::move(offdiag_row_idxs), std::move(offdiag_col_idxs),
        std::move(offdiag_values)};
    as<ReadableFromMatrixData<ValueType, LocalIndexType>>(this->diag_mtx_)
        ->read(diag_data);
    as<ReadableFromMatrixData<ValueType, LocalIndexType>>(this->offdiag_mtx_)
        ->read(offdiag_data);

    // exchange step 1: determine recv_sizes, send_sizes, send_offsets
    exec->get_master()->copy_from(exec.get(), num_parts + 1,
                                  recv_sizes_array.get_data(),
                                  recv_sizes_.data());
    std::partial_sum(recv_sizes_.begin(), recv_sizes_.end(),
                     recv_offsets_.begin() + 1);
    comm.all_to_all(recv_sizes_.data(), 1, send_sizes_.data(), 1);
    std::partial_sum(send_sizes_.begin(), send_sizes_.end(),
                     send_offsets_.begin() + 1);
    send_offsets_[0] = 0;
    recv_offsets_[0] = 0;

    // exchange step 2: exchange gather_idxs from receivers to senders
    auto needs_host_buffer =
        exec->get_master() != exec && !gko::mpi::is_gpu_aware();
    if (needs_host_buffer || comm.force_host_buffer()) {
        recv_gather_idxs.set_executor(exec->get_master());
        gather_idxs_.clear();
        gather_idxs_.set_executor(exec->get_master());
    }
    gather_idxs_.resize_and_reset(send_offsets_.back());
    comm.all_to_all_v(recv_gather_idxs.get_const_data(), recv_sizes_.data(),
                      recv_offsets_.data(), gather_idxs_.get_data(),
                      send_sizes_.data(), send_offsets_.data());
    if (needs_host_buffer || comm.force_host_buffer()) {
        gather_idxs_.set_executor(exec);
    }
}


template <typename ValueType, typename LocalIndexType, typename GlobalIndexType>
void Matrix<ValueType, LocalIndexType, GlobalIndexType>::read_distributed(
    const matrix_data<value_type, global_index_type>& data,
    const Partition<local_index_type, global_index_type>* row_partition,
    const Partition<local_index_type, global_index_type>* col_partition)
{
    this->read_distributed(
        device_matrix_data<value_type, global_index_type>::create_from_host(
            this->get_executor(), data),
        row_partition, col_partition);
}


template <typename ValueType, typename LocalIndexType, typename GlobalIndexType>
void Matrix<ValueType, LocalIndexType, GlobalIndexType>::read_distributed(
    const matrix_data<ValueType, global_index_type>& data,
    const Partition<local_index_type, global_index_type>* partition)
{
    this->read_distributed(
        device_matrix_data<value_type, global_index_type>::create_from_host(
            this->get_executor(), data),
        partition, partition);
}


template <typename ValueType, typename LocalIndexType, typename GlobalIndexType>
void Matrix<ValueType, LocalIndexType, GlobalIndexType>::read_distributed(
    const device_matrix_data<ValueType, GlobalIndexType>& data,
    const Partition<local_index_type, global_index_type>* partition)
{
    this->read_distributed(data, partition, partition);
}


template <typename ValueType, typename LocalIndexType, typename GlobalIndexType>
mpi::request Matrix<ValueType, LocalIndexType, GlobalIndexType>::communicate(
    const local_vector_type* local_b) const
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

    local_b->row_gather(&gather_idxs_, send_buffer_.get());

    auto needs_host_buffer =
        exec->get_master() != exec && !gko::mpi::is_gpu_aware();
    if (needs_host_buffer || comm.force_host_buffer()) {
        host_recv_buffer_.init(exec->get_master(), recv_dim);
        host_send_buffer_.init(exec->get_master(), send_dim);
        host_send_buffer_->copy_from(send_buffer_.get());
    }

    mpi::contiguous_type type(num_cols, mpi::type_impl<ValueType>::get_type());
    auto send_ptr = needs_host_buffer || comm.force_host_buffer()
                        ? host_send_buffer_->get_const_values()
                        : send_buffer_->get_const_values();
    auto recv_ptr = needs_host_buffer || comm.force_host_buffer()
                        ? host_recv_buffer_->get_values()
                        : recv_buffer_->get_values();
    exec->synchronize();
    return comm.i_all_to_all_v(
        send_ptr, send_sizes_.data(), send_offsets_.data(), type.get(),
        recv_ptr, recv_sizes_.data(), recv_offsets_.data(), type.get());
}


template <typename ValueType, typename LocalIndexType, typename GlobalIndexType>
void Matrix<ValueType, LocalIndexType, GlobalIndexType>::apply_impl(
    const LinOp* b, LinOp* x) const
{
    distributed::precision_dispatch_real_complex<ValueType>(
        [this](const auto dense_b, auto dense_x) {
            auto x_exec = dense_x->get_executor();
            auto local_x = gko::matrix::Dense<ValueType>::create(
                x_exec, dense_x->get_local_vector()->get_size(),
                gko::make_array_view(
                    x_exec,
                    dense_x->get_local_vector()->get_num_stored_elements(),
                    dense_x->get_local_values()),
                dense_x->get_local_vector()->get_stride());
            if (this->get_const_local_offdiag()->get_size()) {
                auto comm = this->get_communicator();
                auto req = this->communicate(dense_b->get_local_vector());
                diag_mtx_->apply(dense_b->get_local_vector(), local_x.get());
                req.wait();
                auto exec = this->get_executor();
                auto needs_host_buffer =
                    exec->get_master() != exec && !gko::mpi::is_gpu_aware();
                if (needs_host_buffer || comm.force_host_buffer()) {
                    recv_buffer_->copy_from(host_recv_buffer_.get());
                }
                offdiag_mtx_->apply(one_scalar_.get(), recv_buffer_.get(),
                                    one_scalar_.get(), local_x.get());
            } else {
                diag_mtx_->apply(dense_b->get_local_vector(), local_x.get());
            }
        },
        b, x);
}


template <typename ValueType, typename LocalIndexType, typename GlobalIndexType>
void Matrix<ValueType, LocalIndexType, GlobalIndexType>::apply_impl(
    const LinOp* alpha, const LinOp* b, const LinOp* beta, LinOp* x) const
{
    distributed::precision_dispatch_real_complex<ValueType>(
        [this](const auto local_alpha, const auto dense_b,
               const auto local_beta, auto dense_x) {
            const auto x_exec = dense_x->get_executor();
            auto local_x = gko::matrix::Dense<ValueType>::create(
                x_exec, dense_x->get_local_vector()->get_size(),
                gko::make_array_view(
                    x_exec,
                    dense_x->get_local_vector()->get_num_stored_elements(),
                    dense_x->get_local_values()),
                dense_x->get_local_vector()->get_stride());
            if (this->get_const_local_offdiag()->get_size()) {
                auto comm = this->get_communicator();
                auto req = this->communicate(dense_b->get_local_vector());
                diag_mtx_->apply(local_alpha, dense_b->get_local_vector(),
                                 local_beta, local_x.get());
                req.wait();
                auto exec = this->get_executor();
                auto needs_host_buffer =
                    exec->get_master() != exec && !gko::mpi::is_gpu_aware();
                if (needs_host_buffer || comm.force_host_buffer()) {
                    recv_buffer_->copy_from(host_recv_buffer_.get());
                }
                offdiag_mtx_->apply(local_alpha, recv_buffer_.get(),
                                    one_scalar_.get(), local_x.get());
            } else {
                diag_mtx_->apply(local_alpha, dense_b->get_local_vector(),
                                 local_beta, local_x.get());
            }
        },
        alpha, b, beta, x);
}


template <typename ValueType, typename LocalIndexType, typename GlobalIndexType>
Matrix<ValueType, LocalIndexType, GlobalIndexType>::Matrix(const Matrix& other)
    : EnableLinOp<Matrix<value_type, local_index_type,
                         global_index_type>>{other.get_executor()},
      DistributedBase{other.get_communicator()}
{
    *this = other;
}


template <typename ValueType, typename LocalIndexType, typename GlobalIndexType>
Matrix<ValueType, LocalIndexType, GlobalIndexType>::Matrix(
    Matrix&& other) noexcept
    : EnableLinOp<Matrix<value_type, local_index_type,
                         global_index_type>>{other.get_executor()},
      DistributedBase{other.get_communicator()}
{
    *this = std::move(other);
}


template <typename ValueType, typename LocalIndexType, typename GlobalIndexType>
Matrix<ValueType, LocalIndexType, GlobalIndexType>&
Matrix<ValueType, LocalIndexType, GlobalIndexType>::operator=(
    const Matrix& other)
{
    if (this != &other) {
        GKO_ASSERT_EQ(other.get_communicator().size(),
                      this->get_communicator().size());
        this->set_size(other.get_size());
        diag_mtx_->copy_from(other.diag_mtx_.get());
        offdiag_mtx_->copy_from(other.offdiag_mtx_.get());
        gather_idxs_ = other.gather_idxs_;
        send_offsets_ = other.send_offsets_;
        recv_offsets_ = other.recv_offsets_;
        recv_sizes_ = other.recv_sizes_;
        send_sizes_ = other.send_sizes_;
        recv_sizes_ = other.recv_sizes_;
        local_to_global_ghost_ = other.local_to_global_ghost_;
        one_scalar_.init(this->get_executor(), dim<2>{1, 1});
        one_scalar_->fill(one<value_type>());
    }
    return *this;
}


template <typename ValueType, typename LocalIndexType, typename GlobalIndexType>
Matrix<ValueType, LocalIndexType, GlobalIndexType>&
Matrix<ValueType, LocalIndexType, GlobalIndexType>::operator=(Matrix&& other)
{
    if (this != &other) {
        GKO_ASSERT_EQ(other.get_communicator().size(),
                      this->get_communicator().size());
        this->set_size(other.get_size());
        other.set_size({});
        diag_mtx_->move_from(other.diag_mtx_.get());
        offdiag_mtx_->move_from(other.offdiag_mtx_.get());
        gather_idxs_ = std::move(other.gather_idxs_);
        send_offsets_ = std::move(other.send_offsets_);
        recv_offsets_ = std::move(other.recv_offsets_);
        recv_sizes_ = std::move(other.recv_sizes_);
        send_sizes_ = std::move(other.send_sizes_);
        recv_sizes_ = std::move(other.recv_sizes_);
        local_to_global_ghost_ = std::move(other.local_to_global_ghost_);
        one_scalar_.init(this->get_executor(), dim<2>{1, 1});
        one_scalar_->fill(one<value_type>());
    }
    return *this;
}


#define GKO_DECLARE_DISTRIBUTED_MATRIX(ValueType, LocalIndexType, \
                                       GlobalIndexType)           \
    class Matrix<ValueType, LocalIndexType, GlobalIndexType>
GKO_INSTANTIATE_FOR_EACH_VALUE_AND_LOCAL_GLOBAL_INDEX_TYPE(
    GKO_DECLARE_DISTRIBUTED_MATRIX);


}  // namespace distributed
}  // namespace gko
