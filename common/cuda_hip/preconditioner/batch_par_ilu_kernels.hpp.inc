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

__global__
    __launch_bounds__(default_block_size) void generate_common_pattern_to_fill_L_and_U(
        const int nrows, const int* const __restrict__ row_ptrs,
        const int* const __restrict__ col_idxs,
        const int* const __restrict__ row_ptrs_L,
        const int* const __restrict__ row_ptrs_U,
        int* const __restrict__ L_col_holders,
        int* const __restrict__ U_col_holders)
{
    constexpr int warp_size = config::warp_size;
    auto tile_grp =
        group::tiled_partition<warp_size>(group::this_thread_block());
    const int warp_id = thread::get_subwarp_id_flat<warp_size, int>();
    const int total_num_warps = thread::get_subwarp_num_flat<warp_size, int>();
    const int id_within_warp = tile_grp.thread_rank();

    for (int row_index = warp_id; row_index < nrows;
         row_index += total_num_warps) {
        const int L_row_start = row_ptrs_L[row_index];
        const int U_row_start = row_ptrs_U[row_index];
        const int row_start = row_ptrs[row_index];
        const int row_end = row_ptrs[row_index + 1];

        // const int diag_ele_loc = diag_ptrs[row_index];

        const int nnz_per_row_L =
            row_ptrs_L[row_index + 1] - row_ptrs_L[row_index];
        const int diag_ele_loc = row_start + nnz_per_row_L - 1;

        for (int i = row_start + id_within_warp; i < row_end;
             i += tile_grp.size()) {
            if (i < diag_ele_loc)  // or col_idxs[i] < row_index
            {
                const int corresponding_l_index = L_row_start + (i - row_start);
                L_col_holders[corresponding_l_index] = i;
            } else {
                if (i == diag_ele_loc)  // or col_idxs[i] == row_index
                {
                    const int corresponding_l_index =
                        L_row_start + (i - row_start);
                    L_col_holders[corresponding_l_index] = (-1 * row_index) - 1;
                }

                const int corresponding_u_index =
                    U_row_start + (i - diag_ele_loc);
                U_col_holders[corresponding_u_index] = i;
            }
        }
    }
}


template <typename ValueType>
__global__ __launch_bounds__(default_block_size) void fill_L_and_U(
    const size_type nbatch, const int nrows, const int nnz,
    const int* const __restrict__ col_idxs,
    const ValueType* const __restrict__ vals, const int L_nnz,
    int* const __restrict__ L_col_idxs, ValueType* const __restrict__ L_vals,
    const int* const __restrict__ L_col_holders, const int U_nnz,
    int* const __restrict__ U_col_idxs, ValueType* const __restrict__ U_vals,
    const int* const __restrict__ U_col_holders)
{
    const size_type gid = threadIdx.x + blockIdx.x * blockDim.x;
    const int greater_nnz = L_nnz > U_nnz ? L_nnz : U_nnz;

    for (size_type i = gid; i < greater_nnz * nbatch;
         i += gridDim.x * blockDim.x) {
        size_type batch_id;
        int col;
        ValueType val;

        if (i < L_nnz * nbatch) {
            const int l_A_index = L_col_holders[i % L_nnz];

            if (l_A_index >= 0) {
                col = col_idxs[l_A_index];
                batch_id = i / L_nnz;
                val = vals[l_A_index + batch_id * nnz];

            } else {
                col = -1 * (l_A_index + 1);
                val = one<ValueType>();
            }

            if (i < L_nnz) {
                L_col_idxs[i] = col;
            }

            L_vals[i] = val;
        }

        if (i < U_nnz * nbatch) {
            int u_A_index = U_col_holders[i % U_nnz];
            batch_id = i / U_nnz;

            col = col_idxs[u_A_index];
            val = vals[u_A_index + batch_id * nnz];

            if (i < U_nnz) {
                U_col_idxs[i] = col;
            }

            U_vals[i] = val;
        }
    }
}


template <typename ValueType>
__device__ __forceinline__ void parilu0_sweep_for_batch_entry(
    const int nnz, const ValueType* const __restrict__ A_vals,
    ValueType* const __restrict__ L_vals, ValueType* const __restrict__ U_vals,
    const int* const __restrict__ dependencies,
    const int* const __restrict__ nz_ptrs)
{
    for (int el = threadIdx.x; el < nnz;
         el += blockDim.x)  // non-coalesced accesses, but data locality
    {
        ValueType diag_val{one<ValueType>()};

        const int st = nz_ptrs[el];
        const int end = nz_ptrs[el + 1] - 1;

        bool has_diag_dependency = (end + 1 - st) % 2 == 0 ? true : false;

        ValueType sum{zero<ValueType>()};

        for (int idx = st + 1; idx <= end - 1; idx += 2) {
            sum += L_vals[dependencies[idx]] * U_vals[dependencies[idx + 1]];
        }

        if (has_diag_dependency == true) {
            diag_val = U_vals[dependencies[end]];
        }

        ValueType to_write = (A_vals[el] - sum) / diag_val;

        if (has_diag_dependency == true) {
            L_vals[dependencies[st]] = to_write;
        } else {
            U_vals[dependencies[st]] = to_write;
        }
    }
}

template <typename ValueType>
__global__ __launch_bounds__(default_block_size) void compute_parilu0_kernel(
    const size_type nbatch, const int nrows, const int nnz,
    const ValueType* const __restrict__ values, const int L_nnz,
    ValueType* const __restrict__ L_values, const int U_nnz,
    ValueType* const __restrict__ U_values, const int num_sweeps,
    const int* const __restrict__ dependencies,
    const int* const __restrict__ nz_ptrs)
{
    for (size_type batch_id = blockIdx.x; batch_id < nbatch;
         batch_id += gridDim.x) {
        extern __shared__ char local_mem_sh[];
        ValueType* const __restrict__ L_values_sh =
            reinterpret_cast<ValueType*>(local_mem_sh);
        ValueType* const __restrict__ U_values_sh = L_values_sh + L_nnz;

        single_copy(L_nnz, L_values + batch_id * L_nnz, L_values_sh);
        single_copy(U_nnz, U_values + batch_id * U_nnz, U_values_sh);
        __syncthreads();

        for (int sweep = 0; sweep < num_sweeps; sweep++) {
            parilu0_sweep_for_batch_entry(nnz, values + batch_id * nnz,
                                          L_values_sh, U_values_sh,
                                          dependencies, nz_ptrs);
            __syncthreads();
        }

        single_copy(L_nnz, L_values_sh, L_values + batch_id * L_nnz);
        single_copy(U_nnz, U_values_sh, U_values + batch_id * U_nnz);
    }
}