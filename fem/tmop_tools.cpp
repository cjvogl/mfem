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

#include "tmop_tools.hpp"

namespace mfem
{

using namespace mfem;

AdvectorCGOperator::AdvectorCGOperator(const Vector &x_start,
                                       ParGridFunction &vel,
                                       Vector &xn,
                                       ParFiniteElementSpace &pfes)
   : TimeDependentOperator(pfes.GetVSize()),
     x0(x_start), u(vel), x_now(xn), u_coeff(&u), M(&pfes), K(&pfes)
{
   ConvectionIntegrator *Kinteg = new ConvectionIntegrator(u_coeff);
   K.AddDomainIntegrator(Kinteg);
   K.Assemble(0);
   K.Finalize(0);

   MassIntegrator *Minteg = new MassIntegrator;
   M.AddDomainIntegrator(Minteg);
   M.Assemble();
   M.Finalize();
}

void AdvectorCGOperator::Mult(const Vector &ind, Vector &di_dt) const
{
   const double t = GetTime();

   // Move the mesh.
   add(x0, t, u, x_now);

   // Assemble on the new mesh.
   K.BilinearForm::operator=(0.0);
   K.Assemble();
   ParGridFunction rhs(K.ParFESpace());
   K.Mult(ind, rhs);
   M.BilinearForm::operator=(0.0);
   M.Assemble();

   HypreParVector *RHS = rhs.ParallelAssemble();
   HypreParVector *X   = rhs.ParallelAverage();
   HypreParMatrix *Mh  = M.ParallelAssemble();
/*
   CGSolver cg(M.ParFESpace()->GetParMesh()->GetComm());
   HypreSmoother prec;
   prec.SetType(HypreSmoother::Jacobi, 1);
   cg.SetPreconditioner(prec);
   cg.SetOperator(*Mh);
   cg.SetRelTol(1e-12); cg.SetAbsTol(0.0);
   cg.SetMaxIter(100);
   cg.SetPrintLevel(0);
   cg.Mult(*RHS, *X);
   K.ParFESpace()->Dof_TrueDof_Matrix()->Mult(*X, di_dt);
*/
   GMRESSolver gmres(M.ParFESpace()->GetParMesh()->GetComm());
   HypreSmoother prec;
   prec.SetType(HypreSmoother::Jacobi, 1);
   gmres.SetPreconditioner(prec);
   gmres.SetOperator(*Mh);
   gmres.SetRelTol(1e-12); gmres.SetAbsTol(0.0);
   gmres.SetMaxIter(100);
   gmres.SetPrintLevel(0);
   gmres.Mult(*RHS, *X);
   K.ParFESpace()->Dof_TrueDof_Matrix()->Mult(*X, di_dt);

   delete Mh;
   delete X;
   delete RHS;
}

void AdvectorCG::ComputeAtNewPosition(const Vector &x_start,
                                      const Vector &x_end,
                                      Vector &ind)
{
   ParGridFunction mesh_nodes(pfes);
   mesh_nodes = x_start;
   pmesh->SetNodalGridFunction(&mesh_nodes);

   ParGridFunction u(pfes);
   subtract(x_end, x_start, u);

   // This must be the fes of the ind, associated with the object's mesh.
   AdvectorCGOperator oper(x_start, u, mesh_nodes, *pfes);
   ode_solver.Init(oper);

   // Compute some time step [mesh_size / speed].
   double min_h = std::numeric_limits<double>::infinity();
   for (int i = 0; i < pmesh->GetNE(); i++)
   {
      min_h = std::min(min_h, pmesh->GetElementSize(1));
   }
   double v_max = 0.0;
   int s = u.ParFESpace()->GetVSize() / 2;
   for (int i = 0; i < s; i++)
   {
      double vel = std::sqrt( u(i) * u(i) + u(i+s) * u(i+s) + 1e-14);
      v_max = std::max(v_max, vel);
   }
   double dt = 0.5 * min_h / v_max;
   double glob_dt;
   MPI_Allreduce(&dt, &glob_dt, 1, MPI_DOUBLE, MPI_MIN, pfes->GetComm());

   int myid;
   MPI_Comm_rank(pfes->GetComm(), &myid);
   double t = 0.0;
   bool last_step = false;
   for (int ti = 1; !last_step; ti++)
   {
      if (t + glob_dt >= 1.0)
      {
         if (myid == 0)
         {
            std::cout << "Remap with dt = " << glob_dt
                      << " took " << ti << " steps." << std::endl;
         }
         glob_dt = 1.0 - t;
         last_step = true;
      }
      ode_solver.Step(ind, t, glob_dt);
   }

   // Trim to put it in [0, 1].
   for (int i = 0; i < ind.Size(); i++)
   {
      if (ind(i) < 0.0) { ind(i) = 0.0; }
      if (ind(i) > 1.0) { ind(i) = 1.0; }
   }
}

}