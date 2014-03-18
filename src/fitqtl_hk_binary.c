/**********************************************************************
 * 
 * fitqtl_hk_binary.c
 *
 * copyright (c) 2010-2014, Karl W Broman
 *
 * last modified Mar, 2014
 * first written Jun, 2010
 *
 *     This program is free software; you can redistribute it and/or
 *     modify it under the terms of the GNU General Public License,
 *     version 3, as published by the Free Software Foundation.
 * 
 *     This program is distributed in the hope that it will be useful,
 *     but without any warranty; without even the implied warranty of
 *     merchantability or fitness for a particular purpose.  See the GNU
 *     General Public License, version 3, for more details.
 * 
 *     A copy of the GNU General Public License, version 3, is available
 *     at http://www.r-project.org/Licenses/GPL-3
 *
 * C functions for the R/qtl package
 *
 * These functions are for fitting a fixed multiple-QTL model for a binary 
 * trait, by Haley-Knott regression.
 *
 * Contains: R_fitqtl_hk_binary, fitqtl_hk_binary, galtLODHKbin, nullLODbin
 *
 **********************************************************************/

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <R.h>
#include <Rmath.h>
#include <R_ext/PrtUtil.h>
#include <R_ext/Applic.h>
#include <R_ext/Linpack.h>
#include <R_ext/Utils.h>
#include "util.h"
#include "fitqtl_hk_binary.h"
#define TOL 1e-12

void R_fitqtl_hk_binary(int *n_ind, int *n_qtl, int *n_gen, 
			double *genoprob, int *n_cov, double *cov, int *model, 
			int *n_int, double *pheno, int *get_ests,
			/* return variables */
			double *lod, int *df, double *ests, double *ests_covar,
			double *design_mat, 
			/* convergence */
			double *tol, int *maxit, int *matrix_rank)
{
  double ***Genoprob=0, **Cov=0;
  int tot_gen, i, j, curpos;

  /* reorganize genotype probabilities */
  if(*n_qtl > 0) {
    Genoprob = (double ***)R_alloc(*n_qtl, sizeof(double **));
    tot_gen = 0;
    for(i=0; i < *n_qtl; i++)
      tot_gen += (n_gen[i]+1);
    Genoprob[0] = (double **)R_alloc(tot_gen, sizeof(double *));
    for(i=1; i < *n_qtl; i++)
      Genoprob[i] = Genoprob[i-1] + (n_gen[i-1]+1);
    for(i=0, curpos=0; i < *n_qtl; i++) 
      for(j=0; j<n_gen[i]+1; j++, curpos += *n_ind)
	Genoprob[i][j] = genoprob + curpos;
  }

  /* reorganize cov (if they are not empty) */
  /* currently reorg_errlod function is used to reorganize the data */
  if(*n_cov != 0) reorg_errlod(*n_ind, *n_cov, cov, &Cov);

  fitqtl_hk_binary(*n_ind, *n_qtl, n_gen, Genoprob, 
		   Cov, *n_cov, model, *n_int, pheno, *get_ests, lod, df,
		   ests, ests_covar, design_mat, *tol, *maxit, matrix_rank); 
}


/**********************************************************************
 * 
 * fitqtl_hk_binary
 *
 * Fits a fixed multiple-QTL model by Haley-Knott regression.
 * 
 * n_ind        Number of individuals
 *
 * n_qtl        Number of QTLs in the model 
 *
 * n_gen        Number of different genotypes (really no. genotypes - 1)
 *
 * Genoprob     QTL genotype probabilities
 *
 * Cov          covariates matrix, Cov[mar][ind]
 *
 * n_cov        Number of covariates
 *
 * model        Model matrix
 *
 * n_int        Number of interactions in the model
 *
 * pheno        Phenotype data, as a vector
 *
 * get_ests     0/1: If 1, return estimated effects and their variances
 *
 * lod          Return LOD score
 *
 * df           Return degree of freedom
 *
 * ests         Return ests (vector of length sizefull)
 *
 * ests_covar   Return covariance matrix of ests (sizefull^2 matrix)
 *
 * tol          Tolerance for convergence
 * 
 * maxit        Maximum number of iterations in IRLS
 *
 * matrix_rank  On return, rank of the X matrix
 *
 **********************************************************************/

void fitqtl_hk_binary(int n_ind, int n_qtl, int *n_gen, double ***Genoprob,
		      double **Cov, int n_cov, 
		      int *model, int n_int, double *pheno, int get_ests,
		      double *lod, int *df, double *ests, double *ests_covar,
		      double *design_mat, double tol, int maxit, int *matrix_rank) 
{

  /* create local variables */
  int i, j, n_qc, itmp; /* loop variants and temp variables */
  double llik, llik0;
  double *dwork, **Ests_covar=0;
  int *iwork, sizefull;

  /* initialization */
  sizefull = 1;

  /* calculate the dimension of the design matrix for full model */
  n_qc = n_qtl+n_cov; /* total number of QTLs and covariates */
  /* for additive QTLs and covariates*/
  for(i=0; i<n_qc; i++)
    sizefull += n_gen[i];
  /* for interactions, loop thru all interactions */
  for(i=0; i<n_int; i++) { 
    for(j=0, itmp=1; j<n_qc; j++) {
      if(model[i*n_qc+j])
	itmp *= n_gen[j];
    }
    sizefull += itmp; 
  }

  /* reorganize Ests_covar for easy use later */
  /* and make space for estimates and covariance matrix */
  if(get_ests) 
    reorg_errlod(sizefull, sizefull, ests_covar, &Ests_covar);

  /* allocate memory for working arrays, total memory is
     sizefull*n_ind+6*n_ind+4*sizefull for double array, 
     and sizefull for integer array.
     All memory will be allocated one time and split later */
  dwork = (double *)R_alloc(sizefull*n_ind+6*n_ind+4*sizefull,
			    sizeof(double));
  iwork = (int *)R_alloc(sizefull, sizeof(int));


  /* calculate null model log10 likelihood */
  llik0 = nullLODbin(pheno, n_ind);

  R_CheckUserInterrupt(); /* check for ^C */

  /* fit the model */
  llik = galtLODHKbin(pheno, n_ind, n_gen, n_qtl, Genoprob,
		      Cov, n_cov, model, n_int, dwork, iwork, 
		      sizefull, get_ests, ests, Ests_covar,
		      design_mat, tol, maxit, matrix_rank);

  *lod = llik - llik0;

  /* degree of freedom equals to the number of columns of x minus 1 (mean) */
  *df = sizefull - 1;
}


/* nullLODbin - calculate null log10 likelihood */
double nullLODbin(double *pheno, int n_phe)
{
  int i;
  double llik=0.0, pi0=0.0, q0;

  /* calculate proportion of affecteds */
  for(i=0; i<n_phe; i++) pi0 += pheno[i];
  pi0 /= (double)n_phe;
  q0 = log10(1.0 - pi0);
  pi0 = log10(pi0);

  for(i=0; i<n_phe; i++) 
    llik += pheno[i]*pi0 + (1.0-pheno[i])*q0;

  return(llik);
}
  



/* galtLODHKbin - calculate log10 lik for full model for binary trait by Haley-Knott regression */
double galtLODHKbin(double *pheno, int n_ind, int *n_gen, int n_qtl, 
		    double ***Genoprob, double **Cov, int n_cov, int *model, 
		    int n_int, double *dwork, int *iwork, int sizefull,
		    int get_ests, double *ests, double **Ests_covar,
		    double *designmat, double tol, int maxit, int *matrix_rank) 
{
  /* local variables */
  int i, j, k, kk, *jpvt, ny, idx_col, n_qc, n_int_col, job, outerrep, s;
  double *work, *qty, *qraux, *coef, *resid, tol2, **X;
  int n_int_q, *idx_int_q=0;
  int nrep, thisidx, gen, totrep, thecol, rep, flag;
  double *nu, *pi, *z, *wt;
  /* return variable */
  double llik=0.0, curllik;

  /* initialization */
  ny = 1; 
  tol2 = TOL;
  n_qc = n_qtl + n_cov;
  if(n_qtl > 0) idx_int_q = (int *)R_alloc(n_qtl, sizeof(int));
  X = (double **)R_alloc(sizefull, sizeof(double *));

  /* split the memory block: 
     design matrix x will be (n_ind x sizefull), coef will be (1 x sizefull),
     resid will be (1 x n_ind), qty will be (1 x n_ind), 
     pi, nu, z, and wt are each (1 x n_ind),
     qraux will be (1 x sizefull), work will be (2 x sizefull) */
  X[0] = dwork;
  for(i=1; i<sizefull; i++) X[i] = X[i-1] + n_ind;
  coef = dwork + n_ind*sizefull;
  resid = coef + sizefull;
  qty = resid + n_ind;
  pi = qty + n_ind;
  z = pi + n_ind;
  nu = z + n_ind;
  wt = nu + n_ind;
  qraux = wt + n_ind;
  work = qraux + sizefull; 
  /* integer array */
  jpvt = iwork;
  
  /******************************************************
   The following part will construct the design matrix x 
   ******************************************************/
  /* fill first row with 1s. It's corresponding to the mean */
  for(i=0; i<n_ind; i++) X[0][i] = 1.0;
  idx_col = 1;  /* increment column index */

  /*****************
   * Additive terms 
   *****************/
  /* loop thru QTLs */
  /* if the geno type is one, do nothing (the effects go to the means).
     Otherwise, set proper entry in x to be 1. The idea is that for 
     backcross, genotype 1 -> 0; 2 -> 1. For F2, genotype 1  -> [0 0]; 
     2 -> [1 0]; 3 ->[0 1]. For 4-way, 1 -> [0 0 0], 2 -> [1 0 0],
     3 -> [0 1 0], 4 -> [0 0 1] and so on */
  for(i=0; i<n_qtl; i++) {
    for(k=0; k<n_gen[i]; k++) { /* this is confusing; remember n_gen is 1 fewer than no. genotypes */
      for(j=0; j<n_ind; j++) /*loop thru individuals */
	X[idx_col][j] = Genoprob[i][k+1][j];
      idx_col++;
    }
  }

  /* loop thru covariates */
  for(i=0; i<n_cov; i++) {
    for(j=0; j<n_ind; j++)  /* loop individuals */
      X[idx_col][j] = Cov[i][j];
    idx_col ++; /* increment idx_col by 1 */
  }

  /* put 1's in the remaining columns */
  for(i=idx_col; i<sizefull; i++) 
    for(j=0; j<n_ind; j++) 
      X[i][j] = 1.0;

  /*******************
   * interactive terms 
   *******************/
  /* loop thru interactions */
  for(i=0; i<n_int; i++) {
    n_int_q = 0;
   /* total number of columns in the design matrix for this interaction */
    n_int_col = 1; 
    /* parse the model matrix */
    for(j=0; j<n_qtl; j++) { 
      if(model[i*n_qc+j]) { /* this QTL is in the interaction */
	idx_int_q[n_int_q] = j;
	n_int_q ++;
	n_int_col *= n_gen[j]; 
      }
    }

    nrep = 1; 
    for(k=n_int_q-1; k>=0; k--) { /* go through QTL involved in this interaction */
      thisidx = idx_int_q[k];
      
      totrep = n_int_col / (n_gen[thisidx] * nrep);
      thecol = 0;
      for(outerrep=0; outerrep < totrep; outerrep++) {
	for(gen=0; gen<n_gen[thisidx]; gen++)
	  for(rep=0; rep<nrep; rep++, thecol++) 
	    for(j=0; j<n_ind; j++) 
	      X[idx_col+thecol][j] *= Genoprob[thisidx][gen+1][j];
      }
      nrep *= n_gen[thisidx];
    }

    for(k=0; k<n_cov; k++) { /* covariates in the interaction */
      if(model[i*n_qc+(n_qtl+k)]) {
	for(thecol=0; thecol<n_int_col; thecol++)
	  for(j=0; j<n_ind; j++) 
	    X[idx_col+thecol][j] *= Cov[k][j];
      }
    }
	
    idx_col += n_int_col;
  } /* finish the loop for interaction */
  /* finish design matrix construction */

  /* save design matrix */
  memcpy(designmat, X[0], sizefull*n_ind*sizeof(double));

  /* starting point for IRLS */
  curllik = 0.0;
  for(j=0; j<n_ind; j++) {
    pi[j] = (pheno[j] + 0.5)/2.0;
    wt[j] = sqrt(pi[j] * (1.0-pi[j]));
    nu[j] = log(pi[j]) - log(1.0-pi[j]);
    z[j] = nu[j]*wt[j] + (pheno[j] - pi[j])/wt[j];
    curllik += pheno[j] * log10(pi[j]) + (1.0-pheno[j]) * log10(1.0 - pi[j]);
  }

  /* multiply design matrix by current wts */
  for(i=0; i<sizefull; i++) 
    for(j=0; j<n_ind; j++)
      X[i][j] *= wt[j];

  flag = 0;
  for(s=0; s<maxit; s++) { /* IRLS iterations */
  
    R_CheckUserInterrupt(); /* check for ^C */

    /* make jpvt = numbers 0, 1, ..., (sizefull-1) */
    /*      jpvt keeps track of any permutation of X columns */
    for(i=0; i<sizefull; i++) jpvt[i] = i;

    /* call dqrls to fit regression model */
    F77_CALL(dqrls)(X[0], &n_ind, &sizefull, z, &ny, &tol2, coef, resid,
		    qty, &kk, jpvt, qraux, work);
    /* on output, kk contains the rank */
    *matrix_rank = kk;

    /* get ests; need to permute back */
    for(i=0; i<kk; i++) ests[jpvt[i]] = coef[i];
    for(i=kk; i<sizefull; i++) ests[jpvt[i]] = 0.0;
    
    /* re-form design matrix */
    memcpy(X[0], designmat, sizefull*n_ind*sizeof(double));

    /* calculate fitted values, probs, new wts, new z's */
    llik = 0.0;
    for(j=0; j<n_ind; j++) {
      nu[j] = 0.0;
      for(i=0; i<sizefull; i++) 
	nu[j] += X[i][j] * ests[i];
      pi[j] = exp(nu[j]);
      pi[j] /= (1.0 + pi[j]);
      wt[j] = sqrt(pi[j] * (1.0-pi[j]));
      z[j] = nu[j]*wt[j] + (pheno[j] - pi[j])/wt[j];
      llik += (pheno[j] * log10(pi[j]) + (1.0-pheno[j]) * log10(1.0 - pi[j]));

      /* multiply design matrix by new weights */
      for(i=0; i<sizefull; i++) 
	X[i][j] *= wt[j];
    }

    if(fabs(llik - curllik) < tol) { /* converged? */
      flag = 1;
      break;
    }
    curllik = llik;

  } /* end of IRLS iterations */

  if(!flag)
    warning("Didn't converge.");

  if(get_ests) { /* get the estimates and their covariance matrix */

    /* need to re-run the last regression */
    F77_CALL(dqrls)(X[0], &n_ind, &sizefull, z, &ny, &tol2, coef, resid,
		    qty, &kk, jpvt, qraux, work);

    /* get covariance matrix: dpodi to get (X'X)^-1; re-sort */
    job = 1; /* indicates to dpodi to get inverse and not determinant */
    F77_CALL(dpodi)(X[0], &n_ind, &sizefull, work, &job);

    for(i=0; i<kk; i++) 
      for(j=i; j<kk; j++) 
	Ests_covar[jpvt[i]][jpvt[j]] = Ests_covar[jpvt[j]][jpvt[i]] = X[j][i];
    for(i=kk; i<sizefull; i++)
      for(j=0; j<kk; j++)
	Ests_covar[jpvt[i]][j] = Ests_covar[j][jpvt[i]] = 0.0;
  }

  return(llik);
}


/* end of fitqtl_hk_binary.c */
