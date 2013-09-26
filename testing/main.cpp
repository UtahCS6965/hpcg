
//@HEADER
// ************************************************************************
//
//               HPCG: Simple Conjugate Gradient Benchmark Code
// Questions? Contact Michael A. Heroux (maherou@sandia.gov)
//
// ************************************************************************
//@HEADER

/*!
 @file main.cpp

 HPCG rouine
 */

// Main routine of a program that calls the HPCG conjugate gradient
// solver to solve the problem, and then prints results.

#include <fstream>
#include <iostream>
#include <cstdlib>
#ifdef HPCG_DETAILED_DEBUG
using std::cin;
#endif
using std::endl;

#include <vector>

#include "hpcg.hpp"

#ifndef HPCG_NOMPI
#include <mpi.h> // If this routine is not compiled with HPCG_NOMPI
#endif

#include "GenerateGeometry.hpp"
#include "GenerateProblem.hpp"
#include "SetupHalo.hpp"
#include "ExchangeHalo.hpp"
#include "OptimizeProblem.hpp"
#include "WriteProblem.hpp"
#include "ReportResults.hpp"
#include "mytimer.hpp"
#include "ComputeSPMV_ref.hpp"
#include "ComputeSYMGS_ref.hpp"
#include "ComputeResidual.hpp"
#include "CG.hpp"
#include "CG_ref.hpp"
#include "Geometry.hpp"
#include "SparseMatrix.hpp"
#include "CGData.hpp"
#include "TestCG.hpp"
#include "TestSymmetry.hpp"
#include "TestNorms.hpp"

/*!
  Main driver program: Construct synthetic problem, run V&V tests, compute benchmark parameters, run benchmark, report results.

  @param[in]  argc Standard argument count.  Should equal 1 (no arguments passed in) or 4 (nx, ny, nz passed in)
  @param[in]  argv Standard argument array.  If argc==1, argv is unused.  If argc==4, argv[1], argv[2], argv[3] will be interpreted as nx, ny, nz, resp.

  @return Returns zero on success and a non-zero value otherwise.

*/
int main(int argc, char * argv[]) {

#ifndef HPCG_NOMPI
  MPI_Init(&argc, &argv);
#endif

  HPCG_Params params;

  HPCG_Init(&argc, &argv, params);

  int size = params.comm_size, rank = params.comm_rank; // Number of MPI processes, My process ID

#ifdef HPCG_DETAILED_DEBUG
  if (size < 100 && rank==0) HPCG_fout << "Process "<<rank<<" of "<<size<<" is alive with " << params.numThreads << " threads." <<endl;

  if (rank==0) {
    int junk = 0;
    HPCG_fout << "Press enter to continue"<< endl;
    cin >> junk;
  }
#ifndef HPCG_NOMPI
  MPI_Barrier(MPI_COMM_WORLD);
#endif
#endif

  local_int_t nx,ny,nz;
  nx = (local_int_t)params.nx;
  ny = (local_int_t)params.ny;
  nz = (local_int_t)params.nz;
  int ierr = 0;  // Used to check return codes on function calls

  // //////////////////////
  // Problem setup Phase //
  /////////////////////////

#ifdef HPCG_DEBUG
  double t1 = mytimer();
#endif

  // Construct the geometry and linear system
  Geometry geom;
  GenerateGeometry(size, rank, params.numThreads, nx, ny, nz, geom);

  SparseMatrix A;
  CGData data;
  double * b, *x, *xexact;
  GenerateProblem(geom, A, &b, &x, &xexact);
  SetupHalo(geom, A);
  InitializeSparseCGData(A, data);


  // Use this array for collecting timing information
  std::vector< double > times(9,0.0);

  // Call user-tunable set up function.
  double t7 = mytimer(); OptimizeProblem(geom, A, data, b, x, xexact); t7 = mytimer() - t7;
  times[7] = t7;
#ifdef HPCG_DEBUG
  if (rank==0) HPCG_fout << "Total problem setup time in main (sec) = " << mytimer() - t1 << endl;
#endif

#ifdef HPCG_DETAILED_DEBUG
  if (geom.size==1) WriteProblem(geom, A, b, x, xexact);
#endif


  //////////////////////////////
  // Validation Testing Phase //
  //////////////////////////////

#ifdef HPCG_DEBUG
  t1 = mytimer();
#endif
  TestCGData testcg_data;
  testcg_data.count_pass = testcg_data.count_fail = 0;
  TestCG(geom, A, data, b, x, &testcg_data);

  TestSymmetryData testsymmetry_data;
  TestSymmetry(geom, A, b, xexact, &testsymmetry_data);

#ifdef HPCG_DEBUG
  if (rank==0) HPCG_fout << "Total validation (TestCG and TestSymmetry) execution time in main (sec) = " << mytimer() - t1 << endl;
#endif

#ifdef HPCG_DEBUG
  t1 = mytimer();
#endif

  ///////////////////////////////////////
  // Reference SpMV+SymGS Timing Phase //
  ///////////////////////////////////////

  // Call Reference SpMV and SYMGS. Compute Optimization time as ratio of times in these routines

  local_int_t nrow = A.localNumberOfRows;
  local_int_t ncol = A.localNumberOfColumns;

  double * x_overlap = new double [ncol]; // Overlapped copy of x vector
  double * b_computed = new double [nrow]; // Computed RHS vector


  // Record execution time of reference SpMV and SymGS kernels for reporting times
  // First load vector with random values
  for (int i=0; i<nrow; ++i) {
    x_overlap[i] = ((double) rand() / (RAND_MAX)) + 1;
  }

  int numberOfCalls = 10;
  double t_begin = mytimer();
  for (int i=0; i< numberOfCalls; ++i) {
#ifndef HPCG_NOMPI
    ExchangeHalo(A,x_overlap);
#endif
    ierr = ComputeSPMV_ref(A, x_overlap, b_computed); // b_computed = A*x_overlap
    if (ierr) HPCG_fout << "Error in call to SpMV: " << ierr << ".\n" << endl;
    ierr = ComputeSYMGS_ref(A, x_overlap, b_computed); // b_computed = Minv*y_overlap
    if (ierr) HPCG_fout << "Error in call to SymGS: " << ierr << ".\n" << endl;
  }
  times[8] = (mytimer() - t_begin)/((double) numberOfCalls);  // Total time divided by number of calls.

#ifdef HPCG_DEBUG
  if (rank==0) HPCG_fout << "Total SpMV+SymGS timing phase execution time in main (sec) = " << mytimer() - t1 << endl;
#endif

  ///////////////////////////////
  // Reference CG Timing Phase //
  ///////////////////////////////

#ifdef HPCG_DEBUG
  t1 = mytimer();
#endif
  int global_failure = 0; // assume all is well: no failures

  int niters = 0;
  int totalNiters = 0;
  double normr = 0.0;
  double normr0 = 0.0;
  int maxIters = 50;
  numberOfCalls = 1; // Only need to run the residual reduction analysis once

  // Compute the residual reduction for the natural ordering and reference kernels
  std::vector< double > ref_times(9,0.0);
  double tolerance = 0.0; // Set tolerance to zero to make all runs do max_iter iterations
  int err_count = 0;
  for (int i=0; i< numberOfCalls; ++i) {
    for (int j=0; j< A.localNumberOfRows; ++j) x[j] = 0.0; // start x at all zeros
    ierr = CG_ref( geom, A, data, b, x, maxIters, tolerance, niters, normr, normr0, &ref_times[0], true);
    if (ierr) ++err_count; // count the number of errors in CG
    totalNiters += niters;
  }
  if (rank == 0 && err_count) HPCG_fout << err_count << " error(s) in call(s) to reference CG." << endl;
  double ref_tolerance = normr / normr0;

  //////////////////////////////
  // Optimized CG Setup Phase //
  //////////////////////////////

  totalNiters = 0;
  niters = 0;
  normr = 0.0;
  normr0 = 0.0;
  err_count = 0;
  int tolerance_failures = 0;

  int opt_maxIters = 10*maxIters;
  int opt_iters = 0;
  double opt_worst_time = 0.0;

  std::vector< double > opt_times(9,0.0);

  // Compute the residual reduction and residual count for the user ordering and optimized kernels.
  for (int i=0; i< numberOfCalls; ++i) {
    for (int j=0; j< A.localNumberOfRows; ++j) x[j] = 0.0; // start x at all zeros
    double last_cummulative_time = opt_times[0];
    ierr = CG( geom, A, data, b, x, opt_maxIters, ref_tolerance, niters, normr, normr0, &opt_times[0], true);
    if (ierr) ++err_count; // count the number of errors in CG
    if (normr / normr0 > ref_tolerance) ++tolerance_failures; // the number of failures to reduce residual

    // pick the largest number of iterations to guarantee convergence
    if (niters > opt_iters) opt_iters = niters;

    double current_time = opt_times[0] - last_cummulative_time;
    if (current_time > opt_worst_time) opt_worst_time = current_time;

    totalNiters += niters;
  }
  if (rank == 0 && err_count) HPCG_fout << err_count << " error(s) in call(s) to optimized CG." << endl;
  if (tolerance_failures) {
    global_failure = 1;
    if (rank == 0)
      HPCG_fout << "Failed to reduce the residual " << tolerance_failures << " times." << endl;
  }

  ///////////////////////////////
  // Optimized CG Timing Phase //
  ///////////////////////////////

  // Here we finally run the benchmark phase
  // The variable total_runtime is the target benchmark execution time in seconds
  // This value should be set to 60*60*5 for official runs

  double total_runtime = 60.0; // run for at least one minute when in exploratory mode
  //double total_runtime = 60.0*60.0*5.0; // Run for 5 hours for official runs
  int numberOfCgSets = int(total_runtime / opt_worst_time);
  if (numberOfCgSets < 1) numberOfCgSets = 1; // run CG at least once

  /* This is the timed run for a specified amount of time. */

  totalNiters = 0;
  TestNormsData testnorms_data;
  testnorms_data.samples = numberOfCgSets;
  testnorms_data.values = new double[numberOfCgSets];

  for (int i=0; i< numberOfCgSets; ++i) {
    for (int j=0; j< A.localNumberOfRows; ++j) x[j] = 0.0; // Zero out x
    ierr = CG( geom, A, data, b, x, maxIters, tolerance, niters, normr, normr0, &times[0], true);
    if (ierr) HPCG_fout << "Error in call to CG: " << ierr << ".\n" << endl;
    if (rank==0) HPCG_fout << "Call [" << i << "] Scaled Residual [" << normr/normr0 << "]" << endl;
    testnorms_data.values[i] = normr/normr0; // Record scaled residual from this run
    totalNiters += niters;
  }

  // Compute difference between known exact solution and computed solution
  // All processors are needed here.
#ifdef HPCG_DEBUG
  double residual = 0;
  ierr = ComputeResidual(A.localNumberOfRows, x, xexact, &residual);
  if (ierr) HPCG_fout << "Error in call to compute_residual: " << ierr << ".\n" << endl;
  if (rank==0) HPCG_fout << "Difference between computed and exact  = " << residual << ".\n" << endl;
#endif

  // Test Norm Results
  ierr = TestNorms(&testnorms_data);

  ////////////////////
  // Report Results //
  ////////////////////

  // Report results to YAML file
  ReportResults(geom, A, numberOfCgSets, totalNiters, &times[0], &testcg_data, &testsymmetry_data, &testnorms_data, global_failure);

  // Clean up
  DeleteMatrix(A);
  DeleteCGData(data);
  delete [] testnorms_data.values;
  delete [] x;
  delete [] b;
  delete [] xexact;
  delete [] x_overlap;
  delete [] b_computed;



  HPCG_Finalize();

  // Finish up
#ifndef HPCG_NOMPI
  MPI_Finalize();
#endif
  return 0 ;
}
