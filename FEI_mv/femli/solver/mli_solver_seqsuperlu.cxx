/*BHEADER**********************************************************************
 * (c) 2003   The Regents of the University of California
 *
 * See the file COPYRIGHT_and_DISCLAIMER for a complete copyright
 * notice, contact person, and disclaimer.
 *
 *********************************************************************EHEADER*/

/* **************************************************************************** 
 * -- SuperLU routine (version 1.1) --
 * Univ. of California Berkeley, Xerox Palo Alto Research Center, 
 * and Lawrence Berkeley National Lab.
 * ************************************************************************* */

#ifdef MLI_SUPERLU

#include <string.h>
#include "mli_solver_seqsuperlu.h"

/* ****************************************************************************
 * constructor 
 * --------------------------------------------------------------------------*/

MLI_Solver_SeqSuperLU::MLI_Solver_SeqSuperLU(char *name) : MLI_Solver(name)
{
   permRs_       = NULL;
   permCs_       = NULL;
   mliAmat_      = NULL;
   factorized_   = 0;
   localNRows_   = 0;
   nSubProblems_ = 0;
   subProblemRowSizes_   = NULL;
   subProblemRowIndices_ = NULL;
   numColors_ = 0;
   myColors_  = NULL;
}

/* ****************************************************************************
 * destructor 
 * --------------------------------------------------------------------------*/

MLI_Solver_SeqSuperLU::~MLI_Solver_SeqSuperLU()
{
   int iP;

   for ( iP = 0; iP < nSubProblems_; iP++ )
   {
      if ( permRs_[iP] != NULL ) 
      {
         Destroy_SuperNode_Matrix(&(superLU_Lmats[iP]));
         Destroy_CompCol_Matrix(&(superLU_Umats[iP]));
      }
   }
   if ( permRs_ != NULL ) 
   {
      for ( iP = 0; iP < nSubProblems_; iP++ )
         if ( permRs_[iP] != NULL ) delete [] permRs_[iP];
      delete [] permRs_;
   }
   if ( permCs_ != NULL ) 
   {
      for ( iP = 0; iP < nSubProblems_; iP++ )
         if ( permCs_[iP] != NULL ) delete [] permCs_[iP];
      delete [] permCs_;
   }
   if ( subProblemRowSizes_ != NULL ) delete [] subProblemRowSizes_;
   if ( subProblemRowIndices_ != NULL ) 
   {
      for (iP = 0; iP < nSubProblems_; iP++)
         if (subProblemRowIndices_[iP] != NULL) 
            delete [] subProblemRowIndices_[iP];
      delete [] subProblemRowIndices_;
   }
   if ( myColors_ != NULL ) delete [] myColors_;
}

/* ****************************************************************************
 * setup 
 * --------------------------------------------------------------------------*/

int MLI_Solver_SeqSuperLU::setup( MLI_Matrix *Amat )
{
   int      nrows, iP, startRow, nnz, *csrIA, *csrJA, *cscJA, *cscIA;
   int      irow, icol, *rowArray, *countArray, colNum, index, nSubRows;
   int      *etree, permcSpec, lwork, panelSize, relax, info, rowCnt;
   double   *csrAA, *cscAA, diagPivotThresh, dropTol;
   char     refact[1];
   hypre_ParCSRMatrix   *hypreA;
   hypre_CSRMatrix      *ADiag;
   SuperMatrix          AC, superLU_Amat;
   extern SuperLUStat_t SuperLUStat;

   /* ---------------------------------------------------------------
    * fetch matrix
    * -------------------------------------------------------------*/

   if ( nSubProblems_ > 100 )
   {
      printf("MLI_Solver_SeqSuperLU::setup ERROR - over 100 subproblems.\n");
      exit(1);
   }
   mliAmat_ = Amat;
   if ( strcmp( mliAmat_->getName(), "HYPRE_ParCSR" ) )
   {
      printf("MLI_Solver_SeqSuperLU::setup ERROR - not HYPRE_ParCSR(%s).\n",
             mliAmat_->getName());
      exit(1);
   }
   hypreA = (hypre_ParCSRMatrix *) mliAmat_->getMatrix();

   /* ---------------------------------------------------------------
    * fetch matrix
    * -------------------------------------------------------------*/
 
   ADiag = hypre_ParCSRMatrixDiag(hypreA);
   csrAA = hypre_CSRMatrixData(ADiag);
   csrIA = hypre_CSRMatrixI(ADiag);
   csrJA = hypre_CSRMatrixJ(ADiag);
   nrows = hypre_CSRMatrixNumRows(ADiag);
   nnz   = hypre_CSRMatrixNumNonzeros(ADiag);
   startRow = hypre_ParCSRMatrixFirstRowIndex(hypreA);
   localNRows_ = nrows;

   /* ---------------------------------------------------------------
    * set up coloring and then take out overlap subdomains 
    * -------------------------------------------------------------*/

//   setupBlockColoring();
myColors_ = new int[nSubProblems_];
numColors_ = 1;
for (iP = 0; iP < nSubProblems_; iP++) myColors_[iP] = 0;
printf("nSubProblems = %d\n", nSubProblems_);
#if 0
   if (nSubProblems_ > 0)
   {
      int *domainNumArray = new int[nrows];
      for (iP = nSubProblems_-1; iP >= 0; iP--)
      {
         for (irow = 0; irow < subProblemRowSizes_[iP]; irow++)
            domainNumArray[subProblemRowIndices_[iP][irow]] = iP;
         delete [] subProblemRowIndices_[iP];
      }
      delete [] subProblemRowSizes_;
      delete [] subProblemRowIndices_;
      subProblemRowSizes_ = new int[nSubProblems_];
      subProblemRowIndices_ = new int*[nSubProblems_];
      for (iP = 0; iP < nSubProblems_; iP++) subProblemRowSizes_[iP] = 0;
      for (irow = 0; irow < nrows; irow++)
         subProblemRowSizes_[domainNumArray[irow]]++;
      for (iP = 0; iP < nSubProblems_; iP++) 
         subProblemRowIndices_[iP] = new int[subProblemRowSizes_[iP]];
      for (iP = 0; iP < nSubProblems_; iP++) subProblemRowSizes_[iP] = 0;
      for (irow = 0; irow < nrows; irow++)
      {
         index = domainNumArray[irow];
         subProblemRowIndices_[index][subProblemRowSizes_[index]++] = irow;
      } 
      delete [] domainNumArray;
   } 
#endif

   /* ---------------------------------------------------------------
    * allocate space
    * -------------------------------------------------------------*/
 
   permRs_ = new int*[nSubProblems_];
   permCs_ = new int*[nSubProblems_];

   /* ---------------------------------------------------------------
    * conversion from CSR to CSC 
    * -------------------------------------------------------------*/

   countArray = new int[nrows];
   rowArray = new int[nrows];

#if 0
FILE *fp = fopen("matfile","w");
for (irow = 0; irow < nrows; irow++)
for (icol = csrIA[irow]; icol < csrIA[irow+1]; icol++)
fprintf(fp,"%8d %8d %25.16e\n",irow,csrJA[icol],csrAA[icol]);
fclose(fp);
#endif

   for ( iP = 0; iP < nSubProblems_; iP++ )
   {
      if ( subProblemRowIndices_ != NULL )
      {
         nSubRows = subProblemRowSizes_[iP]; 
         for (irow = 0; irow < nrows; irow++) rowArray[irow] = -1;
         rowCnt = 0;
         for (irow = 0; irow < nSubRows; irow++) 
         {
            index = subProblemRowIndices_[iP][irow] - startRow;
            if (index >= 0 && index < nrows) rowArray[index] = rowCnt++;
         }
         for ( irow = 0; irow < nSubRows; irow++ ) countArray[irow] = 0;
         rowCnt = 0;
         for ( irow = 0; irow < nrows; irow++ ) 
         {
            if ( rowArray[irow] >= 0 )
            {
               for ( icol = csrIA[irow]; icol < csrIA[irow+1]; icol++ ) 
               {
                  index = csrJA[icol];
                  if (rowArray[index] >= 0) countArray[rowArray[index]]++;
               }
            }
         }
         nnz = 0;
         for ( irow = 0; irow < nSubRows; irow++ ) nnz += countArray[irow];
         cscJA = (int *)    malloc( (nSubRows+1) * sizeof(int) );
         cscIA = (int *)    malloc( nnz * sizeof(int) );
         cscAA = (double *) malloc( nnz * sizeof(double) );
         cscJA[0] = 0;
         nnz = 0;
         for ( icol = 1; icol <= nSubRows; icol++ ) 
         {
            nnz += countArray[icol-1]; 
            cscJA[icol] = nnz;
         }
         for ( irow = 0; irow < nrows; irow++ )
         {
            if ( rowArray[irow] >= 0 )
            {
               for ( icol = csrIA[irow]; icol < csrIA[irow+1]; icol++ ) 
               {
                  colNum = rowArray[csrJA[icol]];
                  if ( colNum >= 0) 
                  {
                     index  = cscJA[colNum]++;
                     cscIA[index] = rowArray[irow];
                     cscAA[index] = csrAA[icol];
                  }
               }
            }
         }
         cscJA[0] = 0;
         nnz = 0;
         for ( icol = 1; icol <= nSubRows; icol++ ) 
         {
            nnz += countArray[icol-1]; 
            cscJA[icol] = nnz;
         }
         dCreate_CompCol_Matrix(&superLU_Amat, nSubRows, nSubRows, 
                  cscJA[nSubRows], cscAA, cscIA, cscJA, NC, D_D, GE);
         *refact = 'N';
         etree   = new int[nSubRows];
         permCs_[iP]  = new int[nSubRows];
         permRs_[iP]  = new int[nSubRows];
         permcSpec = 0;
         get_perm_c(permcSpec, &superLU_Amat, permCs_[iP]);
         sp_preorder(refact, &superLU_Amat, permCs_[iP], etree, &AC);
         diagPivotThresh = 1.0;
         dropTol = 0.0;
         panelSize = sp_ienv(1);
         relax = sp_ienv(2);
         StatInit(panelSize, relax);
         lwork = 0;
         dgstrf(refact, &AC, diagPivotThresh, dropTol, relax, panelSize,
                etree,NULL,lwork,permRs_[iP],permCs_[iP],
                &(superLU_Lmats[iP]),&(superLU_Umats[iP]),&info);
         Destroy_CompCol_Permuted(&AC);
         Destroy_CompCol_Matrix(&superLU_Amat);
         delete [] etree;
         StatFree();
      }
      else
      {
         for (irow = 0; irow < nrows; irow++) rowArray[irow] = 1;
         for ( irow = 0; irow < nrows; irow++ ) countArray[irow] = 0;
         for ( irow = 0; irow < nrows; irow++ ) 
            for ( icol = csrIA[irow]; icol < csrIA[irow+1]; icol++ ) 
               countArray[csrJA[icol]]++;
         cscJA = (int *)    malloc( (nrows+1) * sizeof(int) );
         cscIA = (int *)    malloc( nnz * sizeof(int) );
         cscAA = (double *) malloc( nnz * sizeof(double) );
         cscJA[0] = 0;
         nnz = 0;
         for ( icol = 1; icol <= nrows; icol++ ) 
         {
            nnz += countArray[icol-1]; 
            cscJA[icol] = nnz;
         }
         for ( irow = 0; irow < nrows; irow++ )
         {
            for ( icol = csrIA[irow]; icol < csrIA[irow+1]; icol++ ) 
            {
               colNum = csrJA[icol];
               index  = cscJA[colNum]++;
               cscIA[index] = irow;
               cscAA[index] = csrAA[icol];
            }
         }
         cscJA[0] = 0;
         nnz = 0;
         for ( icol = 1; icol <= nrows; icol++ ) 
         {
            nnz += countArray[icol-1]; 
            cscJA[icol] = nnz;
         }
         dCreate_CompCol_Matrix(&superLU_Amat, nrows, nrows, cscJA[nrows], 
                                cscAA, cscIA, cscJA, NC, D_D, GE);
         *refact = 'N';
         etree = new int[nrows];
         permCs_[iP]  = new int[nrows];
         permRs_[iP]  = new int[nrows];
         permcSpec = 0;
         get_perm_c(permcSpec, &superLU_Amat, permCs_[iP]);
         sp_preorder(refact, &superLU_Amat, permCs_[iP], etree, &AC);
         diagPivotThresh = 1.0;
         dropTol = 0.0;
         panelSize = sp_ienv(1);
         relax = sp_ienv(2);
         StatInit(panelSize, relax);
         lwork = 0;
         dgstrf(refact, &AC, diagPivotThresh, dropTol, relax, panelSize,
                etree,NULL,lwork,permRs_[iP],permCs_[iP],&(superLU_Lmats[iP]),
                &(superLU_Umats[iP]),&info);
         Destroy_CompCol_Permuted(&AC);
         Destroy_CompCol_Matrix(&superLU_Amat);
         delete [] etree;
         StatFree();
      }
   }
   factorized_ = 1;
   delete [] countArray;
   delete [] rowArray;
   return 0;
}

/* ****************************************************************************
 * This subroutine calls the SuperLU subroutine to perform LU 
 * backward substitution 
 * --------------------------------------------------------------------------*/

int MLI_Solver_SeqSuperLU::solve(MLI_Vector *fIn, MLI_Vector *uIn)
{
   int    iP, iC, irow, nrows, info, nSubRows, extNCols, nprocs, endp1;
   int    jP, jcol, index, nSends, start, rowInd, *AOffdI, *AOffdJ;
   int    *ADiagI, *ADiagJ;
   double *uData, *fData, *subUData, *sBuffer, *rBuffer, res, *AOffdA;
   double *ADiagA;
   char   trans[1];
   MPI_Comm    comm;
   SuperMatrix B;
   hypre_ParVector *f, *u;
   hypre_CSRMatrix *ADiag, *AOffd;
   hypre_ParCSRMatrix  *A;
   hypre_ParCSRCommPkg *commPkg;
   hypre_ParCSRCommHandle *commHandle;

   /* -------------------------------------------------------------
    * check that the factorization has been called
    * -----------------------------------------------------------*/

   if ( ! factorized_ )
   {
      printf("MLI_Solver_SeqSuperLU::Solve ERROR - not factorized yet.\n");
      exit(1);
   }

   /* -------------------------------------------------------------
    * fetch matrix and vector parameters
    * -----------------------------------------------------------*/

   A       = (hypre_ParCSRMatrix *) mliAmat_->getMatrix();
   comm    = hypre_ParCSRMatrixComm(A);
   commPkg = hypre_ParCSRMatrixCommPkg(A);
   if ( commPkg == NULL )
   {
      hypre_MatvecCommPkgCreate((hypre_ParCSRMatrix *) A);
      commPkg = hypre_ParCSRMatrixCommPkg(A);
   }
   MPI_Comm_size(comm, &nprocs);
   ADiag    = hypre_ParCSRMatrixDiag(A);
   ADiagI   = hypre_CSRMatrixI(ADiag);
   ADiagJ   = hypre_CSRMatrixJ(ADiag);
   ADiagA   = hypre_CSRMatrixData(ADiag);
   AOffd    = hypre_ParCSRMatrixOffd(A);
   AOffdI   = hypre_CSRMatrixI(AOffd);
   AOffdJ   = hypre_CSRMatrixJ(AOffd);
   AOffdA   = hypre_CSRMatrixData(AOffd);
   extNCols = hypre_CSRMatrixNumCols(AOffd);

   nrows  = localNRows_;
   u      = (hypre_ParVector *) uIn->getVector();
   uData  = hypre_VectorData(hypre_ParVectorLocalVector(u));
   f      = (hypre_ParVector *) fIn->getVector();
   fData  = hypre_VectorData(hypre_ParVectorLocalVector(f));

   rBuffer = sBuffer = NULL;
   if (nprocs > 1)
   {
      nSends = hypre_ParCSRCommPkgNumSends(commPkg);
      if ( nSends > 0 )
         sBuffer = new double[hypre_ParCSRCommPkgSendMapStart(commPkg,nSends)];
      else sBuffer = NULL;
      if ( extNCols > 0 ) rBuffer = new double[extNCols];
   }

   /* -------------------------------------------------------------
    * collect global vector and create a SuperLU dense matrix
    * solve the problem
    * clean up 
    * -----------------------------------------------------------*/

   if ( nSubProblems_ == 1 )
   {
      for ( irow = 0; irow < nrows; irow++ ) uData[irow] = fData[irow];
      dCreate_Dense_Matrix(&B, nrows, 1, uData, nrows, DN, D_D, GE);
      *trans  = 'N';
      dgstrs (trans, &(superLU_Lmats[0]), &(superLU_Umats[0]), permRs_[0], 
              permCs_[0], &B, &info);
      Destroy_SuperMatrix_Store(&B);
      return info;
   }

   /* -------------------------------------------------------------
    * if more than 1 subProblems
    * -----------------------------------------------------------*/

   subUData = new double[nrows];
   for ( iC = 0; iC < numColors_; iC++ )
   {
      if (nprocs > 1 && iC > 0)
      {
         index = 0;
         for (iP = 0; iP < nSends; iP++)
         {
            start = hypre_ParCSRCommPkgSendMapStart(commPkg, iP);
            endp1 = hypre_ParCSRCommPkgSendMapStart(commPkg, iP+1);
            for (jP = start; jP < endp1; jP++)
               sBuffer[index++]
                      = uData[hypre_ParCSRCommPkgSendMapElmt(commPkg,jP)];
         }
         commHandle = hypre_ParCSRCommHandleCreate(1,commPkg,sBuffer,rBuffer);
         hypre_ParCSRCommHandleDestroy(commHandle);
         commHandle = NULL;
      }
      for ( iP = 0; iP < nSubProblems_; iP++ ) 
      {
         if ( iC == myColors_[iP] )
         {
            for (irow = 0; irow < subProblemRowSizes_[iP]; irow++)
            {
               rowInd = subProblemRowIndices_[iP][irow];
               res    = fData[rowInd];
               for (jcol = ADiagI[rowInd]; jcol < ADiagI[rowInd+1]; jcol++)
                  res -= ADiagA[jcol] * uData[ADiagJ[jcol]];
               for (jcol = AOffdI[rowInd]; jcol < AOffdI[rowInd+1]; jcol++)
                  res -= AOffdA[jcol] * rBuffer[AOffdJ[jcol]];
               subUData[irow] = res;
            }
            nSubRows = subProblemRowSizes_[iP];
            dCreate_Dense_Matrix(&B,nSubRows,1,subUData,nSubRows,DN,D_D,GE);
            *trans  = 'N';
            dgstrs(trans,&(superLU_Lmats[iP]),&(superLU_Umats[iP]),
                   permRs_[iP],permCs_[iP],&B,&info);
            Destroy_SuperMatrix_Store(&B);
            for ( irow = 0; irow < nSubRows; irow++ ) 
               uData[subProblemRowIndices_[iP][irow]] += subUData[irow];
         }
      }
   }
   if (sBuffer != NULL) delete [] sBuffer;
   if (rBuffer != NULL) delete [] rBuffer;
   return info;
}

/******************************************************************************
 * set SGS parameters
 *---------------------------------------------------------------------------*/

int MLI_Solver_SeqSuperLU::setParams(char *paramString, int argc, char **argv)
{
   int    i, j, *iArray, **iArray2;
   char   param1[100];

   sscanf(paramString, "%s", param1);
   if ( !strcmp(param1, "setSubProblems") )
   {
      if ( argc != 3 ) 
      {
         printf("MLI_Solver_SeqSuperLU::setParams ERROR : needs 3 arg.\n");
         return 1;
      }
      if (subProblemRowSizes_ != NULL) delete [] subProblemRowSizes_;
      subProblemRowSizes_ = NULL; 
      if (subProblemRowIndices_ != NULL) 
      {
         for (i = 0; i < nSubProblems_; i++) 
            if (subProblemRowIndices_[i] != NULL)
               delete [] subProblemRowIndices_[i];
         subProblemRowIndices_ = NULL; 
      }
      nSubProblems_ = *(int *) argv[0];
printf("SeqSuperLU setParam : nSubProblems = %d\n", nSubProblems_);
      if (nSubProblems_ <= 0) nSubProblems_ = 1;
      if (nSubProblems_ > 1)
      {
         iArray = (int *) argv[1];
         subProblemRowSizes_ = new int[nSubProblems_];; 
         for (i = 0; i < nSubProblems_; i++) subProblemRowSizes_[i] = iArray[i];
         iArray2 = (int **) argv[2];
         subProblemRowIndices_ = new int*[nSubProblems_];; 
         for (i = 0; i < nSubProblems_; i++) 
         {
            subProblemRowIndices_[i] = new int[subProblemRowSizes_[i]]; 
            for (j = 0; j < subProblemRowSizes_[i]; j++) 
               subProblemRowIndices_[i][j] = iArray2[i][j];
         }
      }
   }
   else
   {   
      printf("MLI_Solver_SeqSuperLU::setParams - parameter not recognized.\n");
      printf("                 Params = %s\n", paramString);
      return 1;
   }
   return 0;
}

/******************************************************************************
 * multicoloring 
 *---------------------------------------------------------------------------*/

int MLI_Solver_SeqSuperLU::setupBlockColoring()
{
   int                 i, j, k, nSends, mypid, nprocs, myRowOffset, nEntries;
   int                 *procNRows, gNRows, *globalGI, *globalGJ; 
   int                 *localGI, *localGJ, *offsets, globalOffset, gRowCnt; 
   int                 searchIndex, searchStatus;
   MPI_Comm            comm;
   hypre_ParCSRMatrix     *A;
   hypre_ParCSRCommPkg    *commPkg;
   hypre_ParCSRCommHandle *commHandle;
   hypre_CSRMatrix        *AOffd;

   /*---------------------------------------------------------------*/
   /* fetch matrix                                                  */
   /*---------------------------------------------------------------*/

   A       = (hypre_ParCSRMatrix *) mliAmat_->getMatrix();
   comm    = hypre_ParCSRMatrixComm(A);
   commPkg = hypre_ParCSRMatrixCommPkg(A);
   if ( commPkg == NULL )
   {
      hypre_MatvecCommPkgCreate((hypre_ParCSRMatrix *) A);
      commPkg = hypre_ParCSRMatrixCommPkg(A);
   }
   MPI_Comm_rank(comm, &mypid);
   MPI_Comm_size(comm, &nprocs);

   /*---------------------------------------------------------------*/
   /* construct local graph ==> (nSubProblems_, GDiagI, GDiagJ)     */
   /*---------------------------------------------------------------*/

   int *sortIndices;
   int *graphMatrix = new int[nSubProblems_*nSubProblems_];
   for (i = 0; i < nSubProblems_*nSubProblems_; i++) graphMatrix[i] = 0;
   for (i = 0; i < nSubProblems_; i++) 
   {
      for (j = i+1; j < nSubProblems_; j++) 
      {
         nEntries = subProblemRowSizes_[i] + subProblemRowSizes_[j];
         sortIndices = new int[nEntries];
         nEntries = subProblemRowSizes_[i];
         for (k = 0; k < subProblemRowSizes_[i]; k++) 
            sortIndices[k] = subProblemRowIndices_[i][k];
         for (k = 0; k < subProblemRowSizes_[j]; k++) 
            sortIndices[nEntries+k] = subProblemRowIndices_[j][k];
         nEntries += subProblemRowSizes_[j];
         MLI_Utils_IntQSort2(sortIndices,NULL,0,nEntries-1);
         for (k = 1; k < nEntries; k++) 
         {
            if (sortIndices[k] == sortIndices[k-1]) 
            {
               graphMatrix[i*nSubProblems_+j] = 1;
               graphMatrix[j*nSubProblems_+i] = 1;
               break;
            }
         }
         delete [] sortIndices;
      }
   }
   nEntries = 0;
   for (i = 0; i < nSubProblems_*nSubProblems_; i++) 
      if (graphMatrix[i] != 0) nEntries++;
   int *GDiagI = new int[nSubProblems_+1];
   int *GDiagJ = new int[nEntries];
   nEntries = 0;
   GDiagI[0] = nEntries;
   for (i = 0; i < nSubProblems_; i++) 
   {
      for (j = 0; j < nSubProblems_; j++) 
         if (graphMatrix[i*nSubProblems_+j] == 1) GDiagJ[nEntries++] = j;
      GDiagI[i+1] = nEntries;
   }
printf("print graph\n");
for (i = 0; i < nSubProblems_; i++) 
for (j = GDiagI[i]; j < GDiagI[i+1]; j++) 
printf("Graph %d = %d\n",i, GDiagJ[j]);
   delete [] graphMatrix;

   /*---------------------------------------------------------------*/
   /* compute processor number of rows and my row offset            */
   /* (myRowOffset, proNRows)                                       */
   /*---------------------------------------------------------------*/

   procNRows = new int[nprocs];
   MPI_Allgather(&nSubProblems_,1,MPI_INT,procNRows,1,MPI_INT,comm);
   gNRows = 0;
   for (i = 0; i < nprocs; i++) gNRows += procNRows[i];
   myRowOffset = 0;
   for (i = 0; i < mypid; i++) myRowOffset += procNRows[i];
   for (i = 0; i < GDiagI[nSubProblems_]; i++) GDiagJ[i] += myRowOffset;

   /*---------------------------------------------------------------*/
   /* construct local off-diagonal graph                            */
   /*---------------------------------------------------------------*/

   int    extNCols, mapStart, mapEnd, mapIndex, *AOffdI, *AOffdJ;
   int    localNRows;
   double *sBuffer=NULL, *rBuffer=NULL;

   localNRows = hypre_CSRMatrixNumRows(hypre_ParCSRMatrixDiag(A));
   AOffd    = hypre_ParCSRMatrixOffd(A);
   AOffdI   = hypre_CSRMatrixI(AOffd);
   AOffdJ   = hypre_CSRMatrixJ(AOffd);
   extNCols = hypre_CSRMatrixNumCols(AOffd);
   nSends   = hypre_ParCSRCommPkgNumSends(commPkg);
   if (extNCols > 0) rBuffer = new double[extNCols];
   if (nSends > 0)
      sBuffer = new double[hypre_ParCSRCommPkgSendMapStart(commPkg,nSends)];
   mapIndex  = 0;
   for (i = 0; i < nSends; i++)
   {
      mapStart = hypre_ParCSRCommPkgSendMapStart(commPkg, i);
      mapEnd   = hypre_ParCSRCommPkgSendMapStart(commPkg, i+1);
      for (j=mapStart; j<mapEnd; j++)
      {
         searchIndex = hypre_ParCSRCommPkgSendMapElmt(commPkg,j);
         for (k = 0; k < nSubProblems_; k++)
         {
            searchStatus = MLI_Utils_BinarySearch(searchIndex,
                             subProblemRowIndices_[k],subProblemRowSizes_[k]);
            if (searchStatus >= 0)
            {
               sBuffer[mapIndex++] = (double) (k + myRowOffset);
               break;
            }
         }
      }
   }
   if ( nSends > 0 || extNCols > 0 )
   {
      commHandle = hypre_ParCSRCommHandleCreate(1,commPkg,sBuffer,rBuffer);
      hypre_ParCSRCommHandleDestroy(commHandle);
      commHandle = NULL;
   }
   if ( extNCols > 0 )
   {
      int indexI, indexJ;
      int *GOffdCnt = new int[nSubProblems_];
      int *GOffdJ = new int[nSubProblems_*extNCols];
      for (i = 0; i < nSubProblems_; i++) GOffdCnt[i] = 0;
      for (i = 0; i < nSubProblems_*extNCols; i++) GOffdJ[i] = -1;
      for (i = 0; i < localNRows; i++)
      {
         if ( AOffdI[i+1] > AOffdI[i] )
         {
            for (k = 0; k < nSubProblems_; k++)
            {
               indexI = MLI_Utils_BinarySearch(i,subProblemRowIndices_[k],
                                               subProblemRowSizes_[k]);
               if (indexI >= 0) break;
            }
            for (j = AOffdI[i]; j < AOffdJ[i+1]; j++)
            {
               indexJ = (int) rBuffer[i];
               GOffdJ[extNCols*k+AOffdJ[j]] = indexJ;
            }
         }
      }
      int totalNNZ = GDiagI[nSubProblems_];
      for (i = 0; i < nSubProblems_; i++) totalNNZ += GOffdCnt[i];
      localGI = new int[nSubProblems_+1];
      localGJ = new int[totalNNZ];
      totalNNZ = 0;
      localGI[0] = totalNNZ;
      for (i = 0; i < nSubProblems_; i++) 
      {
         for (j = GDiagI[i]; j < GDiagI[i+1]; j++) 
            localGJ[totalNNZ++] = GDiagJ[j];
         for (j = 0; j < extNCols; j++) 
            if (GOffdJ[i*extNCols+j] >= 0) 
               localGJ[totalNNZ++] = GOffdJ[i*extNCols+j];
         localGI[i+1] = totalNNZ;
      } 
      delete [] GDiagI;
      delete [] GDiagJ;
      delete [] GOffdCnt;
      delete [] GOffdJ;
   }
   else
   {
      localGI = GDiagI;
      localGJ = GDiagJ;
   }
   if (sBuffer != NULL) delete [] sBuffer;
   if (rBuffer != NULL) delete [] rBuffer;
   
   /*---------------------------------------------------------------*/
   /* form global graph (gNRows, globalGI, globalGJ)                */
   /*---------------------------------------------------------------*/

   globalGI = new int[gNRows+1];
   offsets  = new int[nprocs+1];
   offsets[0] = 0;
   for (i = 1; i <= nprocs; i++)
      offsets[i] = offsets[i-1] + procNRows[i-1];
   MPI_Allgatherv(&localGI[1], nSubProblems_, MPI_INT, &globalGI[1],
                  procNRows, offsets, MPI_INT, comm);
   delete [] offsets;
   globalOffset = 0; 
   gRowCnt = 1;
   globalGI[0] = globalOffset;
   for (i = 0; i < nprocs; i++)
   {
      for (j = 0; j < procNRows[i]; j++)
      {
         globalGI[gRowCnt] = globalOffset + globalGI[gRowCnt]; 
         gRowCnt++;
      }
      globalOffset += globalGI[gRowCnt-1];
   }
   globalGJ = new int[globalOffset];
   int *recvCnts = new int[nprocs+1];
   globalOffset = 0;
   for (i = 0; i < nprocs; i++)
   {
      gRowCnt = globalOffset;
      globalOffset = globalGI[gRowCnt+procNRows[i]];
      recvCnts[i] = globalOffset - gRowCnt;
   }
   offsets = new int[nprocs+1];
   offsets[0] = 0;
   for (i = 1; i <= nprocs; i++)
      offsets[i] = offsets[i-1] + recvCnts[i-1];
   nEntries = localGI[nSubProblems_];
   MPI_Allgatherv(localGJ, nEntries, MPI_INT, globalGJ, recvCnts, 
                  offsets, MPI_INT, comm);
   delete [] offsets;
   delete [] recvCnts;
   delete [] localGI;
   delete [] localGJ;

#if 0
   if ( mypid == 0 )
   {
      for ( i = 0; i < gNRows; i++ )
         for ( j = globalGI[i]; j < globalGI[i+1]; j++ )
            printf("Graph(%d,%d)\n", i, globalGJ[j]);
   }
#endif

   /*---------------------------------------------------------------*/
   /* start coloring                                                */
   /*---------------------------------------------------------------*/

   int *colors = new int[gNRows];
   int *colorsAux = new int[gNRows];
   int gIndex, gColor;

   for ( i = 0; i < gNRows; i++ ) colors[i] = colorsAux[i] = -1;
   for ( i = 0; i < gNRows; i++ )
   {
      for ( j = globalGI[i]; j < globalGI[i+1]; j++ )
      {
         gIndex = globalGJ[j];
         gColor = colors[gIndex];
         if ( gColor >= 0 ) colorsAux[gColor] = 1;
      }
      for ( j = 0; j < gNRows; j++ ) 
         if ( colorsAux[j] < 0 ) break;
      colors[i] = j;
      for ( j = globalGI[i]; j < globalGI[i+1]; j++ )
      {
         gIndex = globalGJ[j];
         gColor = colors[gIndex];
         if ( gColor >= 0 ) colorsAux[gColor] = -1;
      }
   }
   delete [] colorsAux;
   myColors_ = new int[nSubProblems_];
   for ( j = myRowOffset; j < myRowOffset+nSubProblems_; j++ ) 
      myColors_[j-myRowOffset] = colors[j]; 
   numColors_ = 0;
   for ( j = 0; j < gNRows; j++ ) 
      if ( colors[j]+1 > numColors_ ) numColors_ = colors[j]+1;
   delete [] colors;
   if ( mypid == 0 )
      printf("\tMLI_Solver_SeqSuperLU : number of colors = %d\n",numColors_);
   return 0;
}

#endif

