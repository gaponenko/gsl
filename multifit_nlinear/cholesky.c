/* multifit_nlinear/cholesky.c
 * 
 * Copyright (C) 2015, 2016 Patrick Alken
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or (at
 * your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

/*
 * This module calculates the solution of the normal equations least squares
 * system:
 *
 * [ J~^T J~ + mu D~^T D~ ] p~ = -J~^T f
 *
 * using the modified Cholesky decomposition. Quantities are scaled
 * according to:
 *
 * J~ = J S
 * D~ = D S
 * p~ = S^{-1} p
 *
 * where S is a diagonal matrix and S_jj = || J_j || and J_j is column
 * j of the Jacobian. This balancing transformation seems to be more
 * numerically stable for some Jacobians.
 */

#include <config.h>
#include <gsl/gsl_math.h>
#include <gsl/gsl_vector.h>
#include <gsl/gsl_matrix.h>
#include <gsl/gsl_linalg.h>
#include <gsl/gsl_multifit_nlinear.h>
#include <gsl/gsl_errno.h>
#include <gsl/gsl_blas.h>
#include <gsl/gsl_permutation.h>

#include "common.c"

typedef struct
{
  gsl_matrix *JTJ;           /* J^T J */
  gsl_matrix *work_JTJ;      /* copy of J^T J */
  gsl_matrix *scale_J;       /* scaled Jacobian, J*S, n-by-p */
  gsl_vector *S;             /* scale factors, size p */
  gsl_vector *rhs;           /* -J^T f, size p */
  gsl_permutation *perm;     /* permutation matrix for modified Cholesky */
} cholesky_state_t;

static void *cholesky_alloc (const size_t n, const size_t p);
static int cholesky_init(const void * vtrust_state, void * vstate);
static int cholesky_presolve(const double mu, const void * vtrust_state, void * vstate);
static int cholesky_solve(const gsl_vector * f, gsl_vector *x,
                          const  void * vtrust_state, void *vstate);
static int cholesky_solve_rhs(const gsl_vector * b, gsl_vector *x, cholesky_state_t *state);
static int cholesky_regularize(const double mu, const gsl_vector * diag, gsl_matrix * A,
                               cholesky_state_t * state);

static void *
cholesky_alloc (const size_t n, const size_t p)
{
  cholesky_state_t *state;

  (void)n;
  
  state = calloc(1, sizeof(cholesky_state_t));
  if (state == NULL)
    {
      GSL_ERROR_NULL ("failed to allocate cholesky state", GSL_ENOMEM);
    }

  state->JTJ = gsl_matrix_alloc(p, p);
  if (state->JTJ == NULL)
    {
      GSL_ERROR_NULL ("failed to allocate space for JTJ", GSL_ENOMEM);
    }

  state->work_JTJ = gsl_matrix_alloc(p, p);
  if (state->work_JTJ == NULL)
    {
      GSL_ERROR_NULL ("failed to allocate space for JTJ workspace",
                      GSL_ENOMEM);
    }

  state->scale_J = gsl_matrix_alloc(n, p);
  if (state->scale_J == NULL)
    {
      GSL_ERROR_NULL ("failed to allocate space for scale_J",
                      GSL_ENOMEM);
    }

  state->S = gsl_vector_alloc(p);
  if (state->S == NULL)
    {
      GSL_ERROR_NULL ("failed to allocate space for S", GSL_ENOMEM);
    }

  state->rhs = gsl_vector_alloc(p);
  if (state->rhs == NULL)
    {
      GSL_ERROR_NULL ("failed to allocate space for rhs", GSL_ENOMEM);
    }

  state->perm = gsl_permutation_alloc(p);
  if (state->perm == NULL)
    {
      GSL_ERROR_NULL ("failed to allocate space for perm", GSL_ENOMEM);
    }

  return state;
}

static void
cholesky_free(void *vstate)
{
  cholesky_state_t *state = (cholesky_state_t *) vstate;

  if (state->JTJ)
    gsl_matrix_free(state->JTJ);

  if (state->work_JTJ)
    gsl_matrix_free(state->work_JTJ);

  if (state->scale_J)
    gsl_matrix_free(state->scale_J);

  if (state->S)
    gsl_vector_free(state->S);

  if (state->rhs)
    gsl_vector_free(state->rhs);

  if (state->perm)
    gsl_permutation_free(state->perm);

  free(state);
}

static int
cholesky_init(const void * vtrust_state, void * vstate)
{
  const gsl_multifit_nlinear_trust_state *trust_state =
    (const gsl_multifit_nlinear_trust_state *) vtrust_state;
  cholesky_state_t *state = (cholesky_state_t *) vstate;

  balance_jacobian(trust_state->J, state->scale_J, state->S);

  /* compute J^T J */
  gsl_blas_dsyrk(CblasLower, CblasTrans, 1.0, state->scale_J, 0.0, state->JTJ);

  return GSL_SUCCESS;
}

/*
cholesky_presolve()
  Compute the modified Cholesky decomposition of J^T J + mu D^T D.
Modified Cholesky is used in case mu = 0 and there are rounding
errors in forming J^T J which could lead to an indefinite matrix.

Inputs: mu     - LM parameter
        vstate - workspace

Notes:
1) On output, state->work_JTJ contains the Cholesky decomposition of
J^T J + mu D^T D
*/

static int
cholesky_presolve(const double mu, const void * vtrust_state, void * vstate)
{
  const gsl_multifit_nlinear_trust_state *trust_state =
    (const gsl_multifit_nlinear_trust_state *) vtrust_state;
  cholesky_state_t *state = (cholesky_state_t *) vstate;
  gsl_matrix *JTJ = state->work_JTJ;
  const gsl_vector *diag = trust_state->diag;
  int status;

  /* copy lower triangle of A to workspace */
  gsl_matrix_tricpy('L', 1, JTJ, state->JTJ);

  /* augment normal equations: A -> A + mu D^T D */
  status = cholesky_regularize(mu, diag, JTJ, state);
  if (status)
    return status;

  /* compute modified Cholesky decomposition */
  status = gsl_linalg_mcholesky_decomp(JTJ, state->perm, NULL);
  if (status)
    return status;

  return GSL_SUCCESS;
}

/*
cholesky_solve()
  Compute (J^T J + mu D^T D) x = -J^T f

Inputs: f      - right hand side vector f
        x      - (output) solution vector
        vstate - cholesky workspace
*/

static int
cholesky_solve(const gsl_vector * f, gsl_vector *x,
               const  void * vtrust_state, void *vstate)
{
  cholesky_state_t *state = (cholesky_state_t *) vstate;
  int status;

  /* compute rhs = -J^T f */
  gsl_blas_dgemv(CblasTrans, -1.0, state->scale_J, f, 0.0, state->rhs);

  status = cholesky_solve_rhs(state->rhs, x, state);
  if (status)
    return status;

  /* undo balancing transformation */
  gsl_vector_mul(x, state->S);

  (void) vtrust_state;

  return GSL_SUCCESS;
}

/* solve: (J^T J + mu D^T D) x = b */
static int
cholesky_solve_rhs(const gsl_vector * b, gsl_vector *x, cholesky_state_t *state)
{
  int status;
  gsl_matrix *JTJ = state->work_JTJ;

  status = gsl_linalg_mcholesky_solve(JTJ, state->perm, b, x);
  if (status)
    return status;

  return GSL_SUCCESS;
}

/* A <- A + mu D~^T D~, where D~ = D*S */
static int
cholesky_regularize(const double mu, const gsl_vector * diag, gsl_matrix * A,
                    cholesky_state_t * state)
{
  if (mu != 0.0)
    {
      size_t i;

      for (i = 0; i < diag->size; ++i)
        {
          double di = gsl_vector_get(diag, i);
          double si = gsl_vector_get(state->S, i);
          double yi = di * si;
          double *Aii = gsl_matrix_ptr(A, i, i);
          *Aii += mu * yi * yi;
        }
    }

  return GSL_SUCCESS;
}

static const gsl_multifit_nlinear_solver cholesky_type =
{
  "cholesky",
  cholesky_alloc,
  cholesky_init,
  cholesky_presolve,
  cholesky_solve,
  NULL, /* XXX */
  cholesky_free
};

const gsl_multifit_nlinear_solver *gsl_multifit_nlinear_solver_cholesky = &cholesky_type;
