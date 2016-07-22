// Copyright (c) 2016, Lawrence Livermore National Security, LLC. Produced at
// the Lawrence Livermore National Laboratory. LLNL-CODE-443211. All Rights
// reserved. See file COPYRIGHT for details.
//
// This file is part of the MFEM library. For more information and source code
// availability see http://mfem.org.
//
// MFEM is free software; you can redistribute it and/or modify it under the
// terms of the GNU Lesser General Public License (as published by the Free
// Software Foundation) version 2.1 dated February 1999.

// Author: Stefano Zampini <stefano.zampini@gmail.com>

#include "../config/config.hpp"

#ifdef MFEM_USE_MPI
#ifdef MFEM_USE_PETSC

#include "linalg.hpp"
#include "../fem/fem.hpp"

#include <fstream>
#include <iomanip>
#include <cmath>
#include <cstdlib>

// Error handling
// Prints PETSc's stacktrace and then calls MFEM_ABORT
// We cannot use PETSc's CHKERRQ since it returns a PetscErrorCode
#define PCHKERRQ(obj,err)                                                  \
  if ((err)) {                                                             \
    PetscError(PetscObjectComm((PetscObject)obj),__LINE__,_MFEM_FUNC_NAME, \
               __FILE__,err,PETSC_ERROR_REPEAT,NULL);                      \
    MFEM_ABORT("Error in PETSc. See stacktrace above.");                   \
  }

#define CCHKERRQ(comm,err)                               \
  if ((err)) {                                           \
    PetscError(comm,__LINE__,_MFEM_FUNC_NAME,            \
               __FILE__,err,PETSC_ERROR_REPEAT,NULL);    \
    MFEM_ABORT("Error in PETSc. See stacktrace above."); \
  }

// prototype for auxiliary functions
static PetscErrorCode MatConvert_hypreParCSR_AIJ(hypre_ParCSRMatrix*,Mat*,PetscInt**,PetscInt**,PetscScalar**,PetscInt**,PetscInt**,PetscScalar**);

// use global scope ierr to check PETSc errors inside mfem calls
PetscErrorCode ierr;

using namespace std;

namespace mfem
{

// PetscParVector methods
void PetscParVector::_SetDataAndSize_()
{
   const PetscScalar *array;
   PetscInt           n;

   ierr = VecGetArrayRead(x,&array);PCHKERRQ(x,ierr);
   ierr = VecGetLocalSize(x,&n);PCHKERRQ(x,ierr);
   SetDataAndSize((PetscScalar*)array,n);
   ierr = VecRestoreArrayRead(x,&array);PCHKERRQ(x,ierr);
}

PetscInt PetscParVector::GlobalSize()
{
   PetscInt N;
   ierr = VecGetSize(x,&N);PCHKERRQ(x,ierr);
   return N;
}

PetscParVector::PetscParVector(MPI_Comm comm, PetscInt glob_size,
                               PetscInt *col) : Vector()
{
   ierr = VecCreate(comm,&x);CCHKERRQ(comm,ierr);
   if (col) {
      PetscMPIInt myid;
      MPI_Comm_rank(comm, &myid);
      ierr = VecSetSizes(x,col[myid+1]-col[myid],PETSC_DECIDE);PCHKERRQ(x,ierr);
   } else {
      ierr = VecSetSizes(x,PETSC_DECIDE,glob_size);PCHKERRQ(x,ierr);
   }
   ierr = VecSetType(x,VECSTANDARD);PCHKERRQ(x,ierr);
   _SetDataAndSize_();
}

PetscParVector::~PetscParVector()
{
   MPI_Comm comm = PetscObjectComm((PetscObject)x);
   ierr = VecDestroy(&x);CCHKERRQ(comm,ierr);
}

PetscParVector::PetscParVector(MPI_Comm comm, PetscInt glob_size,
                               PetscScalar *_data, PetscInt *col) : Vector()
{
   if (col) {
      PetscMPIInt myid;
      MPI_Comm_rank(comm, &myid);
      ierr = VecCreateMPIWithArray(comm,1,col[myid+1]-col[myid],PETSC_DECIDE,_data,&x);CCHKERRQ(comm,ierr)
   } else {
      ierr = VecCreateMPIWithArray(comm,1,PETSC_DECIDE,glob_size,_data,&x);CCHKERRQ(comm,ierr)
   }
   _SetDataAndSize_();
}

PetscParVector::PetscParVector(const PetscParVector &y) : Vector()
{
   ierr = VecDuplicate(y.x,&x);PCHKERRQ(x,ierr);
}

PetscParVector::PetscParVector(const PetscParMatrix &A,
                               int transpose) : Vector()
{
   MPI_Comm comm = PETSC_COMM_WORLD;
   if (!transpose)
   {
      ierr = MatCreateVecs(const_cast<PetscParMatrix&>(A),&x,NULL);CCHKERRQ(comm,ierr);
   }
   else
   {
      ierr = MatCreateVecs(const_cast<PetscParMatrix&>(A),NULL,&x);CCHKERRQ(comm,ierr);
   }
   _SetDataAndSize_();
}

PetscParVector::PetscParVector(Vec y) : Vector()
{
   ierr = PetscObjectReference((PetscObject)y);PCHKERRQ(y,ierr);
   x = y;
   _SetDataAndSize_();
}

PetscParVector::PetscParVector(ParFiniteElementSpace *pfes) : Vector()
{

   HYPRE_Int* offsets = pfes->GetTrueDofOffsets();
   MPI_Comm  comm = pfes->GetComm();
   ierr = VecCreate(comm,&x);CCHKERRQ(comm,ierr);

   PetscMPIInt myid = 0;
   if (!HYPRE_AssumedPartitionCheck())
   {
      MPI_Comm_rank(comm,&myid);
   }
   ierr = VecSetSizes(x,offsets[myid+1]-offsets[myid],PETSC_DECIDE);PCHKERRQ(x,ierr);
   ierr = VecSetType(x,VECSTANDARD);PCHKERRQ(x,ierr);
   _SetDataAndSize_();
}

HypreParVector* GetHypreParVector(Vec y)
{
   const PetscScalar *array;
   const PetscInt    *cols;
   PetscInt           N;
   HypreParVector    *out;

   ierr = VecGetSize(y,&N);PCHKERRQ(y,ierr);
   ierr = VecGetArrayRead(y,&array);PCHKERRQ(y,ierr);
   ierr = VecGetOwnershipRanges(y,&cols);PCHKERRQ(y,ierr);
   // TODO ASK
   if (!HYPRE_AssumedPartitionCheck())
   {
      out = new HypreParVector(PetscObjectComm((PetscObject)y),N,(double*)array,(HYPRE_Int*)cols);
   }
   else
   {
      PetscMPIInt myid;
      HYPRE_Int   range[2];

      MPI_Comm comm = PetscObjectComm((PetscObject)y);
      MPI_Comm_rank(comm,&myid);
      range[0] = cols[myid];
      range[1] = cols[myid+1];
      out = new HypreParVector(comm,N,(double*)array,range);
   }
   ierr = VecRestoreArrayRead(y,&array);PCHKERRQ(y,ierr);
   return out;
}

HypreParVector* GetHypreParVector(Mat B,bool transpose)
{
   const PetscInt    *cols;
   PetscInt           N;
   HypreParVector    *out;

   if (transpose)
   {
      ierr = MatGetSize(B,NULL,&N);PCHKERRQ(B,ierr);
      ierr = MatGetOwnershipRangesColumn(B,&cols);PCHKERRQ(B,ierr);
   }
   else
   {
      ierr = MatGetSize(B,&N,NULL);PCHKERRQ(B,ierr);
      ierr = MatGetOwnershipRanges(B,&cols);PCHKERRQ(B,ierr);
   }
   // TODO ASK
   if (!HYPRE_AssumedPartitionCheck())
   {
      out = new HypreParVector(PetscObjectComm((PetscObject)B),N,NULL,(HYPRE_Int*)cols);
   }
   else
   {
      PetscMPIInt myid;
      HYPRE_Int   range[2];

      MPI_Comm comm = PetscObjectComm((PetscObject)B);
      MPI_Comm_rank(comm,&myid);
      range[0] = cols[myid];
      range[1] = cols[myid+1];
      out = new HypreParVector(comm,N,NULL,range);
   }
   return out;
}

Vector * PetscParVector::GlobalVector() const
{
   VecScatter   scctx;
   Vec          vout;
   PetscScalar *array;
   PetscInt     size;

   ierr = VecScatterCreateToAll(x,&scctx,&vout);PCHKERRQ(x,ierr);
   ierr = VecScatterBegin(scctx,x,vout,INSERT_VALUES,SCATTER_FORWARD);PCHKERRQ(x,ierr);
   ierr = VecScatterEnd(scctx,x,vout,INSERT_VALUES,SCATTER_FORWARD);PCHKERRQ(x,ierr);
   ierr = VecScatterDestroy(&scctx);PCHKERRQ(x,ierr);
   ierr = VecGetArray(vout,&array);PCHKERRQ(x,ierr);
   ierr = VecGetLocalSize(vout,&size);PCHKERRQ(x,ierr);
   Array<PetscScalar> data(size);
   data.Assign(array);
   ierr = VecRestoreArray(vout,&array);PCHKERRQ(x,ierr);
   ierr = VecDestroy(&vout);PCHKERRQ(x,ierr);
   Vector *v = new Vector(data, internal::to_int(size));
   v->MakeDataOwner();
   return v;
}

PetscParVector& PetscParVector::operator=(PetscScalar d)
{
   ierr = VecSet(x,d);PCHKERRQ(x,ierr);
   return *this;
}

PetscParVector& PetscParVector::operator=(const PetscParVector &y)
{
   ierr = VecCopy(y.x,x);PCHKERRQ(x,ierr);
   return *this;
}

void PetscParVector::SetData(PetscScalar *_data)
{
   ierr = VecPlaceArray(x,_data);PCHKERRQ(x,ierr);
}

void PetscParVector::ResetData()
{
   ierr = VecResetArray(x);PCHKERRQ(x,ierr);
}

void PetscParVector::Randomize(PetscInt seed)
{
   PetscRandom rctx;

   ierr = PetscRandomCreate(PetscObjectComm((PetscObject)x),&rctx);PCHKERRQ(x,ierr);
   ierr = PetscRandomSetSeed(rctx,(unsigned long)seed);PCHKERRQ(x,ierr);
   ierr = PetscRandomSeed(rctx);PCHKERRQ(x,ierr);
   ierr = VecSetRandom(x,rctx);PCHKERRQ(x,ierr);
   ierr = PetscRandomDestroy(&rctx);PCHKERRQ(x,ierr);
}

void PetscParVector::Print(const char *fname) const
{
   if (fname) {
      PetscViewer view;

      ierr = PetscViewerASCIIOpen(PetscObjectComm((PetscObject)x),fname,&view);PCHKERRQ(x,ierr);
      ierr = VecView(x,view);PCHKERRQ(x,ierr);
      ierr = PetscViewerDestroy(&view);PCHKERRQ(x,ierr);
   } else {
      ierr = VecView(x,NULL);PCHKERRQ(x,ierr);
   }
}

// PetscParMatrix methods

PetscInt PetscParMatrix::M()
{
   PetscInt N;
   ierr = MatGetSize(A,&N,NULL);PCHKERRQ(A,ierr);
   return N;
}

PetscInt PetscParMatrix::N()
{
   PetscInt N;
   ierr = MatGetSize(A,NULL,&N);PCHKERRQ(A,ierr);
   return N;
}

void PetscParMatrix::Init()
{
   A = NULL;
   X = Y = NULL;
   dii = djj = oii = ojj = NULL;
   da = oa = NULL;
   height = width = 0;
}

PetscParMatrix::PetscParMatrix()
{
   Init();
}

PetscParMatrix::PetscParMatrix(const HypreParMatrix *hmat, bool wrap)
{
   Init();
   height = hmat->GetNumRows();
   width  = hmat->GetNumCols();
   if (wrap)
   {
      MakeWrapper(hmat,&A);
   }
   else
   {
      Convert(hmat,&A,&dii,&djj,&da,&oii,&ojj,&oa);
   }
}

typedef struct {
   HypreParMatrix *op;
   HypreParVector *xx,*yy;
} mat_shell_ctx;

#undef __FUNCT__
#define __FUNCT__ "mat_shell_apply"
static PetscErrorCode mat_shell_apply(Mat A, Vec x, Vec y)
{
   mat_shell_ctx     *ctx;
   const PetscScalar *a;
   PetscErrorCode     ierr;

   PetscFunctionBeginUser;
   ierr = MatShellGetContext(A,(void **)&ctx);PCHKERRQ(A,ierr);
   HypreParVector *xx = ctx->xx;
   HypreParVector *yy = ctx->yy;
   ierr = VecGetArrayRead(x,&a);PCHKERRQ(x,ierr);
   xx->SetData((PetscScalar*)a);
   ierr = VecRestoreArrayRead(x,&a);PCHKERRQ(x,ierr);
   ierr = VecGetArrayRead(y,&a);PCHKERRQ(x,ierr);
   yy->SetData((PetscScalar*)a);
   ierr = VecRestoreArrayRead(y,&a);PCHKERRQ(x,ierr);
   ctx->op->Mult(*xx,*yy);
   PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "mat_shell_apply_transpose"
static PetscErrorCode mat_shell_apply_transpose(Mat A, Vec x, Vec y)
{
   mat_shell_ctx     *ctx;
   const PetscScalar *a;
   PetscErrorCode     ierr;

   PetscFunctionBeginUser;
   ierr = MatShellGetContext(A,(void **)&ctx);PCHKERRQ(A,ierr);
   HypreParVector *xx = ctx->xx;
   HypreParVector *yy = ctx->yy;
   ierr = VecGetArrayRead(x,&a);PCHKERRQ(x,ierr);
   yy->SetData((PetscScalar*)a);
   ierr = VecRestoreArrayRead(x,&a);PCHKERRQ(x,ierr);
   ierr = VecGetArrayRead(y,&a);PCHKERRQ(x,ierr);
   xx->SetData((PetscScalar*)a);
   ierr = VecRestoreArrayRead(y,&a);PCHKERRQ(x,ierr);
   ctx->op->MultTranspose(*yy,*xx);
   PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "mat_shell_destroy"
static PetscErrorCode mat_shell_destroy(Mat A)
{
   mat_shell_ctx  *ctx;

   PetscFunctionBeginUser;
   ierr = MatShellGetContext(A,(void **)&ctx);PCHKERRQ(A,ierr);
   delete ctx->xx;
   delete ctx->yy;
   delete ctx;
   PetscFunctionReturn(0);
}
#undef __FUNCT__

void PetscParMatrix::Convert(const HypreParMatrix* hmat, Mat *A,
                             PetscInt** dii, PetscInt** djj,PetscScalar** da,
                             PetscInt** oii, PetscInt** ojj,PetscScalar** oa)
{
   ierr = MatConvert_hypreParCSR_AIJ(const_cast<HypreParMatrix&>(*hmat),A,dii,djj,da,oii,ojj,oa);CCHKERRQ(hmat->GetComm(),ierr);
}

void PetscParMatrix::MakeWrapper(const HypreParMatrix* hmat, Mat *A)
{
   MPI_Comm comm = hmat->GetComm();
   ierr = MatCreate(comm,A);CCHKERRQ(comm,ierr);
   PetscMPIInt myid = 0;
   // TODO ASK
   if (!HYPRE_AssumedPartitionCheck())
   {
     MPI_Comm_rank(comm,&myid);
   }
   const PetscInt *rows = hmat->RowPart();
   const PetscInt *cols = hmat->ColPart();
   ierr = MatSetSizes(*A,rows[myid+1]-rows[myid],cols[myid+1]-cols[myid],PETSC_DECIDE,PETSC_DECIDE);PCHKERRQ(A,ierr);
   ierr = MatSetType(*A,MATSHELL);PCHKERRQ(A,ierr);
   mat_shell_ctx *ctx = new mat_shell_ctx;
   ierr = MatShellSetContext(*A,(void *)ctx);PCHKERRQ(A,ierr);
   ierr = MatShellSetOperation(*A,MATOP_MULT,(void (*)())mat_shell_apply);PCHKERRQ(A,ierr);
   ierr = MatShellSetOperation(*A,MATOP_MULT_TRANSPOSE,(void (*)())mat_shell_apply_transpose);PCHKERRQ(A,ierr);
   ierr = MatShellSetOperation(*A,MATOP_DESTROY,(void (*)())mat_shell_destroy);PCHKERRQ(A,ierr);
   ierr = MatSetUp(*A);PCHKERRQ(*A,ierr);
   // create two HypreVectors in domain and range of A without allocating data
   ctx->xx = GetHypreParVector(*A,false);
   ctx->yy = GetHypreParVector(*A,true);
   ctx->op = const_cast<HypreParMatrix *>(hmat);
}

void PetscParMatrix::Destroy()
{
   MPI_Comm comm = MPI_COMM_NULL;
   if (A)
   {
      comm = PetscObjectComm((PetscObject)A);
      ierr = MatDestroy(&A);CCHKERRQ(comm,ierr);
   }
   if (X) { delete X; }
   if (Y) { delete Y; }
   if (dii && djj && da) { ierr = PetscFree3(dii,djj,da);CCHKERRQ(comm,ierr); }
   if (oii && ojj && oa) { ierr = PetscFree3(oii,ojj,oa);CCHKERRQ(comm,ierr); }
}

PetscParMatrix::PetscParMatrix(Mat a)
{
   if (!a)
   {
      MFEM_ABORT("PetscParMatrix::PetscParMatrix(Mat) : Mat si missing");
   }
   ierr = PetscObjectReference((PetscObject)a);PCHKERRQ(a,ierr);
   Init();
   A = a;
   height = M();
   width = N();
}

void PetscParMatrix::Mult(const Vector &x, Vector &y) const
{
   MFEM_ABORT("Not yet implemented.");
}

// PetscSolver
void PetscSolver::Init()
{
   ksp = NULL;
   A = NULL;
   B = X = NULL;
   wrap = true;
}

PetscSolver::PetscSolver()
{
   Init();
}

PetscSolver::PetscSolver(PetscParMatrix &_A)
{
   Init();
   SetOperator(_A);
}

PetscSolver::PetscSolver(HypreParMatrix &_A,bool wrapin)
{
   Init();
   wrap = wrapin;
   SetOperator(_A);
}

void PetscSolver::SetOperator(const Operator &op)
{
   HypreParMatrix *hA = const_cast<HypreParMatrix *>(dynamic_cast<const HypreParMatrix *>(&op));
   PetscParMatrix *pA = const_cast<PetscParMatrix *>(dynamic_cast<const PetscParMatrix *>(&op));
   if (!hA && !pA)
   {
      MFEM_ABORT("PetscSolver::SetOperator : new Operator must be a HypreParMatrix or a PetscParMatrix!");
   }
   // update base classes: Operator, Solver, PetscSolver
   MPI_Comm comm;
   PetscInt nheight,nwidth;
   if (hA)
   {
      // Create MATSHELL object or convert into PETSc AIJ format
      A = new PetscParMatrix(hA,wrap);
   }
   else
   {
      A = new PetscParMatrix(pA->A);
   }
   comm    = A->GetComm();
   nheight = A->Height();
   nwidth  = A->Width();
   if (ksp)
   {
      if (nheight != height || nwidth != width)
      {
         ierr = KSPReset(ksp);PCHKERRQ(ksp,ierr);
         if (X) delete X;
         if (B) delete B;
         B = X = NULL;
         wrap = true;
      }
   }
   else
   {
      ierr = KSPCreate(comm,&ksp);CCHKERRQ(comm,ierr);
   }
   height = nheight;
   width  = nwidth;
   ierr = KSPSetOperators(ksp,A->A,A->A);PCHKERRQ(ksp,ierr);
}

typedef struct {
   Solver         *op;
   HypreParVector *xx,*yy;
} solver_shell_ctx;


#undef __FUNCT__
#define __FUNCT__ "pc_shell_apply"
static PetscErrorCode pc_shell_apply(PC pc, Vec x, Vec y)
{
   solver_shell_ctx  *ctx;
   const PetscScalar *a;
   PetscErrorCode     ierr;

   PetscFunctionBeginUser;
   ierr = PCShellGetContext(pc,(void **)&ctx);PCHKERRQ(pc,ierr);
   HypreParVector *xx = ctx->xx;
   HypreParVector *yy = ctx->yy;
   ierr = VecGetArrayRead(x,&a);PCHKERRQ(x,ierr);
   xx->SetData((PetscScalar*)a);
   ierr = VecRestoreArrayRead(x,&a);PCHKERRQ(x,ierr);
   ierr = VecGetArrayRead(y,&a);PCHKERRQ(x,ierr);
   yy->SetData((PetscScalar*)a);
   ierr = VecRestoreArrayRead(y,&a);PCHKERRQ(x,ierr);
   ctx->op->Mult(*xx,*yy);
   PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "pc_shell_apply_transpose"
static PetscErrorCode pc_shell_apply_transpose(PC pc, Vec x, Vec y)
{
   solver_shell_ctx  *ctx;
   const PetscScalar *a;
   PetscErrorCode     ierr;

   PetscFunctionBeginUser;
   ierr = PCShellGetContext(pc,(void **)&ctx);PCHKERRQ(pc,ierr);
   HypreParVector *xx = ctx->xx;
   HypreParVector *yy = ctx->yy;
   ierr = VecGetArrayRead(x,&a);PCHKERRQ(x,ierr);
   yy->SetData((PetscScalar*)a);
   ierr = VecRestoreArrayRead(x,&a);PCHKERRQ(x,ierr);
   ierr = VecGetArrayRead(y,&a);PCHKERRQ(x,ierr);
   xx->SetData((PetscScalar*)a);
   ierr = VecRestoreArrayRead(y,&a);PCHKERRQ(x,ierr);
   ctx->op->MultTranspose(*yy,*xx);
   PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "pc_shell_setup"
static PetscErrorCode pc_shell_setup(PC pc)
{
   solver_shell_ctx *ctx;
   Mat              A;
   PetscErrorCode   ierr;

   PetscFunctionBeginUser;
   // we create the vectors here since the operator is known at this point
   ierr = PCShellGetContext(pc,(void **)&ctx);PCHKERRQ(pc,ierr);
   ierr = PCGetOperators(pc,&A,NULL);PCHKERRQ(pc,ierr);
   if (!ctx->xx)
   {
      ctx->xx = GetHypreParVector(A,false);
   }
   if (!ctx->yy)
   {
      ctx->yy = GetHypreParVector(A,true);
   }
   // TODO ask: is there a way to trigger the setup of ctx->op?
   PetscFunctionReturn(0);
}

#undef __FUNCT__
#define __FUNCT__ "pc_shell_destroy"
static PetscErrorCode pc_shell_destroy(PC pc)
{
   solver_shell_ctx *ctx;

   PetscFunctionBeginUser;
   ierr = PCShellGetContext(pc,(void **)&ctx);PCHKERRQ(pc,ierr);
   delete ctx->xx;
   delete ctx->yy;
   delete ctx;
   PetscFunctionReturn(0);
}

#undef __FUNCT__

void PetscSolver::SetPreconditioner(Solver &precond)
{
   if (!ksp)
   {
      MFEM_ABORT("PetscSolver::SetPreconditioner (...) : KSP si missing. You should call PetscSolver::SetOperator (...) first");
      return;
   }
   PC pc;

   ierr = KSPGetPC(ksp,&pc);PCHKERRQ(ksp,ierr);
   ierr = PCSetType(pc,PCSHELL);PCHKERRQ(pc,ierr);
   solver_shell_ctx *ctx = new solver_shell_ctx;
   ctx->op = &precond;
   ctx->xx = NULL;
   ctx->yy = NULL;
   ierr = PCShellSetContext(pc,(void *)ctx);PCHKERRQ(pc,ierr);
   ierr = PCShellSetApply(pc,pc_shell_apply);PCHKERRQ(pc,ierr);
   ierr = PCShellSetApplyTranspose(pc,pc_shell_apply_transpose);PCHKERRQ(pc,ierr);
   ierr = PCShellSetSetUp(pc,pc_shell_setup);PCHKERRQ(pc,ierr);
   ierr = PCShellSetDestroy(pc,pc_shell_destroy);PCHKERRQ(pc,ierr);
}

void PetscSolver::Mult(const PetscParVector &b, PetscParVector &x) const
{
   if (!ksp)
   {
      MFEM_ABORT("PetscSolver::Mult (...) : KSP si missing");
      return;
   }
   // allow user customization
   ierr = KSPSetFromOptions(ksp);PCHKERRQ(ksp,ierr);
   ierr = KSPSetUp(ksp);PCHKERRQ(ksp,ierr);
   ierr = KSPSetInitialGuessNonzero(ksp,PetscBool(iterative_mode));PCHKERRQ(ksp,ierr);
   ierr = KSPSolve(ksp,b.x,x.x);PCHKERRQ(ksp,ierr);
}

void PetscSolver::Mult(const Vector &b, Vector &x) const
{
   if (!ksp)
   {
      MFEM_ABORT("PetscSolver::Mult (...) : KSP is missing");
      return;
   }
   if (!B)
   {
      Mat pA;
      ierr = KSPGetOperators(ksp,&pA,NULL);PCHKERRQ(ksp,ierr);
      PetscParMatrix A = PetscParMatrix(pA);
      B = new PetscParVector(A,true);
   }
   if (!X)
   {
      Mat pA;
      ierr = KSPGetOperators(ksp,&pA,NULL);PCHKERRQ(ksp,ierr);
      PetscParMatrix A = PetscParMatrix(pA);
      X = new PetscParVector(A,false);
   }
   B -> SetData(b.GetData());
   X -> SetData(x.GetData());

   Mult(*B, *X);

   B -> ResetData();
   X -> ResetData();
}

PetscSolver::~PetscSolver()
{
   if (A) { delete A; }
   if (B) { delete B; }
   if (X) { delete X; }
   if (ksp)
   {
      MPI_Comm comm = PetscObjectComm((PetscObject)ksp);
      ierr = KSPDestroy(&ksp);CCHKERRQ(comm,ierr);
   }
}

void PetscSolver::SetTol(double tol)
{
   if (!ksp)
   {
      MFEM_ABORT("PetscSolver::SetTol (...) : KSP is missing");
      return;
   }
   // PETSC_DEFAULT does not change any other
   // customization previously set.
   ierr = KSPSetTolerances(ksp,tol,PETSC_DEFAULT,PETSC_DEFAULT,PETSC_DEFAULT);PCHKERRQ(ksp,ierr)
}

void PetscSolver::SetMaxIter(int max_iter)
{
   if (!ksp)
   {
      MFEM_ABORT("PetscSolver::SetTol (...) : KSP is missing");
      return;
   }
   // PETSC_DEFAULT does not change any other
   // customization previously set.
   ierr = KSPSetTolerances(ksp,PETSC_DEFAULT,PETSC_DEFAULT,PETSC_DEFAULT,max_iter);PCHKERRQ(ksp,ierr)
}

void PetscSolver::SetPrintLevel(int plev)
{
   if (!ksp)
   {
      MFEM_ABORT("PetscSolver::SetTol (...) : KSP is missing");
      return;
   }
  // TODO
}
}

// This function needs to access PETSc private data to make it faster.
// Eventually can be moved to PETSc source code.
#include "petsc/private/matimpl.h"
#include "_hypre_parcsr_mv.h"
#undef __FUNCT__
#define __FUNCT__ "MatConvert_hypreParCSR_AIJ"
static PetscErrorCode MatConvert_hypreParCSR_AIJ(hypre_ParCSRMatrix* hA,Mat* pA,PetscInt** odii,PetscInt** odjj,PetscScalar** oda,PetscInt** ooii,PetscInt** oojj,PetscScalar** ooa)
{
   MPI_Comm        comm = hypre_ParCSRMatrixComm(hA);
   hypre_CSRMatrix *hdiag,*hoffd;
   PetscScalar     *da,*oa,*aptr;
   PetscInt        *dii,*djj,*oii,*ojj,*iptr;
   PetscInt        i,dnnz,onnz,m,n;
   PetscMPIInt     size;
   PetscErrorCode  ierr;

   PetscFunctionBeginUser;
   hdiag = hypre_ParCSRMatrixDiag(hA);
   hoffd = hypre_ParCSRMatrixOffd(hA);
   m     = hypre_CSRMatrixNumRows(hdiag);
   n     = hypre_CSRMatrixNumCols(hdiag);
   dnnz  = hypre_CSRMatrixNumNonzeros(hdiag);
   onnz  = hypre_CSRMatrixNumNonzeros(hoffd);
   ierr  = PetscMalloc3(m+1,&dii,dnnz,&djj,dnnz,&da);CHKERRQ(ierr);
   ierr  = PetscMemcpy(dii,hypre_CSRMatrixI(hdiag),(m+1)*sizeof(PetscInt));CHKERRQ(ierr);
   ierr  = PetscMemcpy(djj,hypre_CSRMatrixJ(hdiag),dnnz*sizeof(PetscInt));CHKERRQ(ierr);
   ierr  = PetscMemcpy(da,hypre_CSRMatrixData(hdiag),dnnz*sizeof(PetscScalar));CHKERRQ(ierr);
   /* TODO should we add a case when the J pointer is already sorted? */
   iptr = djj;
   aptr = da;
   for (i=0;i<m;i++)
   {
      PetscInt nc = dii[i+1]-dii[i];
      ierr = PetscSortIntWithScalarArray(nc,iptr,aptr);CHKERRQ(ierr);
      iptr += nc;
      aptr += nc;
   }
   ierr = MPI_Comm_size(comm,&size);CHKERRQ(ierr);
   if (size > 1)
   {
     PetscInt *offdj,*coffd;

     ierr  = PetscMalloc3(m+1,&oii,onnz,&ojj,onnz,&oa);CHKERRQ(ierr);
     ierr  = PetscMemcpy(oii,hypre_CSRMatrixI(hoffd),(m+1)*sizeof(PetscInt));CHKERRQ(ierr);
     offdj = hypre_CSRMatrixJ(hoffd);
     coffd = hypre_ParCSRMatrixColMapOffd(hA);
     for (i=0;i<onnz;i++) ojj[i] = coffd[offdj[i]];
     ierr  = PetscMemcpy(oa,hypre_CSRMatrixData(hoffd),onnz*sizeof(PetscScalar));CHKERRQ(ierr);
     /* TODO should we add a case when the J pointer is already sorted? */
     iptr = ojj;
     aptr = oa;
     for (i=0;i<m;i++)
     {
        PetscInt nc = oii[i+1]-oii[i];
        ierr = PetscSortIntWithScalarArray(nc,iptr,aptr);CHKERRQ(ierr);
        iptr += nc;
        aptr += nc;
     }
     ierr = MatCreateMPIAIJWithSplitArrays(comm,m,n,PETSC_DECIDE,PETSC_DECIDE,dii,djj,da,oii,ojj,oa,pA);CCHKERRQ(comm,ierr);
   }
   else
   {
     oii = ojj = NULL;
     oa = NULL;
     ierr = MatCreateSeqAIJWithArrays(comm,m,n,dii,djj,da,pA);CCHKERRQ(comm,ierr);
   }
   /* Get back pointers to caller because we need to free those arrays when the matrix is no longer needed */
   *odii = dii;
   *odjj = djj;
   *oda  = da;
   *ooii = oii;
   *oojj = ojj;
   *ooa  = oa;
   PetscFunctionReturn(0);
}

#endif
#endif
