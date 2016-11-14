//                       MFEM Example 9 - Parallel Version
//
// Compile with: make ex9p
//
// Sample runs:
//    mpirun -np 4 ex9p -m ../data/periodic-segment.mesh -p 0 -dt 0.005
//    mpirun -np 4 ex9p -m ../data/periodic-square.mesh -p 0 -dt 0.01
//    mpirun -np 4 ex9p -m ../data/periodic-hexagon.mesh -p 0 -dt 0.01
//    mpirun -np 4 ex9p -m ../data/periodic-square.mesh -p 1 -dt 0.005 -tf 9
//    mpirun -np 4 ex9p -m ../data/periodic-hexagon.mesh -p 1 -dt 0.005 -tf 9
//    mpirun -np 4 ex9p -m ../data/amr-quad.mesh -p 1 -rp 1 -dt 0.002 -tf 9
//    mpirun -np 4 ex9p -m ../data/star-q3.mesh -p 1 -rp 1 -dt 0.004 -tf 9
//    mpirun -np 4 ex9p -m ../data/disc-nurbs.mesh -p 1 -rp 1 -dt 0.005 -tf 9
//    mpirun -np 4 ex9p -m ../data/disc-nurbs.mesh -p 2 -rp 1 -dt 0.005 -tf 9
//    mpirun -np 4 ex9p -m ../data/periodic-square.mesh -p 3 -rp 2 -dt 0.0025 -tf 9 -vs 20
//    mpirun -np 4 ex9p -m ../data/periodic-cube.mesh -p 0 -o 2 -rp 1 -dt 0.01 -tf 8
//    mpirun -np 4 ex9p -m ../data/periodic-cube.mesh --usepetsc
//                      --petscopts .petsc_rc_ex9p_expl
//    mpirun -np 4 ex9p -m ../data/periodic-cube.mesh --usepetsc
//                      --petscopts .petsc_rc_ex9p_impl -implicit
//
// Description:  This example code solves the time-dependent advection equation
//               du/dt + v.grad(u) = 0, where v is a given fluid velocity, and
//               u0(x)=u(0,x) is a given initial condition.
//
//               The example demonstrates the use of Discontinuous Galerkin (DG)
//               bilinear forms in MFEM (face integrators), the use of explicit
//               ODE time integrators, the definition of periodic boundary
//               conditions through periodic meshes, as well as the use of GLVis
//               for persistent visualization of a time-evolving solution. The
//               saving of time-dependent data files for external visualization
//               with VisIt (visit.llnl.gov) is also illustrated.
//               The example also demonstrates how to use PETSc ODE solvers and
//               customize them by command line (see .petsc_rc_ex9p_expl and
//               .petsc_rc_ex9p_impl).

#include "mfem.hpp"
#include <fstream>
#include <iostream>

using namespace std;
using namespace mfem;

// Choice for the problem setup. The fluid velocity, initial condition and
// inflow boundary condition are chosen based on this parameter.
int problem;

// Velocity coefficient
void velocity_function(const Vector &x, Vector &v);

// Initial condition
double u0_function(const Vector &x);

// Inflow boundary condition
double inflow_function(const Vector &x);

// Mesh bounding box
Vector bb_min, bb_max;


/** A time-dependent operator for the ODE as F(u,du/dt,t) = G(u,t)
    The DG weak form of du/dt = -v.grad(u) is M du/dt = K u + b, where M and K are the mass
    and advection matrices, and b describes the flow on the boundary. This can
    be also written as a general ODE with the right-hand side only as
    du/dt = M^{-1} (K u + b).
    This class is used to evaluate the right-hand side and the left-hand side. */
class FE_Evolution : public TimeDependentOperator
{
private:
   HypreParMatrix &M, &K;
   const Vector &b;
   HypreSmoother M_prec;
   CGSolver M_solver;

   mutable Vector z;
#ifdef MFEM_USE_PETSC
   mutable PetscParMatrix* iJacobian;
   mutable PetscParMatrix* rJacobian;
#else
   mutable HypreParMatrix* iJacobian;
   mutable HypreParMatrix* rJacobian;
#endif

public:
   FE_Evolution(HypreParMatrix &_M, HypreParMatrix &_K, const Vector &_b, bool M_in_lhs);

   virtual void Mult(const Vector &x, Vector &y) const;
   virtual void Mult(const Vector &x, const Vector &xp, Vector &y) const;
#ifdef MFEM_USE_PETSC
   virtual Operator& GetGradient(const Vector &x) const;
   virtual Operator& GetGradient(const Vector &x, const Vector &xp, double shift) const;
#endif
   virtual ~FE_Evolution() { delete iJacobian; delete rJacobian; }
};


int main(int argc, char *argv[])
{
   // 1. Initialize MPI.
   int num_procs, myid;
   MPI_Init(&argc, &argv);
   MPI_Comm_size(MPI_COMM_WORLD, &num_procs);
   MPI_Comm_rank(MPI_COMM_WORLD, &myid);

   // 2. Parse command-line options.
   problem = 0;
   const char *mesh_file = "../data/periodic-hexagon.mesh";
   int ser_ref_levels = 2;
   int par_ref_levels = 0;
   int order = 3;
   int ode_solver_type = 4;
   double t_final = 10.0;
   double dt = 0.01;
   bool visualization = true;
   bool visit = false;
   int vis_steps = 5;
   bool use_petsc = false;
   bool implicit = false;
   bool use_step = true;
#ifdef MFEM_USE_PETSC
   const char *petscrc_file = "";
#endif

   int precision = 8;
   cout.precision(precision);

   OptionsParser args(argc, argv);
   args.AddOption(&mesh_file, "-m", "--mesh",
                  "Mesh file to use.");
   args.AddOption(&problem, "-p", "--problem",
                  "Problem setup to use. See options in velocity_function().");
   args.AddOption(&ser_ref_levels, "-rs", "--refine-serial",
                  "Number of times to refine the mesh uniformly in serial.");
   args.AddOption(&par_ref_levels, "-rp", "--refine-parallel",
                  "Number of times to refine the mesh uniformly in parallel.");
   args.AddOption(&order, "-o", "--order",
                  "Order (degree) of the finite elements.");
   args.AddOption(&ode_solver_type, "-s", "--ode-solver",
                  "ODE solver: 1 - Forward Euler, 2 - RK2 SSP, 3 - RK3 SSP,"
                  " 4 - RK4, 6 - RK6.");
   args.AddOption(&t_final, "-tf", "--t-final",
                  "Final time; start time is 0.");
   args.AddOption(&dt, "-dt", "--time-step",
                  "Time step.");
   args.AddOption(&visualization, "-vis", "--visualization", "-no-vis",
                  "--no-visualization",
                  "Enable or disable GLVis visualization.");
   args.AddOption(&visit, "-visit", "--visit-datafiles", "-no-visit",
                  "--no-visit-datafiles",
                  "Save data files for VisIt (visit.llnl.gov) visualization.");
   args.AddOption(&vis_steps, "-vs", "--visualization-steps",
                  "Visualize every n-th timestep.");
#ifdef MFEM_USE_PETSC
   args.AddOption(&use_petsc, "-usepetsc", "--usepetsc", "-no-petsc",
                  "--no-petsc",
                  "Use or not PETSc to solve the ODE system.");
   args.AddOption(&petscrc_file, "-petscopts", "--petscopts",
                  "PetscOptions file to use.");
   args.AddOption(&use_step, "-usestep", "--usestep", "-no-step",
                  "--no-step",
                  "Use the step or mult method to solve the ODE system.");
   args.AddOption(&implicit, "-implicit", "--implicit", "-no-implicit",
                  "--no-implicit",
                  "Use or not an implicit method in PETSc to solve the ODE system.");
#endif
   args.Parse();
   if (!args.Good())
   {
      if (myid == 0)
      {
         args.PrintUsage(cout);
      }
      MPI_Finalize();
      return 1;
   }
   if (myid == 0)
   {
      args.PrintOptions(cout);
   }
   // 2b. We initialize PETSc
#ifdef MFEM_USE_PETSC
   if (use_petsc) { PetscInitialize(NULL,NULL,petscrc_file,NULL); }
#endif

   // 3. Read the serial mesh from the given mesh file on all processors. We can
   //    handle geometrically periodic meshes in this code.
   Mesh *mesh = new Mesh(mesh_file, 1, 1);
   int dim = mesh->Dimension();

   // 4. Define the ODE solver used for time integration. Several explicit
   //    Runge-Kutta methods are available.
   ODESolver *ode_solver = NULL;
#ifdef MFEM_USE_PETSC
   PetscODESolver *pode_solver = NULL;
#endif
   if (!use_petsc)
   {
      switch (ode_solver_type)
      {
         case 1: ode_solver = new ForwardEulerSolver; break;
         case 2: ode_solver = new RK2Solver(1.0); break;
         case 3: ode_solver = new RK3SSPSolver; break;
         case 4: ode_solver = new RK4Solver; break;
         case 6: ode_solver = new RK6Solver; break;
         default:
            if (myid == 0)
            {
               cout << "Unknown ODE solver type: " << ode_solver_type << '\n';
            }
            MPI_Finalize();
            return 3;
      }
   }
#ifdef MFEM_USE_PETSC
   else
   {
      // When using PETSc, we just create the ODE solver. We use command line
      // customization to select a specific solver.
      pode_solver = new PetscODESolver(MPI_COMM_WORLD);
   }
#endif

   // 5. Refine the mesh in serial to increase the resolution. In this example
   //    we do 'ser_ref_levels' of uniform refinement, where 'ser_ref_levels' is
   //    a command-line parameter. If the mesh is of NURBS type, we convert it
   //    to a (piecewise-polynomial) high-order mesh.
   for (int lev = 0; lev < ser_ref_levels; lev++)
   {
      mesh->UniformRefinement();
   }
   if (mesh->NURBSext)
   {
      mesh->SetCurvature(max(order, 1));
   }
   mesh->GetBoundingBox(bb_min, bb_max, max(order, 1));

   // 6. Define the parallel mesh by a partitioning of the serial mesh. Refine
   //    this mesh further in parallel to increase the resolution. Once the
   //    parallel mesh is defined, the serial mesh can be deleted.
   ParMesh *pmesh = new ParMesh(MPI_COMM_WORLD, *mesh);
   delete mesh;
   for (int lev = 0; lev < par_ref_levels; lev++)
   {
      pmesh->UniformRefinement();
   }

   // 7. Define the parallel discontinuous DG finite element space on the
   //    parallel refined mesh of the given polynomial order.
   DG_FECollection fec(order, dim);
   ParFiniteElementSpace *fes = new ParFiniteElementSpace(pmesh, &fec);

   HYPRE_Int global_vSize = fes->GlobalTrueVSize();
   if (myid == 0)
   {
      cout << "Number of unknowns: " << global_vSize << endl;
   }

   // 8. Set up and assemble the parallel bilinear and linear forms (and the
   //    parallel hypre matrices) corresponding to the DG discretization. The
   //    DGTraceIntegrator involves integrals over mesh interior faces.
   VectorFunctionCoefficient velocity(dim, velocity_function);
   FunctionCoefficient inflow(inflow_function);
   FunctionCoefficient u0(u0_function);

   ParBilinearForm *m = new ParBilinearForm(fes);
   m->AddDomainIntegrator(new MassIntegrator);
   ParBilinearForm *k = new ParBilinearForm(fes);
   k->AddDomainIntegrator(new ConvectionIntegrator(velocity, -1.0));
   k->AddInteriorFaceIntegrator(
      new TransposeIntegrator(new DGTraceIntegrator(velocity, 1.0, -0.5)));
   k->AddBdrFaceIntegrator(
      new TransposeIntegrator(new DGTraceIntegrator(velocity, 1.0, -0.5)));

   ParLinearForm *b = new ParLinearForm(fes);
   b->AddBdrFaceIntegrator(
      new BoundaryFlowIntegrator(inflow, velocity, -1.0, -0.5));

   m->Assemble();
   m->Finalize();
   int skip_zeros = 0;
   k->Assemble(skip_zeros);
   k->Finalize(skip_zeros);
   b->Assemble();

   HypreParMatrix *M = m->ParallelAssemble();
   HypreParMatrix *K = k->ParallelAssemble();
   HypreParVector *B = b->ParallelAssemble();

   // 9. Define the initial conditions, save the corresponding grid function to
   //    a file and (optionally) save data in the VisIt format and initialize
   //    GLVis visualization.
   ParGridFunction *u = new ParGridFunction(fes);
   u->ProjectCoefficient(u0);
   HypreParVector *U = u->GetTrueDofs();
   {
      ostringstream mesh_name, sol_name;
      mesh_name << "ex9-mesh." << setfill('0') << setw(6) << myid;
      sol_name << "ex9-init." << setfill('0') << setw(6) << myid;
      ofstream omesh(mesh_name.str().c_str());
      omesh.precision(precision);
      pmesh->Print(omesh);
      ofstream osol(sol_name.str().c_str());
      osol.precision(precision);
      u->Save(osol);
   }

   VisItDataCollection visit_dc("Example9-Parallel", pmesh);
   visit_dc.RegisterField("solution", u);
   if (visit)
   {
      visit_dc.SetCycle(0);
      visit_dc.SetTime(0.0);
      visit_dc.Save();
   }

   socketstream sout;
   if (visualization)
   {
      char vishost[] = "localhost";
      int  visport   = 19916;
      sout.open(vishost, visport);
      if (!sout)
      {
         if (myid == 0)
            cout << "Unable to connect to GLVis server at "
                 << vishost << ':' << visport << endl;
         visualization = false;
         if (myid == 0)
         {
            cout << "GLVis visualization disabled.\n";
         }
      }
      else
      {
         sout << "parallel " << num_procs << " " << myid << "\n";
         sout.precision(precision);
         sout << "solution\n" << *pmesh << *u;
         sout << "pause\n";
         sout << flush;
         if (myid == 0)
            cout << "GLVis visualization paused."
                 << " Press space (in the GLVis window) to resume it.\n";
      }
   }

   // 10. Define the time-dependent evolution operator describing the ODE
   FE_Evolution *adv = new FE_Evolution(*M, *K, *B, implicit);
#ifdef MFEM_USE_PETSC
   if (use_petsc)
   {
     pode_solver->Init(*adv);
   }
   else
#endif
   {
     ode_solver->Init(*adv);
   }
   // Explicitly perform time-integration (looping over the time iterations,
   // ti, with a time-step dt), or use the Mult method of the solver class.
   double t = 0.0;
   if (use_step) {
      for (int ti = 0; true; )
      {
         if (t >= t_final - dt/2)
         {
            break;
         }
#ifdef MFEM_USE_PETSC
         if (use_petsc)
         {
            pode_solver->Step(*U, t, dt);
         }
         else
#endif
         {
            ode_solver->Step(*U, t, dt);
         }
         ti++;

         if (ti % vis_steps == 0)
         {
            if (myid == 0)
            {
               cout << "time step: " << ti << ", time: " << t << endl;
            }

            // 11. Extract the parallel grid function corresponding to the finite
            //     element approximation U (the local solution on each processor).
            *u = *U;

            if (visualization)
            {
               sout << "parallel " << num_procs << " " << myid << "\n";
               sout << "solution\n" << *pmesh << *u << flush;
            }

            if (visit)
            {
               visit_dc.SetCycle(ti);
               visit_dc.SetTime(t);
               visit_dc.Save();
            }
         }
      }
   }
   else
   {
#ifdef MFEM_USE_PETSC
      pode_solver->Mult(*U,*U);
#endif
   }

   // 12. Save the final solution in parallel. This output can be viewed later
   //     using GLVis: "glvis -np <np> -m ex9-mesh -g ex9-final".
   {
      *u = *U;
      ostringstream sol_name;
      sol_name << "ex9-final." << setfill('0') << setw(6) << myid;
      ofstream osol(sol_name.str().c_str());
      osol.precision(precision);
      u->Save(osol);
   }

   // 13. Free the used memory.
   delete U;
   delete u;
   delete B;
   delete b;
   delete K;
   delete k;
   delete M;
   delete m;
   delete fes;
   delete pmesh;
   delete ode_solver;
   delete adv;
#ifdef MFEM_USE_PETSC
   delete pode_solver;
   if (use_petsc) { PetscFinalize(); }
#endif
   MPI_Finalize();
   return 0;
}


// Implementation of class FE_Evolution
FE_Evolution::FE_Evolution(HypreParMatrix &_M, HypreParMatrix &_K,
                           const Vector &_b,bool M_in_lhs)
   : TimeDependentOperator(_M.Height(),M_in_lhs),
     M(_M), K(_K), b(_b), M_solver(M.GetComm()), z(_M.Height()),
     iJacobian(NULL), rJacobian(NULL)
{
   if (!M_in_lhs)
   {
      M_prec.SetType(HypreSmoother::Jacobi);
      M_solver.SetPreconditioner(M_prec);
      M_solver.SetOperator(M);

      M_solver.iterative_mode = false;
      M_solver.SetRelTol(1e-9);
      M_solver.SetAbsTol(0.0);
      M_solver.SetMaxIter(100);
      M_solver.SetPrintLevel(0);
   }
}

// RHS evaluation
void FE_Evolution::Mult(const Vector &x, Vector &y) const
{
   if (!HasLHS())
   {
      // y = M^{-1} (K x + b)
      K.Mult(x, z);
      z += b;
      M_solver.Mult(z, y);
   }
   else
   {
      // y = K x + b
      K.Mult(x, y);
      y += b;
   }
}

// LHS evaluation
void FE_Evolution::Mult(const Vector &x, const Vector &xp, Vector &y) const
{
   if (HasLHS())
   {
      M.Mult(xp, y);
   }
   else
   {
      y = xp;
   }
}

// RHS Jacobian
#ifdef MFEM_USE_PETSC
Operator& FE_Evolution::GetGradient(const Vector &x) const
{
   delete rJacobian;
   if (HasLHS())
   {
      rJacobian = new PetscParMatrix(&K, false);
   }
   else
   {
      mfem_error("FE_Evolution::GetGradient(x): Capability not coded!");
   }
   return *rJacobian;
}
#endif

// LHS Jacobian, evaluated as shift*F_du/dt + F_u
#ifdef MFEM_USE_PETSC
Operator& FE_Evolution::GetGradient(const Vector &x, const Vector &xp, double shift) const
{
   delete iJacobian;
   if (HasLHS())
   {
      iJacobian = new PetscParMatrix(&M, false);
   }
   else
   {
      mfem_error("FE_Evolution::GetGradient(x,xp,shift): Capability not coded!");
   }
   *iJacobian *= shift;
   return *iJacobian;
}
#endif

// Velocity coefficient
void velocity_function(const Vector &x, Vector &v)
{
   int dim = x.Size();

   // map to the reference [-1,1] domain
   Vector X(dim);
   for (int i = 0; i < dim; i++)
   {
      double center = (bb_min[i] + bb_max[i]) * 0.5;
      X(i) = 2 * (x(i) - center) / (bb_max[i] - bb_min[i]);
   }

   switch (problem)
   {
      case 0:
      {
         // Translations in 1D, 2D, and 3D
         switch (dim)
         {
            case 1: v(0) = 1.0; break;
            case 2: v(0) = sqrt(2./3.); v(1) = sqrt(1./3.); break;
            case 3: v(0) = sqrt(3./6.); v(1) = sqrt(2./6.); v(2) = sqrt(1./6.);
               break;
         }
         break;
      }
      case 1:
      case 2:
      {
         // Clockwise rotation in 2D around the origin
         const double w = M_PI/2;
         switch (dim)
         {
            case 1: v(0) = 1.0; break;
            case 2: v(0) = w*X(1); v(1) = -w*X(0); break;
            case 3: v(0) = w*X(1); v(1) = -w*X(0); v(2) = 0.0; break;
         }
         break;
      }
      case 3:
      {
         // Clockwise twisting rotation in 2D around the origin
         const double w = M_PI/2;
         double d = max((X(0)+1.)*(1.-X(0)),0.) * max((X(1)+1.)*(1.-X(1)),0.);
         d = d*d;
         switch (dim)
         {
            case 1: v(0) = 1.0; break;
            case 2: v(0) = d*w*X(1); v(1) = -d*w*X(0); break;
            case 3: v(0) = d*w*X(1); v(1) = -d*w*X(0); v(2) = 0.0; break;
         }
         break;
      }
   }
}

// Initial condition
double u0_function(const Vector &x)
{
   int dim = x.Size();

   // map to the reference [-1,1] domain
   Vector X(dim);
   for (int i = 0; i < dim; i++)
   {
      double center = (bb_min[i] + bb_max[i]) * 0.5;
      X(i) = 2 * (x(i) - center) / (bb_max[i] - bb_min[i]);
   }

   switch (problem)
   {
      case 0:
      case 1:
      {
         switch (dim)
         {
            case 1:
               return exp(-40.*pow(X(0)-0.5,2));
            case 2:
            case 3:
            {
               double rx = 0.45, ry = 0.25, cx = 0., cy = -0.2, w = 10.;
               if (dim == 3)
               {
                  const double s = (1. + 0.25*cos(2*M_PI*X(2)));
                  rx *= s;
                  ry *= s;
               }
               return ( erfc(w*(X(0)-cx-rx))*erfc(-w*(X(0)-cx+rx)) *
                        erfc(w*(X(1)-cy-ry))*erfc(-w*(X(1)-cy+ry)) )/16;
            }
         }
      }
      case 2:
      {
         double x_ = X(0), y_ = X(1), rho, phi;
         rho = hypot(x_, y_);
         phi = atan2(y_, x_);
         return pow(sin(M_PI*rho),2)*sin(3*phi);
      }
      case 3:
      {
         const double f = M_PI;
         return sin(f*X(0))*sin(f*X(1));
      }
   }
   return 0.0;
}

// Inflow boundary condition (zero for the problems considered in this example)
double inflow_function(const Vector &x)
{
   switch (problem)
   {
      case 0:
      case 1:
      case 2:
      case 3: return 0.0;
   }
   return 0.0;
}
