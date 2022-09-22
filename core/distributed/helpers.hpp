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

#include <memory>


#include <ginkgo/config.hpp>
#include <ginkgo/core/distributed/matrix.hpp>
#include <ginkgo/core/distributed/vector.hpp>
#include <ginkgo/core/matrix/dense.hpp>


#include "core/base/dispatch_helper.hpp"


namespace gko {
namespace detail {


template <typename ValueType>
std::unique_ptr<matrix::Dense<ValueType>> create_with_config_of(
    const matrix::Dense<ValueType>* mtx)
{
    return matrix::Dense<ValueType>::create(mtx->get_executor(),
                                            mtx->get_size(), mtx->get_stride());
}


template <typename ValueType>
const matrix::Dense<ValueType>* get_local(const matrix::Dense<ValueType>* mtx)
{
    return mtx;
}


template <typename ValueType>
matrix::Dense<ValueType>* get_local(matrix::Dense<ValueType>* mtx)
{
    return mtx;
}


#if GINKGO_BUILD_MPI


template <typename ValueType>
std::unique_ptr<experimental::distributed::Vector<ValueType>>
create_with_config_of(const experimental::distributed::Vector<ValueType>* mtx)
{
    return experimental::distributed::Vector<ValueType>::create(
        mtx->get_executor(), mtx->get_communicator(), mtx->get_size(),
        mtx->get_local_vector()->get_size(),
        mtx->get_local_vector()->get_stride());
}


template <typename ValueType>
matrix::Dense<ValueType>* get_local(
    experimental::distributed::Vector<ValueType>* mtx)
{
    return const_cast<matrix::Dense<ValueType>*>(mtx->get_local_vector());
}


template <typename ValueType>
const matrix::Dense<ValueType>* get_local(
    const experimental::distributed::Vector<ValueType>* mtx)
{
    return mtx->get_local_vector();
}


#endif


template <typename Arg>
bool is_distributed(Arg* linop)
{
#if GINKGO_BUILD_MPI
    return dynamic_cast<const experimental::distributed::DistributedBase*>(
        linop);
#else
    return false;
#endif
}


template <typename Arg, typename... Rest>
bool is_distributed(Arg* linop, Rest*... rest)
{
#if GINKGO_BUILD_MPI
    bool is_distributed_value =
        dynamic_cast<const experimental::distributed::DistributedBase*>(linop);
    GKO_ASSERT(is_distributed_value == is_distributed(rest...));
    return is_distributed_value;
#else
    return false;
#endif
}


template <typename ValueType, typename T, typename F, typename... Args>
void run_vector(T* linop, F&& f, Args... args)
{
#if GINKGO_BUILD_MPI
    if (is_distributed(linop)) {
        using type = std::conditional_t<std::is_const<T>::value,
                                        const distributed::Vector<ValueType>,
                                        distributed::Vector<ValueType>>;
        f(dynamic_cast<type*>(linop), std::forward<Args>(args)...);
    } else
#endif
    {
        using type = std::conditional_t<std::is_const<T>::value,
                                        const matrix::Dense<ValueType>,
                                        matrix::Dense<ValueType>>;
        f(dynamic_cast<type*>(linop), std::forward<Args>(args)...);
    }
}


/**
 * Returns the local vector as a LinOp
 * @tparam LinOpType either LinOp or const LinOp
 * @param op  the object from which the local vector is extracted
 * @return  the local vector
 */
template <typename LinOpType, typename = std::enable_if_t<std::is_same<
                                  LinOp, std::decay_t<LinOpType>>::value>>
LinOpType* get_local(LinOpType* op)
{
    LinOpType* local = nullptr;
#if GINKGO_BUILD_MPI
    if (is_distributed(op)) {
        run<gko::distributed::Vector, float, double, std::complex<float>,
            std::complex<double>>(
            op, [&](auto vector_op) { local = get_local(vector_op); });
    } else
#endif
    {
        run<gko::matrix::Dense, float, double, std::complex<float>,
            std::complex<double>>(
            op, [&](auto vector_op) { local = get_local(vector_op); });
    }
    return local;
}


/**
 * Extracts the correct Matrix instantiation for a given linop and calls a
 * function with it.
 *
 * @note internally this uses run(T*, func, Args...)
 */
template <
    typename T, typename func, typename... Args,
    typename = std::enable_if<std::is_same<LinOp, std::decay_t<T>>::value>>
void dispatch_distributed_matrix(T* obj, func f, Args... args)
{
    using namespace gko::distributed;
    gko::run<Matrix<float, int32, int32>, Matrix<float, int32, int64>,
             Matrix<float, int64, int64>, Matrix<double, int32, int32>,
             Matrix<double, int32, int64>, Matrix<double, int64, int64>,
             Matrix<std::complex<float>, int32, int32>,
             Matrix<std::complex<float>, int32, int64>,
             Matrix<std::complex<float>, int64, int64>,
             Matrix<std::complex<double>, int32, int32>,
             Matrix<std::complex<double>, int32, int64>,
             Matrix<std::complex<double>, int64, int64>>(
        obj, f, std::forward<Args>(args)...);
}


}  // namespace detail
}  // namespace gko
