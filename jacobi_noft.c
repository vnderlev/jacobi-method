#include "jacobi.h"
#include <math.h>
#include <mpi.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>

/**
 * Prints the minimum and maximum timings of a specific loop in the program.
 *
 * @param scomm  MPI communicator for the processes involved in the timings.
 * @param rank   Rank of the current MPI process.
 * @param twf    Time (in seconds) taken for the specific loop in the current
 * MPI process.
 */
void print_timings(MPI_Comm scomm, int rank, double twf) {
  // Storage for min and max times
  double mtwf, Mtwf;

  // Perform reduction to find the minimum time across all MPI processes
  MPI_Reduce(&twf, &mtwf, 1, MPI_DOUBLE, MPI_MIN, 0, scomm);

  // Perform reduction to find the maximum time across all MPI processes
  MPI_Reduce(&twf, &Mtwf, 1, MPI_DOUBLE, MPI_MAX, 0, scomm);

  // If the current process is rank 0, print the min and max timings
  if (0 == rank) {
    printf("##### Measured Iteration Timings #####\n"
           "# MIN: %.2f ms \t MAX: %.2f ms\n",
           mtwf * 1000, Mtwf * 1000);
  }
}

void create_png(const char *filename, TYPE *matrix, int nb, int mb) {
  FILE *fp = fopen(filename, "wb");
  if (!fp) {
    perror("Failed to open file for PNG output");
    return;
  }

  png_structp png =
      png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  png_infop info = png_create_info_struct(png);
  if (!png || !info) {
    fclose(fp);
    return;
  }

  if (setjmp(png_jmpbuf(png))) {
    fclose(fp);
    png_destroy_write_struct(&png, &info);
    return;
  }

  png_init_io(png, fp);

  png_set_IHDR(png, info, nb, mb, 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
               PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);

  png_bytep *row_pointers = malloc(sizeof(png_bytep) * mb);
  for (int j = 0; j < mb; j++) {
    row_pointers[j] = malloc(sizeof(png_byte) * 3 * nb);
    for (int i = 0; i < nb; i++) {
      int pos = (j + 1) * (nb + 2) + 1 + i; // Skip the ghost rows/columns
      double value = matrix[pos];
      double normalized = (value + 20) / (20 + 20);
      normalized = normalized < 0 ? 0 : (normalized > 1 ? 1 : normalized);

      int r = (int)(normalized * 255);
      int g = 0;
      int b = 255 - r;

      row_pointers[j][i * 3 + 0] = r; // Red
      row_pointers[j][i * 3 + 1] = g; // Green
      row_pointers[j][i * 3 + 2] = b; // Blue
    }
  }

  png_set_rows(png, info, row_pointers);
  png_write_png(png, info, PNG_TRANSFORM_IDENTITY, NULL);

  for (int j = 0; j < mb; j++) {
    free(row_pointers[j]);
  }
  free(row_pointers);
  png_destroy_write_struct(&png, &info);
  fclose(fp);
}

/**
 * Performs one iteration of the Successive Over-Relaxation (SOR) method
 * on the input matrix and computes the squared L2-norm of the difference
 * between the input and output matrices.
 *
 * @param nm   Pointer to the output matrix after one iteration of the SOR
 * method.
 * @param om   Pointer to the input matrix.
 * @param nb   Number of columns in the input matrix.
 * @param mb   Number of rows in the input matrix.
 * @return     The squared L2-norm of the difference between the input and
 * output matrices.
 */
TYPE SOR1(TYPE *nm, TYPE *om, int nb, int mb) {
  TYPE norm = 0.0;
  TYPE _W = 2.0 / (1.0 + M_PI / (TYPE)nb);
  int i, j, pos;

  // Iterate through each element of the matrix
  for (j = 0; j < mb; j++) {
    for (i = 0; i < nb; i++) {
      // Compute the position of the current element
      pos = 1 + i + (j + 1) * (nb + 2);

      // Update the current element using the SOR method
      nm[pos] =
          (1 - _W) * om[pos] + _W / 4.0 *
                                   (nm[pos - 1] + om[pos + 1] +
                                    nm[pos - (nb + 2)] + om[pos + (nb + 2)]);

      // Accumulate the squared L2-norm of the difference
      norm += (nm[pos] - om[pos]) * (nm[pos] - om[pos]);
    }
  }

  return norm;
}

/**
 * Performs any required pre-initialization steps for the Jacobi method.
 * This function is a placeholder that can be extended if needed.
 *
 * @return     0 on successful completion.
 */
int preinit_jacobi_cpu(void) {
  // Currently, there are no pre-initialization steps required for the
  // Jacobi method on the CPU. This function serves as a placeholder and
  // can be extended if necessary.

  return 0;
}

int jacobi_cpu(TYPE *matrix, int NB, int MB, int P, int Q, MPI_Comm comm,
               TYPE epsilon, int max_iter, int save_output) {
  int i, iter = 0;
  int rank, size, ew_rank, ew_size, ns_rank, ns_size;
  TYPE *om, *nm, *tmpm, *send_east, *send_west, *recv_east, *recv_west,
      diff_norm;
  double start, twf = 0; /* timings */
  MPI_Comm ns, ew;
  MPI_Request req[8] = {MPI_REQUEST_NULL, MPI_REQUEST_NULL, MPI_REQUEST_NULL,
                        MPI_REQUEST_NULL, MPI_REQUEST_NULL, MPI_REQUEST_NULL,
                        MPI_REQUEST_NULL, MPI_REQUEST_NULL};

  MPI_Comm_rank(comm, &rank);
  MPI_Comm_size(comm, &size);

  om = matrix;
  nm = (TYPE *)calloc(sizeof(TYPE), (NB + 2) * (MB + 2));
  send_east = (TYPE *)malloc(sizeof(TYPE) * MB);
  send_west = (TYPE *)malloc(sizeof(TYPE) * MB);
  recv_east = (TYPE *)malloc(sizeof(TYPE) * MB);
  recv_west = (TYPE *)malloc(sizeof(TYPE) * MB);

  /* create the north-south and east-west communicator */
  MPI_Comm_split(comm, rank % P, rank, &ns);
  MPI_Comm_size(ns, &ns_size);
  MPI_Comm_rank(ns, &ns_rank);
  MPI_Comm_split(comm, rank / P, rank, &ew);
  MPI_Comm_size(ew, &ew_size);
  MPI_Comm_rank(ew, &ew_rank);

  start = MPI_Wtime();
  do {
    if (rank == 0) {
      printf("Iteration %d: diff_norm = %f, epsilon = %f\n", iter,
             sqrt(diff_norm), epsilon);
    }
    if (save_output) {
      char filename[256];
      snprintf(filename, sizeof(filename), "pngs/rank_%d_iteration_%04d.png",
               rank, iter);
      create_png(filename, om, NB, MB);
    }

    /* post receives from the neighbors */
    if (0 != ns_rank)
      MPI_Irecv(RECV_NORTH(om), NB, MPI_TYPE, ns_rank - 1, 0, ns, &req[0]);
    if ((ns_size - 1) != ns_rank)
      MPI_Irecv(RECV_SOUTH(om), NB, MPI_TYPE, ns_rank + 1, 0, ns, &req[1]);
    if ((ew_size - 1) != ew_rank)
      MPI_Irecv(recv_east, MB, MPI_TYPE, ew_rank + 1, 0, ew, &req[2]);
    if (0 != ew_rank)
      MPI_Irecv(recv_west, MB, MPI_TYPE, ew_rank - 1, 0, ew, &req[3]);

    /* post the sends */
    if (0 != ns_rank)
      MPI_Isend(SEND_NORTH(om), NB, MPI_TYPE, ns_rank - 1, 0, ns, &req[4]);
    if ((ns_size - 1) != ns_rank)
      MPI_Isend(SEND_SOUTH(om), NB, MPI_TYPE, ns_rank + 1, 0, ns, &req[5]);
    for (i = 0; i < MB; i++) {
      send_west[i] = om[(i + 1) * (NB + 2) + 1];      /* the real local data */
      send_east[i] = om[(i + 1) * (NB + 2) + NB + 0]; /* not the ghost region */
    }
    if ((ew_size - 1) != ew_rank)
      MPI_Isend(send_east, MB, MPI_TYPE, ew_rank + 1, 0, ew, &req[6]);
    if (0 != ew_rank)
      MPI_Isend(send_west, MB, MPI_TYPE, ew_rank - 1, 0, ew, &req[7]);
    /* wait until they all complete */
    MPI_Waitall(8, req, MPI_STATUSES_IGNORE);

    /* unpack the east-west newly received data */
    for (i = 0; i < MB; i++) {
      om[(i + 1) * (NB + 2)] = recv_west[i];
      om[(i + 1) * (NB + 2) + NB + 1] = recv_east[i];
    }

    /**
     * Call the Successive Over Relaxation (SOR) method
     */
    diff_norm = SOR1(nm, om, NB, MB);

    MPI_Allreduce(MPI_IN_PLACE, &diff_norm, 1, MPI_TYPE, MPI_SUM, comm);

    tmpm = om;
    om = nm;
    nm = tmpm; /* swap the 2 matrices */
    iter++;
  } while (iter < max_iter); // && (sqrt(diff_norm) > epsilon)

  twf = MPI_Wtime() - start;
  print_timings(comm, rank, twf);

  if (matrix != om)
    free(om);
  else
    free(nm);
  free(send_west);
  free(send_east);
  free(recv_west);
  free(recv_east);

  MPI_Comm_free(&ns);
  MPI_Comm_free(&ew);

  return iter;
}
