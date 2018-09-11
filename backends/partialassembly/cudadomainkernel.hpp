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


#ifndef MFEM_BACKENDS_PA_CUDADOMAINKERNEL_HPP
#define MFEM_BACKENDS_PA_CUDADOMAINKERNEL_HPP

#if defined(MFEM_USE_BACKENDS) && defined(MFEM_USE_PA)

#include "util.hpp"
#include "padomainkernel.hpp"
#ifdef __NVCC__
#include "cuda-kernels.hpp"
#endif

namespace mfem
{

namespace pa
{

template <PAOp OpName>
class CudaDomainKernel;

#ifdef __NVCC__

template <typename QFunc, typename Tensor>
__global__ void BtDBevalDkernel(QFunc& qfunc, Tensor& D) {
	int e = blockIdx.x;//Assumes one block per element
	// auto D_e = D.slice(D, e);
	auto D_e = D + e;
	qfunc.evalD(e, D_e);
}

template <>
class CudaDomainKernel<BtDB> {
private:
	typedef VectorType<CudaDevice, double> Vector;
	// template <typename Equation>
	// using QFunc = QuadTensorFunc<Equation, CudaDevice>;
	// const FESpace<CudaDevice>& trial_fes, test_fes;
	// Tensor<2> shape1d;//I don't like that this is not const
	const int dim;
	CudaKernel kernel;
	// const int nbElts;
public:
	template <typename Equation>
	CudaDomainKernel(const Equation* eq)
		: dim(eq->getDim())
		, kernel("/home/dudouit1/Projects/mfem-engine/mfem/backends/partialassembly/kernels/", "mass.cu", "evector_kernel",
				{ {"NI", std::to_string(eq->getNbDofs1d())}, {"NK", std::to_string(eq->getNbQuads1d())} },
				std::vector<const char*>())
	{

	}

	template <typename Tensor>
	void Mult(const Tensor& D, const Vector& x, Vector& y) const {
		// if(dim == 1) Mult1d(D,x,y);
		// else if(dim == 2 ) Mult2d(D,x,y);
		// else if(dim == 3 ) Mult3d(D,x,y);
	}

	template <typename Tensor>
	void MultAdd(const Tensor& D, const Vector& x, Vector& y) const {
		// if(dim == 1) MultAdd1d(D,x,y);
		// else if(dim == 2 ) MultAdd2d(D,x,y);
		// else if(dim == 3 ) MultAdd3d(D,x,y);
	}

	template <typename QFunc, typename Tensor>// c'est bourrin ca
	void evalD(QFunc& qfunc, Tensor& D) const {
		//TODO call the assembly
		//call QFunc.evalD inside __global__ where blocksize=nb_quads and nb_block=mn_elem?
		const int nb_elts = 1;
		const int nb_quads = 1;
		BtDBevalDkernel<<< nb_elts, nb_quads>>>(qfunc, D);
	}

};


template <typename QFunc, typename Tensor>
__global__ void GtDGevalDkernel(QFunc& qfunc, Tensor& D) {
	int e = blockIdx.x;//Assumes one block per element
	// auto D_e = D.slice(D, e);
	auto D_e = D + e;
	qfunc.evalD(e, D_e);
}


template <>
class CudaDomainKernel<GtDG> {
private:
	typedef VectorType<CudaDevice, double> Vector;
	// template <typename Equation>
	// using QFunc = QuadTensorFunc<Equation, CudaDevice>;
	// const FESpace<CudaDevice>& trial_fes, test_fes;
	// Tensor<2> shape1d;//I don't like that this is not const
	const int dim;
	CudaKernel kernel;
	// const int nbElts;
public:
	template <typename Equation>
	CudaDomainKernel(const Equation* eq)
		: dim(eq->getDim())
		, kernel("/home/dudouit1/Projects/mfem-engine/mfem/backends/partialassembly/kernels/", "mass.cu", "evector_kernel",
				{ {"NI", std::to_string(eq->getNbDofs1d())}, {"NK", std::to_string(eq->getNbQuads1d())} },
				std::vector<const char*>())
	{
		std::cout << "building cuda diffusion" << std::endl;
	}

	template <typename Tensor>
	void Mult(const Tensor& D, const Vector& x, Vector& y) const {
		// kernel.launch();
		// if(dim == 1) Mult1d(D,x,y);
		// else if(dim == 2 ) Mult2d(D,x,y);
		// else if(dim == 3 ) Mult3d(D,x,y);
	}

	template <typename Tensor>
	void MultAdd(const Tensor& D, const Vector& x, Vector& y) const {
		// if(dim == 1) MultAdd1d(D,x,y);
		// else if(dim == 2 ) MultAdd2d(D,x,y);
		// else if(dim == 3 ) MultAdd3d(D,x,y);
	}

	template <typename QFunc, typename Tensor>// c'est bourrin ca
	void evalD(QFunc& qfunc, Tensor& D) const {
		//TODO call the assembly
		const int nb_elts = 1;
		const int nb_quads = 1;
		GtDGevalDkernel<<< nb_elts, nb_quads>>>(qfunc, D);
	}

};

#endif

}

}

#endif

#endif