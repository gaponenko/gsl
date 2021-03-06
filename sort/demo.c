#include <gsl/gsl_rng.h>
#include <gsl/gsl_sort_double.h>

int 
main ()
{
  gsl_rng * r;

  int i, k = 5, N = 100000;

  double * x = malloc (N * sizeof(double));
  double * small = malloc (k * sizeof(double));

  gsl_rng_env_setup();

  r = gsl_rng_alloc (gsl_rng_default);

  for (i = 0; i < N; i++)
    {
      x[i] = gsl_rng_uniform(r);
    }

  gsl_sort_smallest (small, k, x, 1, N);

  printf("%d smallest values from %d\n", k, N);

  for (i = 0; i < k; i++)
    {
      printf ("%d: %.18f\n", i, small[i]);
    }

}
