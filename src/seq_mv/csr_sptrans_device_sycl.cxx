/******************************************************************************
 * Copyright 1998-2019 Lawrence Livermore National Security, LLC and other
 * HYPRE Project Developers. See the top-level COPYRIGHT file for details.
 *
 * SPDX-License-Identifier: (Apache-2.0 OR MIT)
 ******************************************************************************/

#include "seq_mv.h"
#include "_hypre_utilities.hpp"

#if defined(HYPRE_USING_ONEMKLSPARSE)

HYPRE_Int
hypreDevice_CSRSpTransOnemklsparse(HYPRE_Int   m,        HYPRE_Int   n,        HYPRE_Int       nnzA,
                                   HYPRE_Int  *d_ia,     HYPRE_Int      *d_ja,     HYPRE_Complex  *d_aa,
                                   HYPRE_Int **d_ic_out, HYPRE_Int **d_jc_out, HYPRE_Complex **d_ac_out,
                                   HYPRE_Int   want_data)
{
   hypre_error_w_msg(HYPRE_ERROR_GENERIC, "hypreDevice_CSRSpTransOnemklsparse not implemented for onemkl::SPARSE!\n");
   return hypre_error_flag;
}

#endif // #if defined(HYPRE_USING_ONEMKLSPARSE)


#if defined(HYPRE_USING_SYCL)

HYPRE_Int
hypreDevice_CSRSpTrans(HYPRE_Int   m,        HYPRE_Int   n,        HYPRE_Int       nnzA,
                       HYPRE_Int  *d_ia,     HYPRE_Int  *d_ja,     HYPRE_Complex  *d_aa,
                       HYPRE_Int **d_ic_out, HYPRE_Int **d_jc_out, HYPRE_Complex **d_ac_out,
                       HYPRE_Int   want_data)
{
   /* trivial case */
   if (nnzA == 0)
   {
      *d_ic_out = hypre_CTAlloc(HYPRE_Int, n + 1, HYPRE_MEMORY_DEVICE);
      *d_jc_out = hypre_CTAlloc(HYPRE_Int,     0, HYPRE_MEMORY_DEVICE);
      *d_ac_out = hypre_CTAlloc(HYPRE_Complex, 0, HYPRE_MEMORY_DEVICE);

      return hypre_error_flag;
   }

#ifdef HYPRE_PROFILE
   hypre_profile_times[HYPRE_TIMER_ID_SPTRANS] -= hypre_MPI_Wtime();
#endif

   HYPRE_Int *d_jt, *d_it, *d_pm, *d_ic, *d_jc;
   HYPRE_Complex *d_ac = nullptr;
   HYPRE_Int *mem_work = hypre_TAlloc(HYPRE_Int, 3*nnzA, HYPRE_MEMORY_DEVICE);

   /* allocate C */
   d_jc = hypre_TAlloc(HYPRE_Int, nnzA, HYPRE_MEMORY_DEVICE);
   if (want_data)
   {
      d_ac = hypre_TAlloc(HYPRE_Complex, nnzA, HYPRE_MEMORY_DEVICE);
   }

   /* permutation vector */
   //d_pm = hypre_TAlloc(HYPRE_Int, nnzA, HYPRE_MEMORY_DEVICE);
   d_pm = mem_work;

   /* expansion: A's row idx */
   //d_it = hypre_TAlloc(HYPRE_Int, nnzA, HYPRE_MEMORY_DEVICE);
   d_it = d_pm + nnzA;
   hypreDevice_CsrRowPtrsToIndices_v2(m, nnzA, d_ia, d_it);

   /* a copy of col idx of A */
   //d_jt = hypre_TAlloc(HYPRE_Int, nnzA, HYPRE_MEMORY_DEVICE);
   d_jt = d_it + nnzA;
   hypre_TMemcpy(d_jt, d_ja, HYPRE_Int, nnzA, HYPRE_MEMORY_DEVICE, HYPRE_MEMORY_DEVICE);

   /* sort: by col */
   HYPRE_SYCL_CALL( sycl_iota(d_pm, d_pm + nnzA) );

   auto zipped_begin = oneapi::dpl::make_zip_iterator(d_jt, d_pm);
   HYPRE_ONEDPL_CALL(std::stable_sort, zipped_begin, zipped_begin + nnzA,
                     [](auto lhs, auto rhs) { return get<0>(lhs) < get<0>(rhs); } ); // thrust::stable_sort_by_key

   HYPRE_ONEDPL_CALL(gather, d_pm, d_pm + nnzA, d_it, d_jc);
   if (want_data)
   {
      HYPRE_ONEDPL_CALL(gather, d_pm, d_pm + nnzA, d_aa, d_ac);
   }

   /* convert into ic: row idx --> row ptrs */
   d_ic = hypreDevice_CsrRowIndicesToPtrs(n, nnzA, d_jt);

#ifdef HYPRE_DEBUG
   HYPRE_Int nnzC;
   hypre_TMemcpy(&nnzC, &d_ic[n], HYPRE_Int, 1, HYPRE_MEMORY_HOST, HYPRE_MEMORY_DEVICE);
   hypre_assert(nnzC == nnzA);
#endif

   /*
   hypre_TFree(d_jt, HYPRE_MEMORY_DEVICE);
   hypre_TFree(d_it, HYPRE_MEMORY_DEVICE);
   hypre_TFree(d_pm, HYPRE_MEMORY_DEVICE);
   */
   hypre_TFree(mem_work, HYPRE_MEMORY_DEVICE);

   *d_ic_out = d_ic;
   *d_jc_out = d_jc;
   *d_ac_out = d_ac;

#ifdef HYPRE_PROFILE
   hypre_HandleSyclComputeQueue(hypre_handle())->wait();
   hypre_profile_times[HYPRE_TIMER_ID_SPTRANS] += hypre_MPI_Wtime();
#endif

   return hypre_error_flag;
}

#endif /* HYPRE_USING_SYCL */
