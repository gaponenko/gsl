/* multilarge_nlinear/common.c
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

static void scaled_addition (const double alpha, const gsl_vector * x,
                             const double beta, const gsl_vector * y,
                             gsl_vector * z);
static double quadratic_preduction(const gsl_multilarge_nlinear_trust_state * trust_state,
                                   const gsl_vector * dx, gsl_vector * work);

/* compute z = alpha*x + beta*y */
static void
scaled_addition (const double alpha, const gsl_vector * x,
                 const double beta, const gsl_vector * y, gsl_vector * z)
{
  const size_t N = z->size;
  size_t i;

  for (i = 0; i < N; i++)
    {
      double xi = gsl_vector_get (x, i);
      double yi = gsl_vector_get (y, i);
      gsl_vector_set (z, i, alpha * xi + beta * yi);
    }
}

/*
quadratic_preduction()
  Calculate predicted reduction based on standard
quadratic model:

m_k(dx) = Phi(x_k) + dx' g + 1/2 dx' B_k dx

predicted_reduction = m_k(0) - m_k(dx)
                    = -2 g^T dx / ||f||^2 - ( ||J*dx|| / ||f|| )^2
                    = -2 fhat . beta - ||beta||^2

where: beta = J*dx / ||f||

Inputs: trust_state - trust state
        dx          - proposed step, size p
        work        - workspace, size n

Return: predicted reduction
*/

static double
quadratic_preduction(const gsl_multilarge_nlinear_trust_state * trust_state,
                     const gsl_vector * dx, gsl_vector * work)
{
  int status;
  const gsl_vector * x = trust_state->x;
  const gsl_vector * f = trust_state->f;
  const gsl_vector * swts = trust_state->sqrt_wts;
  const gsl_multilarge_nlinear_parameters * params = trust_state->params;
  const size_t n = f->size;
  const double normf = gsl_blas_dnrm2(f);
  gsl_multilarge_nlinear_fdf * fdf = trust_state->fdf;
  double pred_reduction;
  double norm_beta; /* ||J*dx|| / ||f|| */
  size_t i;

  /* compute work = J*dx */
  status = gsl_multilarge_nlinear_eval_df(CblasNoTrans, x, f, dx,
                                          swts, params->h_df, params->fdtype,
                                          fdf, work, NULL);
  if (status)
    {
      GSL_ERROR_VAL("error computing preduction", status, 0.0);
    }

  /* compute beta = J*dx / ||f|| */
  gsl_vector_scale(work, 1.0 / normf);
  norm_beta = gsl_blas_dnrm2(work);

  /* initialize to ( ||J*dx|| / ||f|| )^2 */
  pred_reduction = -norm_beta * norm_beta;

  /* subtract 2*fhat.beta */
  for (i = 0; i < n; ++i)
    {
      double fi = gsl_vector_get(f, i);
      double betai = gsl_vector_get(work, i);

      pred_reduction -= 2.0 * (fi / normf) * betai;
    }

  return pred_reduction;
}
