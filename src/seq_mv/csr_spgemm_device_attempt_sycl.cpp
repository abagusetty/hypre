/******************************************************************************
 * Copyright 1998-2019 Lawrence Livermore National Security, LLC and other
 * HYPRE Project Developers. See the top-level COPYRIGHT file for details.
 *
 * SPDX-License-Identifier: (Apache-2.0 OR MIT)
 ******************************************************************************/

/*- - - - - - - - - - - - - - - - - - - - - - - - - - *
           Perform SpMM with Row Nnz Estimation
 *- - - - - - - - - - - - - - - - - - - - - - - - - - */
#include "seq_mv.h"
#include "csr_spgemm_device.h"

#if defined(HYPRE_USING_SYCL)

template <typename T>
using relaxed_atomic_ref =
  cl::sycl::ONEAPI::atomic_ref< T, cl::sycl::ONEAPI::memory_order::relaxed,
				cl::sycl::ONEAPI::memory_scope::device,
				cl::sycl::access::address_space::global_space>;

template <char HashType, HYPRE_Int attempt>
static __inline__ __attribute__((always_inline)) HYPRE_Int hash_insert_attempt(
    HYPRE_Int HashSize,           /* capacity of the hash table */
    volatile HYPRE_Int *HashKeys, /* assumed to be initialized as all -1's */
    volatile HYPRE_Complex *HashVals, /* assumed to be initialized as all 0's */
    HYPRE_Int key,                    /* assumed to be nonnegative */
    HYPRE_Complex val, HYPRE_Int &count, /* increase by 1 if is a new entry */
    char failed, volatile char *warp_failed)
{
#pragma unroll
   for (HYPRE_Int i = 0; i < HashSize; i++)
   {
      HYPRE_Int j;
      /* compute the hash value of key */
      if (i == 0)
      {
         j = key & (HashSize - 1);
      }
      else
      {
         j = HashFunc<HashType>(HashSize, key, i, j);
      }

      /* try to insert key+1 into slot j */
      /*
      DPCT1039:0: The generated code assumes that "(HYPRE_Int*)(HashKeys+j)"
      points to the global memory address space. If it points to a local memory
      address space, replace "dpct::atomic_compare_exchange_strong" with
      "dpct::atomic_compare_exchange_strong<HYPRE_Int,
      sycl::access::address_space::local_space>".
      */
      HYPRE_Int old = dpct::atomic_compare_exchange_strong(
          (HYPRE_Int *)(HashKeys + j), -1, key);

      if (old == -1)
      {
         /* new insertion, increase counter */
         count++;
         /* this slot was open, insert value */
         if (attempt == 2 || failed == 0 || *warp_failed == 0)
         {
	   relaxed_atomic_ref<HYPRE_Complex *>((HYPRE_Complex *)(HashVals + j)).fetch_add(val);
         }
         return j;
      }

      if (old == key)
      {
         /* this slot contains 'key', update value */
         if (attempt == 2 || failed == 0 || *warp_failed == 0)
         {
	   relaxed_atomic_ref<HYPRE_Complex *>((HYPRE_Complex *)(HashVals + j)).fetch_add(val);
         }
         return j;
      }
   }

   return -1;
}

template <HYPRE_Int attempt, char HashType>
static __inline__ __attribute__((always_inline)) HYPRE_Int csr_spmm_compute_row_attempt(
    HYPRE_Int rowi, volatile HYPRE_Int lane_id, HYPRE_Int *ia, HYPRE_Int *ja,
    HYPRE_Complex *aa, HYPRE_Int *ib, HYPRE_Int *jb, HYPRE_Complex *ab,
    HYPRE_Int s_HashSize, volatile HYPRE_Int *s_HashKeys,
    volatile HYPRE_Complex *s_HashVals, HYPRE_Int g_HashSize,
    HYPRE_Int *g_HashKeys, HYPRE_Complex *g_HashVals, char &failed,
    volatile char *warp_s_failed, sycl::nd_item<3>& item)
{
   /* load the start and end position of row i of A */
   HYPRE_Int j = -1;
   if (lane_id < 2)
   {
      j = read_only_load(ia + rowi + lane_id);
   }

   cl::sycl::ONEAPI::sub_group SG = item.get_sub_group();
   
   const HYPRE_Int istart = SG.shuffle(j, 0);
   const HYPRE_Int iend = SG.shuffle(j, 1);

   HYPRE_Int num_new_insert = 0;

   /* load column idx and values of row i of A */
   for (HYPRE_Int i = istart; i < iend; i += item.get_local_range().get(1))
   {
      HYPRE_Int     colA = -1;
      HYPRE_Complex valA = 0.0;

      if (item.get_local_id(2) == 0 && i + item.get_local_id(1) < iend)
      {
         colA = read_only_load(ja + i + item.get_local_id(1));
         valA = read_only_load(aa + i + item.get_local_id(1));
      }

#if 0
      //const HYPRE_Int ymask = get_mask<4>(lane_id);
      // TODO: need to confirm the behavior of __ballot_sync, leave it here for now
      //const HYPRE_Int num_valid_rows = __popc(__ballot_sync(ymask, valid_i));
      //for (HYPRE_Int j = 0; j < num_valid_rows; j++)
#endif

      /* threads in the same ygroup work on one row together */
      const HYPRE_Int rowB = SG.shuffle(colA, 0);
      const HYPRE_Complex mult = SG.shuffle(valA, 0);
      /* open this row of B, collectively */
      HYPRE_Int tmp = -1;
      if (rowB != -1 && item.get_local_id(2) < 2)
      {
         tmp = read_only_load(ib + rowB + item.get_local_id(2));
      }

      const HYPRE_Int rowB_start = SG.shuffle(tmp, 0);
      const HYPRE_Int rowB_end = SG.shuffle(tmp, 1);

      for (HYPRE_Int k = rowB_start; k < rowB_end;
           k += item.get_local_range(2))
      {
         if (k + item.get_local_id(2) < rowB_end)
         {
            const HYPRE_Int k_idx =
                read_only_load(jb + k + item.get_local_id(2));
            const HYPRE_Complex k_val =
                read_only_load(ab + k + item.get_local_id(2)) * mult;
            /* first try to insert into shared memory hash table */
            HYPRE_Int pos = hash_insert_attempt<HashType, attempt>
               (s_HashSize, s_HashKeys, s_HashVals, k_idx, k_val, num_new_insert,
                failed, warp_s_failed);

            if (-1 == pos)
            {
               pos = hash_insert_attempt<HashType, attempt>
                     (g_HashSize, g_HashKeys, g_HashVals, k_idx, k_val, num_new_insert,
                      failed, warp_s_failed);
            }
            /* if failed again, both hash tables must have been full
               (hash table size estimation was too small).
               Increase the counter anyhow (will lead to over-counting)
               */
            if (pos == -1)
            {
               num_new_insert ++;
               failed = 1;
               if (attempt == 1)
               {
                  *warp_s_failed = 1;
               }
            }
         }
      }
   }

   return num_new_insert;
}

template <HYPRE_Int NUM_WARPS_PER_BLOCK, HYPRE_Int SHMEM_HASH_SIZE, HYPRE_Int attempt, char HashType>
void
csr_spmm_attempt(HYPRE_Int  M, /* HYPRE_Int K, HYPRE_Int N, */
                 HYPRE_Int *ia, HYPRE_Int *ja, HYPRE_Complex *aa,
                 HYPRE_Int *ib, HYPRE_Int *jb, HYPRE_Complex *ab,
                                HYPRE_Int *js, HYPRE_Complex *as,
                 HYPRE_Int *ig, HYPRE_Int *jg, HYPRE_Complex *ag,
                 HYPRE_Int *rc, HYPRE_Int *rg, sycl::nd_item<3>& item,
                 volatile HYPRE_Int *s_HashKeys,
                 volatile HYPRE_Complex *s_HashVals, volatile char *s_failed)
{
   volatile const HYPRE_Int num_warps =
       NUM_WARPS_PER_BLOCK * item.get_group_range(2);
   /* warp id inside the block */
   volatile const HYPRE_Int warp_id = get_warp_id(item);
   /* lane id inside the warp */
   volatile HYPRE_Int lane_id = get_lane_id(item);
   /* shared memory hash table */

   /* shared memory hash table for this warp */
   volatile HYPRE_Int  *warp_s_HashKeys = s_HashKeys + warp_id * SHMEM_HASH_SIZE;
   volatile HYPRE_Complex *warp_s_HashVals = s_HashVals + warp_id * SHMEM_HASH_SIZE;
   /* shared memory failed flag for warps */

   volatile char *warp_s_failed = s_failed + warp_id;

#ifdef HYPRE_DEBUG
   assert(blockDim.z              == NUM_WARPS_PER_BLOCK);
   assert(blockDim.x * blockDim.y == warpSize);
   assert(NUM_WARPS_PER_BLOCK <= warpSize);
#endif

   cl::sycl::ONEAPI::sub_group SG = item.get_sub_group();
   
   for (HYPRE_Int i = item.get_group(2) * NUM_WARPS_PER_BLOCK + warp_id;
        i < M; i += num_warps)
   {
      /* start/end position of global memory hash table */
      HYPRE_Int j = -1, istart_g, iend_g, ghash_size;
      char failed = 0;

      if (lane_id < 2)
      {
         j = read_only_load(ig + i + lane_id);
      }

      istart_g = SG.shuffle(j, 0);
      iend_g = SG.shuffle(j, 1);

      /* size of global hash table allocated for this row (must be power of 2) */
      ghash_size = iend_g - istart_g;

      if (attempt == 2)
      {
         if (ghash_size == 0)
         {
            continue;
         }
      }

      /* initialize warp's shared failed flag */
      if (attempt == 1 && lane_id == 0)
      {
         *warp_s_failed = 0;
      }
      /* initialize warp's shared and global memory hash table */
#pragma unrolll
      for (HYPRE_Int k = lane_id; k < SHMEM_HASH_SIZE;
           k += SG.get_local_range(0))
      {
         warp_s_HashKeys[k] = -1;
         warp_s_HashVals[k] = 0.0;
      }
#pragma unrolll
      for (HYPRE_Int k = lane_id; k < ghash_size;
           k += SG.get_local_range(0))
      {
         jg[istart_g+k] = -1;
         ag[istart_g+k] = 0.0;
      }
      /*
      DPCT1007:9: Migration of this CUDA API is not supported by the Intel(R)
      DPC++ Compatibility Tool.
      */
      __syncwarp();

      /* work with two hash tables */
      j = csr_spmm_compute_row_attempt<attempt, HashType>(
          i, lane_id, ia, ja, aa, ib, jb, ab, SHMEM_HASH_SIZE, warp_s_HashKeys,
          warp_s_HashVals, ghash_size, jg + istart_g, ag + istart_g, failed,
          warp_s_failed, item);

#ifdef HYPRE_DEBUG
      if (attempt == 2)
      {
         assert(failed == 0);
      }
#endif

      /* num of inserts in this row (an upper bound) */
      j = warp_reduce_sum(j);

      if (attempt == 1)
      {
         failed = warp_allreduce_sum(failed);
      }

      if (attempt == 1 && failed)
      {
         if (lane_id == 0)
         {
            rg[i] = next_power_of_2(j - SHMEM_HASH_SIZE);
         }
      }
      else
      {
         if (lane_id == 0)
         {
            rc[i] = j;
            if (attempt == 1)
            {
               rg[i] = 0;
            }
         }
#pragma unroll
         for (HYPRE_Int k = lane_id; k < SHMEM_HASH_SIZE;
              k += SG.get_local_range(0))
         {
            js[i*SHMEM_HASH_SIZE + k] = warp_s_HashKeys[k];
            as[i*SHMEM_HASH_SIZE + k] = warp_s_HashVals[k];
         }
      }
   } // for (i=...)
}

template <HYPRE_Int NUM_WARPS_PER_BLOCK, HYPRE_Int SHMEM_HASH_SIZE>
static __inline__ __attribute__((always_inline)) HYPRE_Int copy_from_hash_into_C_row(
    HYPRE_Int lane_id, volatile HYPRE_Int *s_HashKeys,
    volatile HYPRE_Complex *s_HashVals, HYPRE_Int ghash_size,
    HYPRE_Int *jg_start, HYPRE_Complex *ag_start, HYPRE_Int *jc_start,
    HYPRE_Complex *ac_start)
{
   HYPRE_Int j = 0;
   // abb: needs item to be passed as a parameter
   cl::sycl::ONEAPI::sub_group SG = item.get_sub_group();

   /* copy shared memory hash table into C */
#pragma unrolll
   for (HYPRE_Int k = lane_id; k < SHMEM_HASH_SIZE;
        k += SG.get_local_range(0))
   {
      HYPRE_Int key, sum, pos;
      key = s_HashKeys[k];
      HYPRE_Int in = key != -1;
      pos = warp_prefix_sum(lane_id, in, sum, item);
      if (key != -1)
      {
         jc_start[j + pos] = key;
         ac_start[j + pos] = s_HashVals[k];
      }
      j += sum;
   }

   /* copy global memory hash table into C */
#pragma unrolll
   for (HYPRE_Int k = 0; k < ghash_size;
        k += SG.get_local_range(0))
   {
      HYPRE_Int key = -1, sum, pos;
      if (k + lane_id < ghash_size)
      {
         key = jg_start[k + lane_id];
      }
      HYPRE_Int in = key != -1;
      pos = warp_prefix_sum(lane_id, in, sum, item);
      if (key != -1)
      {
         jc_start[j + pos] = key;
         ac_start[j + pos] = ag_start[k + lane_id];
      }
      j += sum;
   }

   return j;
}

template <HYPRE_Int NUM_WARPS_PER_BLOCK, HYPRE_Int SHMEM_HASH_SIZE>
void
copy_from_hash_into_C(HYPRE_Int  M,   HYPRE_Int *js,  HYPRE_Complex *as,
                      HYPRE_Int *ig1, HYPRE_Int *jg1, HYPRE_Complex *ag1,
                      HYPRE_Int *ig2, HYPRE_Int *jg2, HYPRE_Complex *ag2,
                      HYPRE_Int *ic, HYPRE_Int *jc, HYPRE_Complex *ac,
                      sycl::nd_item<3>& item)
{
   const HYPRE_Int num_warps =
       NUM_WARPS_PER_BLOCK * item.get_group_range(2);
   /* warp id inside the block */
   const HYPRE_Int warp_id = get_warp_id(item);
   /* lane id inside the warp */
   volatile const HYPRE_Int lane_id = get_lane_id(item);

   cl::sycl::ONEAPI::sub_group SG = item.get_sub_group();
   
#ifdef HYPRE_DEBUG
   assert(blockDim.x * blockDim.y == SG.get_local_range(0));
#endif

   for (HYPRE_Int i = item.get_group(2) * NUM_WARPS_PER_BLOCK + warp_id;
        i < M; i += num_warps)
   {
      HYPRE_Int kc, kg1, kg2;

      /* start/end position in C */
      if (lane_id < 2)
      {
         kc  = read_only_load(ic  + i + lane_id);
         kg1 = read_only_load(ig1 + i + lane_id);
         kg2 = read_only_load(ig2 + i + lane_id);
      }

      HYPRE_Int istart_c = SG.shuffle(kc, 0);
#ifdef HYPRE_DEBUG
      HYPRE_Int iend_c    = SG.shuffle(kc, 1);
#endif

      HYPRE_Int istart_g1 = SG.shuffle(kg1, 0);
      HYPRE_Int iend_g1 = SG.shuffle(kg1, 1);

      HYPRE_Int istart_g2 = SG.shuffle(kg2, 0);
      HYPRE_Int iend_g2 = SG.shuffle(kg2, 1);

      HYPRE_Int g1_size = iend_g1 - istart_g1;
      HYPRE_Int g2_size = iend_g2 - istart_g2;

#ifdef HYPRE_DEBUG
      HYPRE_Int j;
#endif

      if (g2_size == 0)
      {
#ifdef HYPRE_DEBUG
         j =
#endif
         copy_from_hash_into_C_row<NUM_WARPS_PER_BLOCK, SHMEM_HASH_SIZE>
         (lane_id, js + i * SHMEM_HASH_SIZE, as + i * SHMEM_HASH_SIZE, g1_size, jg1 + istart_g1,
         ag1 + istart_g1, jc + istart_c, ac + istart_c);
      }
      else
      {
#ifdef HYPRE_DEBUG
         j =
#endif
         copy_from_hash_into_C_row<NUM_WARPS_PER_BLOCK, SHMEM_HASH_SIZE>
         (lane_id, js + i * SHMEM_HASH_SIZE, as + i * SHMEM_HASH_SIZE, g2_size, jg2 + istart_g2,
         ag2 + istart_g2, jc + istart_c, ac + istart_c);
      }
#ifdef HYPRE_DEBUG
      assert(istart_c + j == iend_c);
#endif
   }
}

/* SpGeMM with Rownnz Estimates */
HYPRE_Int
hypreDevice_CSRSpGemmWithRownnzEstimate(HYPRE_Int m, HYPRE_Int k, HYPRE_Int n,
                                        HYPRE_Int *d_ia, HYPRE_Int *d_ja, HYPRE_Complex *d_a,
                                        HYPRE_Int *d_ib, HYPRE_Int *d_jb, HYPRE_Complex *d_b,
                                        HYPRE_Int *d_rc,
                                        HYPRE_Int **d_ic_out, HYPRE_Int **d_jc_out, HYPRE_Complex **d_c_out,
                                        HYPRE_Int *nnzC)
{
   const HYPRE_Int num_warps_per_block =  20;
   const HYPRE_Int shmem_hash_size     = 128;
   const HYPRE_Int BDIMX               =   2;
   const HYPRE_Int BDIMY               =  16;

   /* SYCL kernel configurations */
   sycl::range<3> bDim(num_warps_per_block, BDIMY, BDIMX);

   // abb: check to make sure item.....is not available yet here
   hypre_assert(bDim[2] * bDim[1] ==
                item.get_sub_group().get_local_range(0));

   // for cases where one WARP works on a row
   sycl::range<3> gDim(1, 1, (m + bDim[0] - 1) / bDim[0]);

   char hash_type = hypre_HandleSpgemmHashType(hypre_handle());

   /* ---------------------------------------------------------------------------
    * build hash table
    * ---------------------------------------------------------------------------*/
   HYPRE_Int  *d_ghash_i, *d_ghash_j, ghash_size;
   HYPRE_Complex *d_ghash_a;
   csr_spmm_create_hash_table(m, d_rc, NULL, shmem_hash_size, m,
                              &d_ghash_i, &d_ghash_j, &d_ghash_a, &ghash_size);

   size_t m_ul = m;

   HYPRE_Int     *d_ic       = hypre_TAlloc(HYPRE_Int,     m+1,                  HYPRE_MEMORY_DEVICE);
   HYPRE_Int     *d_ghash2_i = hypre_TAlloc(HYPRE_Int,     m+1,                  HYPRE_MEMORY_DEVICE);
   HYPRE_Int     *d_js       = hypre_TAlloc(HYPRE_Int,     shmem_hash_size*m_ul, HYPRE_MEMORY_DEVICE);
   HYPRE_Complex *d_as       = hypre_TAlloc(HYPRE_Complex, shmem_hash_size*m_ul, HYPRE_MEMORY_DEVICE);

   /* ---------------------------------------------------------------------------
    * 1st multiplication attempt:
    * ---------------------------------------------------------------------------*/
   if (hash_type == 'L')
   {
      HYPRE_SYCL_LAUNCH( (csr_spmm_attempt<num_warps_per_block, shmem_hash_size, 1, 'L'>), gDim, bDim,
                         m, /*k, n,*/ d_ia, d_ja, d_a, d_ib, d_jb, d_b, d_js, d_as, d_ghash_i, d_ghash_j, d_ghash_a,
                         d_ic + 1, d_ghash2_i + 1 );
   }
   else if (hash_type == 'Q')
   {
      HYPRE_SYCL_LAUNCH( (csr_spmm_attempt<num_warps_per_block, shmem_hash_size, 1, 'Q'>), gDim, bDim,
                         m, /*k, n,*/ d_ia, d_ja, d_a, d_ib, d_jb, d_b, d_js, d_as, d_ghash_i, d_ghash_j, d_ghash_a,
                         d_ic + 1, d_ghash2_i + 1);
   }
   else if (hash_type == 'D')
   {
      HYPRE_SYCL_LAUNCH( (csr_spmm_attempt<num_warps_per_block, shmem_hash_size, 1, 'D'>), gDim, bDim,
                         m, /*k, n,*/ d_ia, d_ja, d_a, d_ib, d_jb, d_b, d_js, d_as, d_ghash_i, d_ghash_j, d_ghash_a,
                         d_ic + 1, d_ghash2_i + 1 );
   }
   else
   {
      printf("Unrecognized hash type ... [L, Q, D]\n");
      exit(0);
   }

   /* ---------------------------------------------------------------------------
    * build a secondary hash table for long rows
    * ---------------------------------------------------------------------------*/
   HYPRE_Int ghash2_size, *d_ghash2_j;
   HYPRE_Complex *d_ghash2_a;

   csr_spmm_create_ija(m, d_ghash2_i, &d_ghash2_j, &d_ghash2_a, &ghash2_size);

   /* ---------------------------------------------------------------------------
    * 2nd multiplication attempt:
    * ---------------------------------------------------------------------------*/
   if (ghash2_size > 0)
   {
      if (hash_type == 'L')
      {
         HYPRE_SYCL_LAUNCH( (csr_spmm_attempt<num_warps_per_block, shmem_hash_size, 2, 'L'>), gDim, bDim,
                            m, /*k, n,*/ d_ia, d_ja, d_a, d_ib, d_jb, d_b, d_js, d_as, d_ghash2_i, d_ghash2_j, d_ghash2_a,
                            d_ic + 1, NULL );
      }
      else if (hash_type == 'Q')
      {
         HYPRE_SYCL_LAUNCH( (csr_spmm_attempt<num_warps_per_block, shmem_hash_size, 2, 'Q'>), gDim, bDim,
                            m, /*k, n,*/ d_ia, d_ja, d_a, d_ib, d_jb, d_b, d_js, d_as, d_ghash2_i, d_ghash2_j, d_ghash2_a,
                            d_ic + 1, NULL);
      }
      else if (hash_type == 'D')
      {
         HYPRE_SYCL_LAUNCH( (csr_spmm_attempt<num_warps_per_block, shmem_hash_size, 2, 'D'>), gDim, bDim,
                            m, /*k, n,*/ d_ia, d_ja, d_a, d_ib, d_jb, d_b, d_js, d_as, d_ghash2_i, d_ghash2_j, d_ghash2_a,
                            d_ic + 1, NULL);
      }
      else
      {
         printf("Unrecognized hash type ... [L, Q, D]\n");
         exit(0);
      }
   }

   HYPRE_Int nnzC_gpu, *d_jc;
   HYPRE_Complex *d_c;
   csr_spmm_create_ija(m, d_ic, &d_jc, &d_c, &nnzC_gpu);

   HYPRE_SYCL_LAUNCH( (copy_from_hash_into_C<num_warps_per_block, shmem_hash_size>), gDim, bDim,
                      m, d_js, d_as, d_ghash_i, d_ghash_j, d_ghash_a, d_ghash2_i, d_ghash2_j, d_ghash2_a,
                      d_ic, d_jc, d_c);

   hypre_TFree(d_ghash_i,  HYPRE_MEMORY_DEVICE);
   hypre_TFree(d_ghash_j,  HYPRE_MEMORY_DEVICE);
   hypre_TFree(d_ghash_a,  HYPRE_MEMORY_DEVICE);
   hypre_TFree(d_ghash2_i, HYPRE_MEMORY_DEVICE);
   hypre_TFree(d_ghash2_j, HYPRE_MEMORY_DEVICE);
   hypre_TFree(d_ghash2_a, HYPRE_MEMORY_DEVICE);
   hypre_TFree(d_js,       HYPRE_MEMORY_DEVICE);
   hypre_TFree(d_as,       HYPRE_MEMORY_DEVICE);

   *d_ic_out = d_ic;
   *d_jc_out = d_jc;
   *d_c_out  = d_c;
   *nnzC     = nnzC_gpu;

   return hypre_error_flag;
}

#endif /* HYPRE_USING_SYCL */