// Copyright (c) 2010, Lawrence Livermore National Security, LLC. Produced at
// the Lawrence Livermore National Laboratory. LLNL-CODE-443211. All Rights
// reserved. See file COPYRIGHT for details.
//
// This file is part of the MFEM library. For more information and source code
// availability see http://mfem.org.
//
// MFEM is free software; you can redistribute it and/or modify it under the
// terms of the GNU Lesser General Public License (as published by the Free
// Software Foundation) version 2.1 dated February 1999.

#include "../config/config.hpp"

#ifdef MFEM_USE_MPI

#include "fem.hpp"
#include <iostream>
#include <limits>
using namespace std;

namespace mfem
{

ParGridFunction::ParGridFunction(ParFiniteElementSpace *pf, GridFunction *gf)
{
   fes = pfes = pf;
   SetDataAndSize(gf->GetData(), gf->Size());
}

ParGridFunction::ParGridFunction(ParFiniteElementSpace *pf, HypreParVector *tv)
   : GridFunction(pf), pfes(pf)
{
   Distribute(tv);
}

ParGridFunction::ParGridFunction(ParMesh *pmesh, GridFunction *gf,
                                 int * partitioning)
{
   // duplicate the FiniteElementCollection from 'gf'
   fec = FiniteElementCollection::New(gf->FESpace()->FEColl()->Name());
   fes = pfes = new ParFiniteElementSpace(pmesh, fec, gf->FESpace()->GetVDim(),
                                          gf->FESpace()->GetOrdering());
   SetSize(pfes->GetVSize());

   if (partitioning)
   {
      Array<int> gvdofs, lvdofs;
      Vector lnodes;
      int element_counter = 0;
      Mesh & mesh(*gf->FESpace()->GetMesh());
      int MyRank;
      MPI_Comm_rank(pfes->GetComm(), &MyRank);
      for (int i = 0; i < mesh.GetNE(); i++)
         if (partitioning[i] == MyRank)
         {
            pfes->GetElementVDofs(element_counter, lvdofs);
            gf->FESpace()->GetElementVDofs(i, gvdofs);
            gf->GetSubVector(gvdofs, lnodes);
            SetSubVector(lvdofs, lnodes);
            element_counter++;
         }
   }
}

void ParGridFunction::Update(ParFiniteElementSpace *f)
{
   face_nbr_data.Destroy();
   GridFunction::Update(f);
   pfes = f;
}

void ParGridFunction::Update(ParFiniteElementSpace *f, Vector &v, int v_offset)
{
   face_nbr_data.Destroy();
   GridFunction::Update(f, v, v_offset);
   pfes = f;
}

void ParGridFunction::Distribute(const Vector *tv)
{
   pfes->Dof_TrueDof_Matrix()->Mult(*tv, *this);
}

void ParGridFunction::AddDistribute(double a, const Vector *tv)
{
   pfes->Dof_TrueDof_Matrix()->Mult(a, *tv, 1.0, *this);
}

void ParGridFunction::GetTrueDofs(Vector &tv) const
{
   pfes->GetRestrictionMatrix()->Mult(*this, tv);
}

HypreParVector *ParGridFunction::GetTrueDofs() const
{
   HypreParVector *tv = pfes->NewTrueDofVector();
   GetTrueDofs(*tv);
   return tv;
}

void ParGridFunction::ParallelAverage(Vector &tv) const
{
   pfes->Dof_TrueDof_Matrix()->MultTranspose(*this, tv);
   pfes->DivideByGroupSize(tv);
}

void ParGridFunction::ParallelAverage(HypreParVector &tv) const
{
   pfes->Dof_TrueDof_Matrix()->MultTranspose(*this, tv);
   pfes->DivideByGroupSize(tv);
}

HypreParVector *ParGridFunction::ParallelAverage() const
{
   HypreParVector *tv = pfes->NewTrueDofVector();
   ParallelAverage(*tv);
   return tv;
}

void ParGridFunction::ParallelProject(Vector &tv) const
{
   pfes->GetRestrictionMatrix()->Mult(*this, tv);
}

void ParGridFunction::ParallelProject(HypreParVector &tv) const
{
   pfes->GetRestrictionMatrix()->Mult(*this, tv);
}

HypreParVector *ParGridFunction::ParallelProject() const
{
   HypreParVector *tv = pfes->NewTrueDofVector();
   ParallelProject(*tv);
   return tv;
}

void ParGridFunction::ParallelAssemble(Vector &tv) const
{
   pfes->Dof_TrueDof_Matrix()->MultTranspose(*this, tv);
}

void ParGridFunction::ParallelAssemble(HypreParVector &tv) const
{
   pfes->Dof_TrueDof_Matrix()->MultTranspose(*this, tv);
}

HypreParVector *ParGridFunction::ParallelAssemble() const
{
   HypreParVector *tv = pfes->NewTrueDofVector();
   ParallelAssemble(*tv);
   return tv;
}

void ParGridFunction::ExchangeFaceNbrData()
{
   pfes->ExchangeFaceNbrData();

   if (pfes->GetFaceNbrVSize() <= 0)
   {
      return;
   }

   ParMesh *pmesh = pfes->GetParMesh();

   face_nbr_data.SetSize(pfes->GetFaceNbrVSize());
   Vector send_data(pfes->send_face_nbr_ldof.Size_of_connections());

   int *send_offset = pfes->send_face_nbr_ldof.GetI();
   int *send_ldof = pfes->send_face_nbr_ldof.GetJ();
   int *recv_offset = pfes->face_nbr_ldof.GetI();
   MPI_Comm MyComm = pfes->GetComm();

   int num_face_nbrs = pmesh->GetNFaceNeighbors();
   MPI_Request *requests = new MPI_Request[2*num_face_nbrs];
   MPI_Request *send_requests = requests;
   MPI_Request *recv_requests = requests + num_face_nbrs;
   MPI_Status  *statuses = new MPI_Status[num_face_nbrs];

   for (int i = 0; i < send_data.Size(); i++)
   {
      send_data[i] = data[send_ldof[i]];
   }

   for (int fn = 0; fn < num_face_nbrs; fn++)
   {
      int nbr_rank = pmesh->GetFaceNbrRank(fn);
      int tag = 0;

      MPI_Isend(&send_data(send_offset[fn]),
                send_offset[fn+1] - send_offset[fn],
                MPI_DOUBLE, nbr_rank, tag, MyComm, &send_requests[fn]);

      MPI_Irecv(&face_nbr_data(recv_offset[fn]),
                recv_offset[fn+1] - recv_offset[fn],
                MPI_DOUBLE, nbr_rank, tag, MyComm, &recv_requests[fn]);
   }

   MPI_Waitall(num_face_nbrs, send_requests, statuses);
   MPI_Waitall(num_face_nbrs, recv_requests, statuses);

   delete [] statuses;
   delete [] requests;
}

double ParGridFunction::GetValue(int i, const IntegrationPoint &ip, int vdim)
const
{
   Array<int> dofs;
   Vector DofVal, LocVec;
   int nbr_el_no = i - pfes->GetParMesh()->GetNE();
   if (nbr_el_no >= 0)
   {
      int fes_vdim = pfes->GetVDim();
      pfes->GetFaceNbrElementVDofs(nbr_el_no, dofs);
      if (fes_vdim > 1)
      {
         int s = dofs.Size()/fes_vdim;
         Array<int> _dofs(&dofs[(vdim-1)*s], s);
         face_nbr_data.GetSubVector(_dofs, LocVec);
         DofVal.SetSize(s);
      }
      else
      {
         face_nbr_data.GetSubVector(dofs, LocVec);
         DofVal.SetSize(dofs.Size());
      }
      pfes->GetFaceNbrFE(nbr_el_no)->CalcShape(ip, DofVal);
   }
   else
   {
      fes->GetElementDofs(i, dofs);
      fes->DofsToVDofs(vdim-1, dofs);
      DofVal.SetSize(dofs.Size());
      fes->GetFE(i)->CalcShape(ip, DofVal);
      GetSubVector(dofs, LocVec);
   }

   return (DofVal * LocVec);
}

void ParGridFunction::ProjectCoefficient(Coefficient &coeff)
{
   DeltaCoefficient *delta_c = dynamic_cast<DeltaCoefficient *>(&coeff);

   if (delta_c == NULL)
   {
      GridFunction::ProjectCoefficient(coeff);
   }
   else
   {
      double loc_integral, glob_integral;

      ProjectDeltaCoefficient(*delta_c, loc_integral);

      MPI_Allreduce(&loc_integral, &glob_integral, 1, MPI_DOUBLE, MPI_SUM,
                    pfes->GetComm());

      (*this) *= (delta_c->Scale() / glob_integral);
   }
}

void ParGridFunction::ProjectDiscCoefficient(VectorCoefficient &coeff)
{
   // local maximal element attribute for each dof
   Array<int> ldof_attr;

   // local projection
   GridFunction::ProjectDiscCoefficient(coeff, ldof_attr);

   // global maximal element attribute for each dof
   Array<int> gdof_attr;
   ldof_attr.Copy(gdof_attr);
   GroupCommunicator &gcomm = pfes->GroupComm();
   gcomm.Reduce<int>(gdof_attr, GroupCommunicator::Max);
   gcomm.Bcast(gdof_attr);

   // set local value to zero if global maximal element attribute is larger than
   // the local one, and mark (in gdof_attr) if we have the correct value
   for (int i = 0; i < pfes->GetVSize(); i++)
   {
      if (gdof_attr[i] > ldof_attr[i])
      {
         (*this)(i) = 0.0;
         gdof_attr[i] = 0;
      }
      else
      {
         gdof_attr[i] = 1;
      }
   }

   // parallel averaging plus interpolation to determine final values
   HypreParVector *tv = pfes->NewTrueDofVector();
   gcomm.Reduce<int>(gdof_attr, GroupCommunicator::Sum);
   gcomm.Bcast(gdof_attr);
   for (int i = 0; i < fes->GetVSize(); i++)
   {
      (*this)(i) /= gdof_attr[i];
   }
   this->ParallelAssemble(*tv);
   this->Distribute(tv);
   delete tv;
}


void ParGridFunction::Save(std::ostream &out) const
{
   for (int i = 0; i < size; i++)
   {
      if (pfes->GetDofSign(i) < 0) { data[i] = -data[i]; }
   }

   GridFunction::Save(out);

   for (int i = 0; i < size; i++)
   {
      if (pfes->GetDofSign(i) < 0) { data[i] = -data[i]; }
   }
}

void ParGridFunction::SaveAsOne(std::ostream &out)
{
   int i, p;

   MPI_Comm MyComm;
   MPI_Status status;
   int MyRank, NRanks;

   MyComm = pfes -> GetComm();

   MPI_Comm_size(MyComm, &NRanks);
   MPI_Comm_rank(MyComm, &MyRank);

   double **values = new double*[NRanks];
   int *nv = new int[NRanks];
   int *nvdofs = new int[NRanks];
   int *nedofs = new int[NRanks];
   int *nfdofs = new int[NRanks];
   int *nrdofs = new int[NRanks];

   values[0] = data;
   nv[0]     = pfes -> GetVSize();
   nvdofs[0] = pfes -> GetNVDofs();
   nedofs[0] = pfes -> GetNEDofs();
   nfdofs[0] = pfes -> GetNFDofs();

   if (MyRank == 0)
   {
      pfes -> Save(out);
      out << '\n';

      for (p = 1; p < NRanks; p++)
      {
         MPI_Recv(&nv[p], 1, MPI_INT, p, 455, MyComm, &status);
         MPI_Recv(&nvdofs[p], 1, MPI_INT, p, 456, MyComm, &status);
         MPI_Recv(&nedofs[p], 1, MPI_INT, p, 457, MyComm, &status);
         MPI_Recv(&nfdofs[p], 1, MPI_INT, p, 458, MyComm, &status);
         values[p] = new double[nv[p]];
         MPI_Recv(values[p], nv[p], MPI_DOUBLE, p, 460, MyComm, &status);
      }

      int vdim = pfes -> GetVDim();

      for (p = 0; p < NRanks; p++)
      {
         nrdofs[p] = nv[p]/vdim - nvdofs[p] - nedofs[p] - nfdofs[p];
      }

      if (pfes->GetOrdering() == Ordering::byNODES)
      {
         for (int d = 0; d < vdim; d++)
         {
            for (p = 0; p < NRanks; p++)
               for (i = 0; i < nvdofs[p]; i++)
               {
                  out << *values[p]++ << '\n';
               }

            for (p = 0; p < NRanks; p++)
               for (i = 0; i < nedofs[p]; i++)
               {
                  out << *values[p]++ << '\n';
               }

            for (p = 0; p < NRanks; p++)
               for (i = 0; i < nfdofs[p]; i++)
               {
                  out << *values[p]++ << '\n';
               }

            for (p = 0; p < NRanks; p++)
               for (i = 0; i < nrdofs[p]; i++)
               {
                  out << *values[p]++ << '\n';
               }
         }
      }
      else
      {
         for (p = 0; p < NRanks; p++)
            for (i = 0; i < nvdofs[p]; i++)
               for (int d = 0; d < vdim; d++)
               {
                  out << *values[p]++ << '\n';
               }

         for (p = 0; p < NRanks; p++)
            for (i = 0; i < nedofs[p]; i++)
               for (int d = 0; d < vdim; d++)
               {
                  out << *values[p]++ << '\n';
               }

         for (p = 0; p < NRanks; p++)
            for (i = 0; i < nfdofs[p]; i++)
               for (int d = 0; d < vdim; d++)
               {
                  out << *values[p]++ << '\n';
               }

         for (p = 0; p < NRanks; p++)
            for (i = 0; i < nrdofs[p]; i++)
               for (int d = 0; d < vdim; d++)
               {
                  out << *values[p]++ << '\n';
               }
      }

      for (p = 1; p < NRanks; p++)
      {
         values[p] -= nv[p];
         delete [] values[p];
      }
   }
   else
   {
      MPI_Send(&nv[0], 1, MPI_INT, 0, 455, MyComm);
      MPI_Send(&nvdofs[0], 1, MPI_INT, 0, 456, MyComm);
      MPI_Send(&nedofs[0], 1, MPI_INT, 0, 457, MyComm);
      MPI_Send(&nfdofs[0], 1, MPI_INT, 0, 458, MyComm);
      MPI_Send(data, nv[0], MPI_DOUBLE, 0, 460, MyComm);
   }

   delete [] values;
   delete [] nv;
   delete [] nvdofs;
   delete [] nedofs;
   delete [] nfdofs;
   delete [] nrdofs;
}

double GlobalLpNorm(const double p, double loc_norm, MPI_Comm comm)
{
   double glob_norm;

   if (p < numeric_limits<double>::infinity())
   {
      // negative quadrature weights may cause the error to be negative
      if (loc_norm < 0.0)
      {
         loc_norm = -pow(-loc_norm, p);
      }
      else
      {
         loc_norm = pow(loc_norm, p);
      }

      MPI_Allreduce(&loc_norm, &glob_norm, 1, MPI_DOUBLE, MPI_SUM, comm);

      if (glob_norm < 0.0)
      {
         glob_norm = -pow(-glob_norm, 1.0/p);
      }
      else
      {
         glob_norm = pow(glob_norm, 1.0/p);
      }
   }
   else
   {
      MPI_Allreduce(&loc_norm, &glob_norm, 1, MPI_DOUBLE, MPI_MAX, comm);
   }

   return glob_norm;
}


void ParGridFunction::ComputeFlux(
   BilinearFormIntegrator &blfi,
   GridFunction &flux_, int wcoef, int subdomain)
{
   // In this context we know that flux should be a ParGridFunction
   ParGridFunction& flux = dynamic_cast<ParGridFunction&>(flux_);

   ParFiniteElementSpace *ffes = flux.ParFESpace();

   Array<int> count(flux.Size());
   SumFluxAndCount(blfi, flux, count, 0, subdomain);

   if (ffes->Conforming()) // FIXME: nonconforming
   {
      // Accumulate flux and counts in parallel

      ffes->GroupComm().Reduce<double>(flux, GroupCommunicator::Sum);
      ffes->GroupComm().Bcast<double>(flux);

      ffes->GroupComm().Reduce<int>(count, GroupCommunicator::Sum);
      ffes->GroupComm().Bcast<int>(count);
   }
   else
   {
      MFEM_WARNING("Averaging on processor boundaries not implemented for "
                   "NC meshes yet.");
   }

   // complete averaging
   for (int i = 0; i < count.Size(); i++)
   {
      if (count[i] != 0) { flux(i) /= count[i]; }
   }

   if (ffes->Nonconforming())
   {
      // On a partially conforming flux space, project on the conforming space.
      // Using this code may lead to worse refinements in ex6, so we do not use
      // it by default.

      // Vector conf_flux;
      // flux.ConformingProject(conf_flux);
      // flux.ConformingProlongate(conf_flux);
   }
}

// Supports L2ZZErrorEstimator function below, cf. GridFunction::ComputeLpError.
double computeElementLpNorm(double p, int i,
                            const FiniteElementSpace* fes,
                            Vector& el_f)
{
   double norm = 0.0;

   const FiniteElement* fe = fes->GetFE(i);
   int ndof = fe->GetDof();
   const IntegrationRule *ir;
   int intorder = 2*fe->GetOrder() + 1; // <----------
   ir = &(IntRules.Get(fe->GetGeomType(), intorder));

   int vdim = fes->GetVDim();
   int nip = ir->GetNPoints();

   DenseMatrix vals;
   vals.SetSize(vdim,nip);

   Array<int> vdofs;
   fes->GetElementVDofs(i, vdofs);

   const FiniteElement *FElem = fes->GetFE(i);
   Vector DofVal(FElem->GetDof());

   // Compute vector values at each integration point
   for (int j = 0; j < nip; j++)
   {
      FElem->CalcShape(ir->IntPoint(j), DofVal);
      for (int k = 0; k < vdim; k++)
      {
         vals(k,j) = DofVal * ((const double *)el_f +ndof*k);
      }
   }

   // Compute magnitudes at each integration point
   Vector loc_norm;
   loc_norm.SetSize(vals.Width());
   vals.Norm2(loc_norm);

   // Integrate
   ElementTransformation *T = fes->GetElementTransformation(i);
   for (int j = 0; j < ir->GetNPoints(); j++)
   {
      const IntegrationPoint &ip = ir->IntPoint(j);
      T->SetIntPoint(&ip);
      double err = fabs(loc_norm(j));
      if (p < numeric_limits<double>::infinity())
      {
         err = pow(err, p);
         norm += ip.weight * T->Weight() * err;
      }
      else
      {
         norm = std::max(norm, err);
      }
   }

   if (p < numeric_limits<double>::infinity())
   {
      // Negative quadrature weights may cause the norm to be negative
      if (norm < 0.)
      {
         norm = -pow(-norm, 1./p);
      }
      else
      {
         norm = pow(norm, 1./p);
      }
   }

   return norm;
}

void L2ZZErrorEstimator(BilinearFormIntegrator &flux_integrator,
                        ParGridFunction &x,
                        ParFiniteElementSpace &flux_fespace,
                        ParFiniteElementSpace &flux_dcfespace,
                        Vector &errors)
{
   double solver_tol = 1e-12;
   int solver_max_it = 200;
   int norm_p        = 1;

   // Compute fluxes in discontinuous space
   GridFunction discflux(&flux_dcfespace);

   ElementTransformation *Transf;

   FiniteElementSpace *xfes = x.FESpace();
   FiniteElementSpace *ffes = discflux.FESpace();

   int nfe = xfes->GetNE();
   Array<int> xdofs;
   Array<int> fdofs;
   Vector el_x, el_f;

   discflux = 0.0;

   for (int i = 0; i < nfe; i++)
   {
      xfes->GetElementVDofs(i, xdofs);
      ffes->GetElementVDofs(i, fdofs);

      x.GetSubVector(xdofs, el_x);

      Transf = xfes->GetElementTransformation(i);
      bool with_coeff = false;
      flux_integrator.ComputeElementFlux(*xfes->GetFE(i), *Transf, el_x,
                                         *ffes->GetFE(i), el_f, with_coeff);

      discflux.AddElementVector(fdofs, el_f);
   }

   ParLinearForm *b = new ParLinearForm(&flux_fespace);
   VectorGridFunctionCoefficient f(&discflux);
   b->AddDomainIntegrator(new VectorDomainLFIntegrator(f));
   b->Assemble();

   ParBilinearForm* a = new ParBilinearForm(&flux_fespace);
   a->AddDomainIntegrator(new VectorMassIntegrator);
   a->Assemble();
   a->Finalize();

   // The destination of the projected discontinuous flux
   ParGridFunction fbar(&flux_fespace);
   fbar = 0.0;

   HypreParMatrix* A = a->ParallelAssemble();
   HypreParVector* B = b->ParallelAssemble();
   HypreParVector* X = fbar.ParallelProject();

   delete a;
   delete b;

   // Define and apply a parallel PCG solver for AX=B with the BoomerAMG
   // preconditioner from hypre.
   HypreSolver *amg = new HypreBoomerAMG(*A);
   HyprePCG *pcg = new HyprePCG(*A);
   pcg->SetTol(solver_tol);
   pcg->SetMaxIter(solver_max_it);
   pcg->SetPrintLevel(2);
   pcg->SetPreconditioner(*amg);
   pcg->Mult(*B, *X);

   // Extract the parallel grid function corresponding to the finite element
   // approximation X. This is the local solution on each processor.
   fbar = *X;

   // Proceed through the elements one by one, and find the difference between
   // the flux as computed per element and the flux projected onto a continuous
   // H1 space.

   Vector el_fbar;
   ParFiniteElementSpace& fespace = *x.ParFESpace();

   for (int i = 0; i < fespace.GetNE(); i++)
   {

      fespace.GetElementVDofs(i, xdofs);
      flux_fespace.GetElementVDofs(i, fdofs);

      x.GetSubVector(xdofs, el_x);
      fbar.GetSubVector(fdofs, el_fbar);

      const FiniteElement* xelem = fespace.GetFE(i);
      const FiniteElement* flux_elem = flux_fespace.GetFE(i);

      Transf = fespace.GetElementTransformation(i);
      flux_integrator.ComputeElementFlux(*xelem,
                                         *Transf, el_x,
                                         *flux_elem,
                                         el_f, 0);
      el_f -= el_fbar;
      errors(i) = computeElementLpNorm(norm_p, i, &flux_fespace, el_f);
   }

   delete amg;
   delete pcg;
}

}

#endif
