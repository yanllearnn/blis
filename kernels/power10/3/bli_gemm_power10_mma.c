/*

   BLIS
   An object-based framework for developing high-performance BLAS-like
   libraries.

   Copyright (C) 2014, The University of Texas at Austin

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are
   met:
    - Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    - Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    - Neither the name(s) of the copyright holder(s) nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
   HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

*/

#include "blis.h"

typedef double dv4sf_t __attribute__ ((vector_size (16)));
typedef unsigned char vec_t __attribute__ ((vector_size (16)));

/*  disassemble the acc accumulator into a result array of vectors
	store the result accordingly  */
#define dgemm_SAVE_ACC_(ACC, rs_c, j)                   \
    __builtin_mma_disassemble_acc (result, ACC);      \
    rowC = (dv4sf_t *) &C0[j];                        \
    rowC[0] = alpha_ * result[0] + beta_ * rowC[0];   \
    rowC = (dv4sf_t *) &C0[rs_c+j];                     \
    rowC[0] = alpha_ * result[1] + beta_ * rowC[0];   \
    rowC = (dv4sf_t *) &C0[2*rs_c+j];                   \
    rowC[0] = alpha_ * result[2] + beta_ * rowC[0] ;  \
    rowC = (dv4sf_t *) &C0[3*rs_c+j];                   \
    rowC[0] = alpha_ * result[3] + beta_ * rowC[0] ;

#define dgemm_SAVE_ACC_bz(ACC, rs_c, j)                 \
    __builtin_mma_disassemble_acc (result, ACC);      \
    rowC = (dv4sf_t *) &C0[j];                        \
    rowC[0] = alpha_ * result[0];                     \
    rowC = (dv4sf_t *) &C0[rs_c+j];                     \
    rowC[0] = alpha_ * result[1];                     \
    rowC = (dv4sf_t *) &C0[2*rs_c+j];                   \
    rowC[0] = alpha_ * result[2];                     \
    rowC = (dv4sf_t *) &C0[3*rs_c+j];                   \
    rowC[0] = alpha_ * result[3];

#define PREFETCH1(x, y) __asm__ volatile ("dcbt %0, %1" : : "r" (x), "b" (y) : "memory");

#define LOAD_VECTORS \
		ca = (vec_t *) A0; \
		rb = (vec_t *) B0; 

#define D_ASSEMBLE_VEC_PAIR \
		__builtin_mma_assemble_pair (&colA_1, ca[1], ca[0]); \
		__builtin_mma_assemble_pair (&colA_2, ca[3], ca[2]); 

#define D_ACCUMULATE \
		__builtin_mma_xvf64gerpp (&acc0, colA_1, rb[0]); \
		__builtin_mma_xvf64gerpp (&acc1, colA_1, rb[1]); \
		__builtin_mma_xvf64gerpp (&acc2, colA_1, rb[2]); \
		__builtin_mma_xvf64gerpp (&acc3, colA_1, rb[3]); \
		__builtin_mma_xvf64gerpp (&acc4, colA_2, rb[0]); \
		__builtin_mma_xvf64gerpp (&acc5, colA_2, rb[1]); \
		__builtin_mma_xvf64gerpp (&acc6, colA_2, rb[2]); \
		__builtin_mma_xvf64gerpp (&acc7, colA_2, rb[3]); 

#define D_INCREMENT \
		A0+=8; \
		B0+=8;

#define D_AB_PRODUCT \
		LOAD_VECTORS \
		D_ASSEMBLE_VEC_PAIR \
		D_INCREMENT \
		D_ACCUMULATE 


void bli_dgemm_power10_mma_8x8
	(
		dim_t               k0,
		double*    restrict alpha,
		double*    restrict a,
		double*    restrict b,
		double*    restrict beta,
		double*    restrict c, inc_t rs_c0, inc_t cs_c0,
		auxinfo_t* restrict data,
		cntx_t*    restrict cntx
	)
{

	// Typecast local copies of integers in case dim_t and inc_t are a
	// different size than is expected by load instructions.
	// (1 is subtracted from k0 because 1 iteration of the k loop is pulled out)
	uint64_t k_iter = (k0-1) / 4;
	uint64_t k_left = (k0-1) % 4;

	uint64_t rs_c   = rs_c0;

	double* restrict A0 = a;
	double* restrict B0 = b;
	double* restrict C0 = c;

	double alpha_ = *alpha,
	       beta_ = *beta;

	dv4sf_t result[4];
  	dv4sf_t *rowC;

	/* 8 accumulator registers that will be used to store the result.
	   
	   Each accumulator register is mapped to 4 vector registers.
	   Illustration:
					  
			acc0 = [  vs0
					  vs1
			          vs3
					  vs4  ]

		These registers are used to store the result of an outer product 
		instruction (general outer product instruction syntax: xv???ger??). */
	__vector_quad acc0, acc1, acc2, acc3, 
	              acc4, acc5, acc6, acc7;

	/* 2 vector pairs are necessary for a double precision outer product 
	   instruction. */
	__vector_pair colA_1, 
	              colA_2;

	/* Prefetch C so that it stays in cache */
	PREFETCH1 (C0, 0);
	PREFETCH1 (C0 + rs_c, 0);
	PREFETCH1 (C0 + rs_c + rs_c, 0);
	PREFETCH1 (C0 + rs_c + rs_c + rs_c, 0);
	PREFETCH1 (C0, 128);
	PREFETCH1 (C0 + rs_c, 128);
	PREFETCH1 (C0 + rs_c + rs_c, 128);
	PREFETCH1 (C0 + rs_c + rs_c + rs_c, 128);

	/* Load elements into vector registers */
	vec_t *ca = (vec_t *) A0;
	vec_t *rb = (vec_t *) B0; 

	/* Each accumulator represents a matrix of size 
	   4 x ((datatype size in bytes) / 16)  (vector register size = 128b)

	   Thus in the case of double, the accumulate registers represent a 4x2 
	   matrix. However, a vector register can hold at most 2 doubles. Thus, if
	   we performed an outer product using 2 vector register, we can only get a 
	   2x2 matrix. Therefore, we must create a vector register pair in order
	   to get the desired 4x2 matrix.
	
	*/
	D_ASSEMBLE_VEC_PAIR

	/* Compute accumulate outer products and override accumulators with result */
	__builtin_mma_xvf64ger (&acc0, colA_1, rb[0]);
	__builtin_mma_xvf64ger (&acc1, colA_1, rb[1]);
	__builtin_mma_xvf64ger (&acc2, colA_1, rb[2]);
	__builtin_mma_xvf64ger (&acc3, colA_1, rb[3]);
	__builtin_mma_xvf64ger (&acc4, colA_2, rb[0]);
	__builtin_mma_xvf64ger (&acc5, colA_2, rb[1]);
	__builtin_mma_xvf64ger (&acc6, colA_2, rb[2]);
	__builtin_mma_xvf64ger (&acc7, colA_2, rb[3]);

	/* Move A and B pointers */
	D_INCREMENT

	// k loop (unrolled by 4)
	for (int k = 0; k<k_iter; k++)
	{
		D_AB_PRODUCT
		D_AB_PRODUCT
		D_AB_PRODUCT
		D_AB_PRODUCT
	}
	
	// edge loop
	for (int k = 0; k<k_left; k++)
	{
		D_AB_PRODUCT
	}

	// handle beta cases
	if (beta_ != 0.0)
	{
		dgemm_SAVE_ACC_(&acc0, rs_c, 0       );
		dgemm_SAVE_ACC_(&acc1, rs_c, 2       );
		dgemm_SAVE_ACC_(&acc2, rs_c, 4       );
		dgemm_SAVE_ACC_(&acc3, rs_c, 6       );
		dgemm_SAVE_ACC_(&acc4, rs_c,   4*rs_c);
		dgemm_SAVE_ACC_(&acc5, rs_c, 2+4*rs_c);
		dgemm_SAVE_ACC_(&acc6, rs_c, 4+4*rs_c);
		dgemm_SAVE_ACC_(&acc7, rs_c, 6+4*rs_c);
	}
	else
	{
		dgemm_SAVE_ACC_bz(&acc0, rs_c, 0       );
		dgemm_SAVE_ACC_bz(&acc1, rs_c, 2       );
		dgemm_SAVE_ACC_bz(&acc2, rs_c, 4       );
		dgemm_SAVE_ACC_bz(&acc3, rs_c, 6       );
		dgemm_SAVE_ACC_bz(&acc4, rs_c,   4*rs_c);
		dgemm_SAVE_ACC_bz(&acc5, rs_c, 2+4*rs_c);
		dgemm_SAVE_ACC_bz(&acc6, rs_c, 4+4*rs_c);
		dgemm_SAVE_ACC_bz(&acc7, rs_c, 6+4*rs_c);
	}

}


typedef float fv4sf_t __attribute__ ((vector_size (16)));

#define sgemm_SAVE_ACC_(ACC, rs_c, j)                \
    __builtin_mma_disassemble_acc (result, ACC);       \
    rowC = (fv4sf_t *) &C0[j];                        \
    rowC[0] = alpha_ * result[0] + beta_ * rowC[0];    \
    rowC = (fv4sf_t *) &C0[rs_c+j];                     \
    rowC[0] = alpha_ * result[1] + beta_ * rowC[0];    \
    rowC = (fv4sf_t *) &C0[2*rs_c+j];                   \
    rowC[0] = alpha_ * result[2] + beta_ * rowC[0] ;   \
    rowC = (fv4sf_t *) &C0[3*rs_c+j];                   \
    rowC[0] = alpha_ * result[3] + beta_ * rowC[0] ;

#define sgemm_SAVE_ACC_bz(ACC, rs_c, j)                     \
    __builtin_mma_disassemble_acc (result, ACC);     \
    rowC = (fv4sf_t *) &C0[j];                      \
    rowC[0] = alpha_ * result[0];                      \
    rowC = (fv4sf_t *) &C0[rs_c+j];                     \
    rowC[0] = alpha_ * result[1];                      \
    rowC = (fv4sf_t *) &C0[2*rs_c+j];                   \
    rowC[0] = alpha_ * result[2];                      \
    rowC = (fv4sf_t *) &C0[3*rs_c+j];                   \
    rowC[0] = alpha_ * result[3];

#define S_ACCUMULATE \
		__builtin_mma_xvf32gerpp (&acc0, ca[0], rb[0]); \
		__builtin_mma_xvf32gerpp (&acc1, ca[0], rb[1]); \
		__builtin_mma_xvf32gerpp (&acc2, ca[0], rb[2]); \
		__builtin_mma_xvf32gerpp (&acc3, ca[0], rb[3]); \
		__builtin_mma_xvf32gerpp (&acc4, ca[1], rb[0]); \
		__builtin_mma_xvf32gerpp (&acc5, ca[1], rb[1]); \
		__builtin_mma_xvf32gerpp (&acc6, ca[1], rb[2]); \
		__builtin_mma_xvf32gerpp (&acc7, ca[1], rb[3]); 

#define S_INCREMENT \
		A0+=8; \
		B0+=16;

#define S_AB_PRODUCT \
		LOAD_VECTORS \
		S_INCREMENT \
		S_ACCUMULATE 

void bli_sgemm_power10_mma_8x16
	(
		dim_t               k0,
		float*     restrict alpha,
		float*     restrict a,
		float*     restrict b,
		float*     restrict beta,
		float*     restrict c, inc_t rs_c0, inc_t cs_c0,
		auxinfo_t* restrict data,
		cntx_t*    restrict cntx
	)
{
	// Typecast local copies of integers in case dim_t and inc_t are a
	// different size than is expected by load instructions.
	// (1 is subtracted from k0 because 1 iteration of the k loop is pulled out)
	uint64_t k_iter = (k0-1) / 4;
	uint64_t k_left = (k0-1) % 4;
	
	uint64_t rs_c   = rs_c0;

	fv4sf_t result[4];
  	fv4sf_t *rowC;

	// accumulators that will hold the matrix product
	__vector_quad acc0, acc1, acc2, acc3, 
	              acc4, acc5, acc6, acc7;

	float* restrict A0 = a;
	float* restrict B0 = b;
	float* restrict C0 = c;

	float alpha_ = *alpha,
	      beta_  = *beta;

	/* Load elements into vector registers */
	vec_t *ca = (vec_t *) A0;
	vec_t *rb = (vec_t *) B0;

	/* Compute accumulate outer products and override accumulators with result */
	__builtin_mma_xvf32ger (&acc0, ca[0], rb[0]);
	__builtin_mma_xvf32ger (&acc1, ca[0], rb[1]);
	__builtin_mma_xvf32ger (&acc2, ca[0], rb[2]);
	__builtin_mma_xvf32ger (&acc3, ca[0], rb[3]);
	__builtin_mma_xvf32ger (&acc4, ca[1], rb[0]);
	__builtin_mma_xvf32ger (&acc5, ca[1], rb[1]);
	__builtin_mma_xvf32ger (&acc6, ca[1], rb[2]);
	__builtin_mma_xvf32ger (&acc7, ca[1], rb[3]);

	S_INCREMENT

	// k loop (unrolled by 4)
	for (int k = 0; k<k_iter; k++)
	{
		S_AB_PRODUCT
		S_AB_PRODUCT
		S_AB_PRODUCT
		S_AB_PRODUCT
	}
	
	// edge loop
	for (int k = 0; k<k_left; k++)
	{
		S_AB_PRODUCT
	}

	// handle beta cases
	if (beta_ != 0.0)
	{
		sgemm_SAVE_ACC_(&acc0, rs_c, 0      );
		sgemm_SAVE_ACC_(&acc1, rs_c, 4      );
		sgemm_SAVE_ACC_(&acc2, rs_c, 8      );
		sgemm_SAVE_ACC_(&acc3, rs_c, 12     );
		sgemm_SAVE_ACC_(&acc4, rs_c,    4*rs_c);
		sgemm_SAVE_ACC_(&acc5, rs_c,  4+4*rs_c);
		sgemm_SAVE_ACC_(&acc6, rs_c,  8+4*rs_c);
		sgemm_SAVE_ACC_(&acc7, rs_c, 12+4*rs_c);
	}
	else
	{
		sgemm_SAVE_ACC_bz( &acc0, rs_c,  0     );
		sgemm_SAVE_ACC_bz( &acc1, rs_c,  4     );
		sgemm_SAVE_ACC_bz( &acc2, rs_c,  8     );
		sgemm_SAVE_ACC_bz( &acc3, rs_c, 12     );
		sgemm_SAVE_ACC_bz( &acc4, rs_c,    4*rs_c);
		sgemm_SAVE_ACC_bz( &acc5, rs_c,  4+4*rs_c);
		sgemm_SAVE_ACC_bz( &acc6, rs_c,  8+4*rs_c);
		sgemm_SAVE_ACC_bz( &acc7, rs_c, 12+4*rs_c);
	}
}

