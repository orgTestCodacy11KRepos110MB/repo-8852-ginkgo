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

#ifndef GINKGO_BENCHMARK_UTILS_DISTRIBUTED_HELPERS_HPP
#define GINKGO_BENCHMARK_UTILS_DISTRIBUTED_HELPERS_HPP


#include "benchmark/utils/formats.hpp"
#include "benchmark/utils/general.hpp"
#include "benchmark/utils/loggers.hpp"
#include "benchmark/utils/stencil_matrix.hpp"


template <typename ValueType>
using dist_vec = gko::experimental::distributed::Vector<ValueType>;
template <typename ValueType, typename LocalIndexType, typename GlobalIndexType>
using dist_mtx =
    gko::experimental::distributed::Matrix<ValueType, LocalIndexType,
                                           GlobalIndexType>;


std::string broadcast_json_input(std::istream& is,
                                 gko::experimental::mpi::communicator comm)
{
    auto exec = gko::ReferenceExecutor::create();

    std::string json_input;
    if (comm.rank() == 0) {
        std::string line;
        while (std::cin >> line) {
            json_input += line;
        }
    }

    auto input_size = json_input.size();
    comm.broadcast(exec->get_master(), &input_size, 1, 0);
    json_input.resize(input_size);
    comm.broadcast(exec->get_master(), &json_input[0],
                   static_cast<int>(input_size), 0);

    return json_input;
}


std::unique_ptr<dist_mtx<etype, itype, gko::int64>> create_distributed_matrix(
    std::shared_ptr<gko::Executor> exec,
    gko::experimental::mpi::communicator comm, const std::string& format_local,
    const std::string& format_non_local,
    const gko::matrix_data<etype, gko::int64>& data,
    const gko::experimental::distributed::Partition<itype, gko::int64>* part,
    rapidjson::Value& spmv_case, rapidjson::MemoryPoolAllocator<>& allocator)
{
    auto local_mat = formats::matrix_type_factory.at(format_local)(exec);
    auto non_local_mat =
        formats::matrix_type_factory.at(format_non_local)(exec);

    auto storage_logger = std::make_shared<StorageLogger>();
    exec->add_logger(storage_logger);

    auto dist_mat = dist_mtx<etype, itype, gko::int64>::create(
        exec, comm, local_mat.get(), non_local_mat.get());
    dist_mat->read_distributed(data, part);

    exec->remove_logger(gko::lend(storage_logger));
    storage_logger->write_data(comm, spmv_case, allocator);

    return dist_mat;
}

std::unique_ptr<dist_mtx<etype, itype, gko::int64>> create_distributed_matrix(
    std::shared_ptr<const gko::Executor> exec,
    gko::experimental::mpi::communicator comm, const std::string& format_local,
    const std::string& format_non_local,
    const gko::matrix_data<etype, gko::int64>& data,
    const gko::experimental::distributed::Partition<itype, gko::int64>* part)
{
    auto local_mat = formats::matrix_type_factory.at(format_local)(exec);
    auto non_local_mat =
        formats::matrix_type_factory.at(format_non_local)(exec);

    auto dist_mat = dist_mtx<etype, itype, gko::int64>::create(
        exec, comm, local_mat.get(), non_local_mat.get());
    dist_mat->read_distributed(data, part);

    return dist_mat;
}

#endif  // GINKGO_BENCHMARK_UTILS_DISTRIBUTED_HELPERS_HPP
