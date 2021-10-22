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

#ifndef GKO_PUBLIC_CORE_BASE_INDEX_SET_HPP_
#define GKO_PUBLIC_CORE_BASE_INDEX_SET_HPP_


#include <algorithm>
#include <mutex>
#include <vector>


#include <ginkgo/core/base/array.hpp>
#include <ginkgo/core/base/exception_helpers.hpp>
#include <ginkgo/core/base/executor.hpp>
#include <ginkgo/core/base/types.hpp>
#include <ginkgo/core/base/utils.hpp>


namespace gko {


/**
 * An index set class represents an ordered set with  in a mathematical sense.
 * The index set contains subsets which store the starting and end points of a
 * range, [a,b), storing the first index and one past the last index. As the
 * index set only stores the end-points of ranges, it can be quite efficient in
 * terms of storage.
 *
 * This class is particularly useful in storing continuous ranges. For example,
 * consider the index set (1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 12, 18, 19, 20, 21,
 * 42). Instead of storing the entire array of indices, one can store intervals
 * ([1,9), [10,13), [18,22), [42,43)), thereby only using half the storage.
 *
 * We store three arrays, one (subsets_begin) with the starting indices of the
 * subsets in the index set, another (subsets_end) storing one
 * index beyond the end indices of the subsets and the last
 * (superset_cumulative_indices) storing the cumulative number of indices in the
 * subsequent subsets with an initial zero which speeds up the
 * querying.
 *
 * Therefore the storage would look as follows
 *
 * > index_set = (1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 12, 18, 19, 20, 21, 42)
 * > subsets_begin = {1, 10, 18, 42}
 * > subsets_end = {9, 13, 22, 43}
 * > superset_cumulative_indices = {0, 8, 11, 15, 16}
 *
 * @tparam index_type  type of the indices being stored in the index set.
 *
 * @ingroup IndexSet
 */
template <typename IndexType = int32>
class IndexSet {
public:
    /**
     * The type of elements stored in the index set.
     */
    using index_type = IndexType;

    /**
     * Creates an index set not tied to any executor.
     *
     * This can only be empty. The executor can be set using the set_executor
     * method at a later time.
     */
    IndexSet() noexcept = default;

    /**
     * Creates an index set on the specified executor and the given size
     *
     * @param exec  the Executor where the index set data will be allocated
     * @param size  the maximum index the index set it allowed to hold. This
     *              is the size of the index space.
     */
    IndexSet(std::shared_ptr<const gko::Executor> executor,
             const index_type size = 0)
        : index_set_id_(0),
          index_space_size_(size),
          exec_(executor),
          subsets_begin_(exec_),
          subsets_end_(exec_),
          superset_cumulative_indices_(exec_)
    {}


    /**
     * Creates an index set on the specified executor and the given size
     *
     * @param exec  the Executor where the index set data will be allocated
     * @param size  the maximum index the index set it allowed to hold. This
     *              is the size of the index space.
     * @param indices  the indices that the index set should hold.
     * @param is_sorted  a parameter that specifies if the indices array is
     *                   sorted or not. `true` if sorted.
     */
    IndexSet(std::shared_ptr<const gko::Executor> executor,
             const index_type size, const gko::Array<index_type>& indices,
             const bool is_sorted = false)
        : index_set_id_(0),
          index_space_size_(size),
          exec_(executor),
          subsets_begin_(exec_),
          subsets_end_(exec_),
          superset_cumulative_indices_(exec_)
    {
        this->populate_subsets(indices, is_sorted);
    }


    /**
     * Creates an index set on the specified executor and the given size
     *
     * @param exec  the Executor where the index set data will be allocated
     * @param id    the id of the index set
     * @param size  the maximum index the index set it allowed to hold. This
     *              is the size of the index space.
     * @param indices  the indices that the index set should hold.
     * @param is_sorted  a parameter that specifies if the indices array is
     *                   sorted or not. `true` if sorted.
     */
    IndexSet(std::shared_ptr<const gko::Executor> executor, const index_type id,
             const index_type size, const gko::Array<index_type>& indices,
             const bool is_sorted = false)
        : index_set_id_(id),
          index_space_size_(size),
          exec_(executor),
          subsets_begin_(exec_),
          subsets_end_(exec_),
          superset_cumulative_indices_(exec_)
    {
        GKO_ASSERT(index_space_size_ >= indices.get_num_elems());
        this->populate_subsets(indices, is_sorted);
    }

    /**
     * Returns the Executor associated with the index set.
     *
     * @return the Executor associated with the index set
     */
    std::shared_ptr<const gko::Executor> get_executor() const noexcept
    {
        return exec_;
    }

    /**
     * Returns the size of the index set space.
     *
     * @return  the size of the index set space.
     */
    index_type get_size() const { return this->index_space_size_; }

    /**
     * Returns the id of the index set.
     *
     * @return  the id of the index set.
     */
    index_type get_id() const { return this->index_set_id_; }

    /**
     * Returns the id of the index set.
     *
     * @param  id  the id to set for the index set
     */
    void set_id(index_type id) { this->index_set_id_ = id; }

    /**
     * Returns if the index set is contiguous
     *
     * @return  if the index set is contiguous.
     */
    bool is_contiguous() const { return (this->get_num_subsets() <= 1); }

    /**
     * Return the actual number of indices stored in the index set
     *
     * @return  number of indices stored in the index set
     */
    index_type get_num_elems() const { return this->num_stored_indices_; };

    /**
     * Return the global index given a local index.
     *
     * Consider the set idx_set = (0, 1, 2, 4, 6, 7, 8, 9). This function
     * returns the element at the global index k stored in the index set. For
     * example, `idx_set.get_global_index(0) == 0` `idx_set.get_global_index(3)
     * == 4` and `idx_set.get_global_index(7) == 9`
     *
     * @note This function returns a scalar value and needs a scalar value.
     *       It is probably more efficient to use the Array functions that
     *       take and return arrays which allow for more throughput.
     *
     * @param local_index  the local index.
     * @return  the global index from the index set.
     */
    index_type get_global_index(const index_type local_index) const;

    /**
     * Return the local index given a global index.
     *
     * Consider the set idx_set = (0, 1, 2, 4, 6, 7, 8, 9). This function
     * returns the local index in the index set of the provided index set. For
     * example, `idx_set.get_local_index(0) == 0` `idx_set.get_local_index(4)
     * == 3` and `idx_set.get_local_index(6) == 4`.
     *
     * @note This function returns a scalar value and needs a scalar value.
     *       It is probably more efficient to use the Array functions that
     *       take and return arrays which allow for more throughput.
     *
     * @param global_index  the global index.
     * @return  the local index of the element in the index set.
     */
    index_type get_local_index(const index_type global_index) const;

    /**
     * This is an array version of the scalar function above.
     *
     * @param local_indices  the local index array.
     * @param is_sorted  a parameter that specifies if the query array is sorted
     *                   or not. `true` if sorted .
     * @return  the global index array from the index set.
     *
     * @note Whenever possible, passing a sorted array is preferred as the
     *       queries can be significantly faster.
     */
    Array<index_type> get_global_indices(const Array<index_type>& local_indices,
                                         const bool is_sorted = false) const;

    /**
     * This is an array version of the scalar function above.
     *
     * @param global_indices  the global index array.
     * @param is_sorted  a parameter that specifies if the query array is sorted
     *                   or not. `true` if sorted.
     * @return  the local index array from the index set.
     *
     * @note Whenever possible, passing a sorted array is preferred as the
     *       queries can be significantly faster.
     */
    Array<index_type> get_local_indices(const Array<index_type>& global_indices,
                                        const bool is_sorted = false) const;

    /**
     * Checks if the global index exists in the index set.
     *
     * @param index  the index to check.
     * @return  whether the element exists in the index set.
     */
    bool contains(const index_type index) const;

    /**
     * Returns the number of subsets stored in the index set.
     *
     * @return  the number of stored subsets.
     */
    index_type get_num_subsets() const
    {
        return this->subsets_begin_.get_num_elems();
    }

    /**
     * Returns a pointer to the beginning indices of the subsets.
     *
     * @return  a pointer to the beginning indices of the subsets.
     */
    const index_type* get_subsets_begin() const
    {
        return this->subsets_begin_.get_const_data();
    }

    /**
     * Returns a pointer to the end indices of the subsets.
     *
     * @return  a pointer to the end indices of the subsets.
     */
    const index_type* get_subsets_end() const
    {
        return this->subsets_end_.get_const_data();
    }

    /**
     * Returns a pointer to the cumulative indices of the superset of
     * the subsets.
     *
     * @return  a pointer to the cumulative indices of the superset of the
     *          subsets.
     */
    const index_type* get_superset_indices() const
    {
        return this->superset_cumulative_indices_.get_const_data();
    }

private:
    void populate_subsets(const gko::Array<index_type>& indices,
                          const bool is_sorted);

    std::shared_ptr<const gko::Executor> exec_;

    index_type index_set_id_;
    index_type index_space_size_;
    index_type num_stored_indices_;
    gko::Array<index_type> subsets_begin_;
    gko::Array<index_type> subsets_end_;
    gko::Array<index_type> superset_cumulative_indices_;
};


}  // namespace gko


#endif  // GKO_PUBLIC_CORE_BASE_INDEX_SET_HPP_