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

#ifndef GKO_PUBLIC_EXT_RESOURCE_MANAGER_MATRIX_DENSE_HPP_
#define GKO_PUBLIC_EXT_RESOURCE_MANAGER_MATRIX_DENSE_HPP_


#include <type_traits>


#include "resource_manager/base/element_types.hpp"
#include "resource_manager/base/helper.hpp"
#include "resource_manager/base/macro_helper.hpp"
#include "resource_manager/base/rapidjson_helper.hpp"
#include "resource_manager/base/resource_manager.hpp"
#include "resource_manager/base/type_list.hpp"


namespace gko {
namespace extension {
namespace resource_manager {


// TODO: Please add the corresponding to the resource_manager/base/types.hpp
// Add _expand(Dense) to ENUM_LINOP
// If need to override the generated enum for RM, use RM_CLASS or
// RM_CLASS_FACTORY env and rerun the generated script. Or replace the
// (RM_LinOpFactory::)DenseFactory and (RM_LinOp::)Dense and their snake case in
// IMPLEMENT_BRIDGE, ENABLE_SELECTION, *_select, ...


template <typename ValueType>
struct Generic<typename gko::matrix::Dense<ValueType>> {
    using type = std::shared_ptr<gko::matrix::Dense<ValueType>>;
    static type build(rapidjson::Value& item,
                      std::shared_ptr<const Executor> exec,
                      std::shared_ptr<const LinOp> linop,
                      ResourceManager* manager)
    {
        auto exec_ptr =
            get_pointer_check<Executor>(item, "exec", exec, linop, manager);
        auto size = get_value_with_default(item, "dim", gko::dim<2>{});
        // TODO: consider other thing from constructor
        auto ptr = share(gko::matrix::Dense<ValueType>::create(exec_ptr, size));

        if (item.HasMember("read")) {
            std::ifstream mtx_fd(item["read"].GetString());
            auto data = gko::read_raw<
                typename gko::matrix::Dense<ValueType>::value_type,
                typename gko::matrix::Dense<ValueType>::index_type>(mtx_fd);
            ptr->read(data);
        }

        return std::move(ptr);
    }
};


ENABLE_SELECTION(dense_select, call, std::shared_ptr<gko::LinOp>,
                 get_actual_type);


constexpr auto dense_list =
    typename span_list<tt_list_g_t<handle_type::ValueType>>::type();


template <>
std::shared_ptr<gko::LinOp>
create_from_config<RM_LinOp, RM_LinOp::Dense, gko::LinOp>(
    rapidjson::Value& item, std::shared_ptr<const Executor> exec,
    std::shared_ptr<const LinOp> linop, ResourceManager* manager)
{
    // go though the type
    auto type_string = create_type_name(  // trick for clang-format
        get_value_with_default(item, "ValueType",
                               get_default_string<handle_type::ValueType>()));
    auto ptr = dense_select<gko::matrix::Dense>(
        dense_list, [=](std::string key) { return key == type_string; }, item,
        exec, linop, manager);
    return std::move(ptr);
}


}  // namespace resource_manager
}  // namespace extension
}  // namespace gko


#endif  // GKO_PUBLIC_EXT_RESOURCE_MANAGER_MATRIX_DENSE_HPP_