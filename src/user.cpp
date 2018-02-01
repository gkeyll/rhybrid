/** This file is part of the RHybrid simulation.
 *
 *  Copyright 2018- Aalto University
 *  Copyright 2015- Finnish Meteorological Institute
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <cstdlib>
#include <iostream>
#include <iterator>
#include <istream>
#include <user.h>
#include <particle_list_skeleton.h>

#include "hybrid.h"
#include "hybrid_propagator.h"
#include "particle_definition.h"
#include "particle_species.h"
#include "particle_accumulator.h"
#include "particle_propagator_boris_buneman.h"
#include "particle_injector.h"
#include "particle_list_hybrid.h"
#include "operator_userdata.h"
#ifdef USE_RESISTIVITY
#include "resistivity.h"
#endif
#ifdef USE_B_INITIAL
#include "magnetic_field.h"
#endif

using namespace std;

bool propagate(Simulation& sim,SimulationClasses& simClasses,vector<ParticleListBase*>& particleLists) {
   bool rvalue = true;
   if(Hybrid::logInterval > 0) {
      if( (sim.timestep)%(Hybrid::logInterval) == 0.0) {
         if(writeLogs(sim,simClasses,particleLists) == false) { rvalue = false; }
      }
   }
#ifdef ION_SPECTRA_ALONG_ORBIT
   if(sim.t >= Hybrid::tStartSpectra && sim.t <= Hybrid::tEndSpectra && Hybrid::spectraFileLineCnt < Hybrid::maxRecordedSpectraParticles) {
      Hybrid::recordSpectra = true;
      Hybrid::spectraTimestepCnt++;
   }
   else { Hybrid::recordSpectra = false; }
#endif
   setupGetFields(sim,simClasses);
   // Propagate all particles:
   for(size_t p=0;p<particleLists.size();++p) { if(particleLists[p]->propagateBoundaryCellParticles() == false) { rvalue = false; }  }
   for(size_t p=0;p<particleLists.size();++p) { if(particleLists[p]->propagateInnerCellParticles() == false) { rvalue = false; } }
   for(size_t p=0;p<particleLists.size();++p) { if(particleLists[p]->clearAccumulationArrays() == false) { rvalue = false; } }
   for(size_t p=0;p<particleLists.size();++p) { if(particleLists[p]->waitParticleSends() == false) { rvalue = false; } }
   // Accumulate particle quantities to simulation mesh:
   for(size_t p=0;p<particleLists.size();++p) { if(particleLists[p]->accumulateBoundaryCells() == false) { rvalue = false; } }
   for(size_t p=0;p<particleLists.size();++p) { if(particleLists[p]->accumulateInnerCells() == false) { rvalue = false; } }
   // Apply boundary conditions:
   for(size_t p=0;p<particleLists.size();++p) { if(particleLists[p]->applyBoundaryConditions() == false) { rvalue = false; } }
   // Inject new particles:
   for(size_t p=0;p<particleLists.size();++p) {
      if(particleLists[p]->injectParticles() == false) { rvalue = false; } 
   }
#ifdef ION_SPECTRA_ALONG_ORBIT   
   if(Hybrid::recordSpectra == true) {
      if(Hybrid::spectraTimestepCnt >= Hybrid::writeIntervalTimesteps) {
	 bool ok = writeSpectraParticles(sim,simClasses);
	 Hybrid::spectraTimestepCnt = 0;
      }
   }
#endif
   // propagate magnetic field
   if(propagateB(sim,simClasses,particleLists) == false) { rvalue = false; }
   return rvalue;
}

bool userEarlyInitialization(Simulation& sim,SimulationClasses& simClasses,ConfigReader& cr,vector<ParticleListBase*>& particleLists) {
   Hybrid hybrid;
#if defined(USE_B_INITIAL) && defined(USE_B_CONSTANT)
#error                  "(HYBRID) ERROR: Cannot define USE_B_INITIAL and USE_B_CONSTANT in the same time"
   simClasses.logger << "(HYBRID) ERROR: Cannot define USE_B_INITIAL and USE_B_CONSTANT in the same time" << endl << write;
   return false;
#endif
   simClasses.logger
     << "(HYBRID): Compile information and Makefile options: " << endl
#ifdef COMPILEINFO
     << COMPILEINFO << endl
#endif
     << write;
   Hybrid::X_POS_EXISTS = (1 << simClasses.pargrid.calcNeighbourTypeID(+1,+0,+0));   
   Hybrid::X_NEG_EXISTS = (1 << simClasses.pargrid.calcNeighbourTypeID(-1,+0,+0));   
   Hybrid::Y_POS_EXISTS = (1 << simClasses.pargrid.calcNeighbourTypeID(+0,+1,+0));
   Hybrid::Y_NEG_EXISTS = (1 << simClasses.pargrid.calcNeighbourTypeID(+0,-1,+0));
   Hybrid::Z_POS_EXISTS = (1 << simClasses.pargrid.calcNeighbourTypeID(+0,+0,+1));
   Hybrid::Z_NEG_EXISTS = (1 << simClasses.pargrid.calcNeighbourTypeID(+0,+0,-1));
   return true;
}

// read an ascii file in a two dimensional vector of reals
vector< vector<Real> > readRealsFromFile(string fn) {
   vector< vector<Real> > result;
   string tmpstr;
   ifstream in(fn);
   while( getline(in,tmpstr) ) {
      istringstream buff(tmpstr);
      vector<Real> line((istream_iterator<Real>(buff)),istream_iterator<Real>());
      result.push_back(line);
   }
   in.close();
   return result;
}

// check orbit read from a file
bool checkOrbit(vector< vector<Real> > xyz) {
   if(xyz.size() <= 0) { return false; }
   for (unsigned int i = 0; i < xyz.size(); ++i) {
      if(xyz[i].size() != 3) { return false; }
   }
   return true;
}

// broadcast 2d vector from master to all PEs
bool MPI_BcastFromMaster2DVector(Simulation& sim,vector< vector<Real> >& d) {
   int NN[2];
   if(sim.mpiRank == sim.MASTER_RANK) {
      NN[0]=d.size();
      if(NN[0] > 0) { NN[1]=d[0].size(); }
      else { return false; }
      // check that all vector rows have same number of columns
      for(unsigned int i = 0;i<NN[0];i++) {
         if(d[i].size() != NN[1]) { return false; }
      }
   }
   // distribute orbit coordinates read by master to all processes
   MPI_Bcast(NN,2,MPI_Type<int>(),sim.MASTER_RANK,sim.comm);
   Real* buff = new Real[NN[1]];
   for(int i=0;i<NN[0];i++) {
      if(sim.mpiRank == sim.MASTER_RANK) {
	 for(int j=0;j<NN[1];j++) {
	    buff[j] = d[i][j];
	 }
      }
      MPI_Bcast(buff,NN[1],MPI_Type<Real>(),sim.MASTER_RANK,sim.comm);
      if(sim.mpiRank != sim.MASTER_RANK) {
	 d.push_back(vector<Real>());
	 for(int j=0;j<NN[1];j++)  {
	    d[i].push_back(buff[j]);
	 }
      }
   }
   delete buff;
   return true;
}

// gaussian distribution
Real getGaussianDistr(Real x,Real sigma) {
   if(sigma > 0) {
      return exp( -0.5*sqr(x/sigma) );
   }
   else {
      return -1.0;
   }
}

bool userLateInitialization(Simulation& sim,SimulationClasses& simClasses,ConfigReader& cr,const ObjectFactories& objectFactories,
			    vector<ParticleListBase*>& particleLists) {

   // Figure out dx. If this process has at least one cell, send its 
   // size to master. The master will then broadcast the cellsize to 
   // all processes.
   Real cellSize[3];
   for (int i=0; i<3; ++i) { cellSize[i] = -1.0; }
   if (simClasses.pargrid.getNumberOfLocalCells() > 0) {
      getBlockCellSize(simClasses,sim,0,cellSize);
   }
     {
	Real recvBuffer[3];
	MPI_Reduce(cellSize,recvBuffer,3,MPI_Type<Real>(),MPI_MAX,sim.MASTER_RANK,sim.comm);
	for (int i=0; i<3; ++i) { cellSize[i] = recvBuffer[i]; }
	MPI_Bcast(cellSize,3,MPI_Type<Real>(),sim.MASTER_RANK,sim.comm);
     }
   if( (cellSize[0] != cellSize[1]) || (cellSize[0] != cellSize[2]) || (cellSize[1] != cellSize[2]) ) {
      simClasses.logger << "(HYBRID) ERROR: Only cube shaped cells allowed (dx = " << cellSize[0]/1e3 << " km, dy = " << cellSize[1]/1e3 << " km, dz = " << cellSize[2]/1e3 << " km)" << endl << write;
      exit(1);
      return false;
   }
   Hybrid::dx=cellSize[0];
   Hybrid::dV=cube(Hybrid::dx);
   const Real defaultValue = 0.0;
   string outputParams = "";
#if defined(USE_B_INITIAL) || defined(USE_B_CONSTANT)
   string magneticFieldProfileName = "";
#endif
#ifdef USE_RESISTIVITY
   string resistivityProfileName = "";
#endif
   cr.add("Hybrid.log_interval","Log interval in units of timestep [-] (int)",0);
   cr.add("Hybrid.output_parameters","Parameters to write in output files (string)","");
   cr.add("Hybrid.R_object","Radius of simulated object [m] (float)",defaultValue);
   cr.add("Hybrid.R_fieldObstacle","Radius of inner field boundary [m] (float)",defaultValue);
   cr.add("Hybrid.R_particleObstacle","Radius of inner particle boundary [m] (float)",defaultValue);
#ifdef USE_XMIN_BOUNDARY
   cr.add("Hybrid.xmin","Back X boundary [m] (float)",defaultValue);
#endif
   cr.add("Hybrid.M_object","Mass of simulated object [kg] (float)",defaultValue);
   cr.add("Hybrid.maxUe","Maximum magnitude of electron velocity [m/s] (float)",defaultValue);
   cr.add("Hybrid.maxVi","Maximum magnitude of ion velocity [m/s] (float)",defaultValue);
   cr.add("Hybrid.minRhoQi","Minimum value of ion charge density [C/m^3] (float)",defaultValue);
#ifdef USE_ECUT
   cr.add("Hybrid.Ecut","Maximum value of node electric field [V/m] (float)",defaultValue);
#endif
   cr.add("Hybrid.hall_term","Use Hall term in the electric field [-] (bool)",true);
   cr.add("Hybrid.Efilter","E filtering number [-] (int)",static_cast<int>(0));
   cr.add("Hybrid.EfilterNodeGaussSigma","E filtering number [dx] (float)",defaultValue);
#ifdef USE_RESISTIVITY
   cr.add("Resistivity.profile_name","Resistivity profile name [-] (string)","");
   cr.add("Resistivity.etaC","Dimensionless resistivity constant [-] (float)",defaultValue);
   cr.add("Resistivity.R","Radius of the super conducting sphere [m] (float)",defaultValue);
#endif
   cr.add("IMF.Bx","IMF Bx [T] (float)",defaultValue);
   cr.add("IMF.By","IMF By [T] (float)",defaultValue);
   cr.add("IMF.Bz","IMF Bz [T] (float)",defaultValue);
#if defined(USE_B_INITIAL) || defined(USE_B_CONSTANT)
   cr.add("IntrinsicB.profile_name","Magnetic field profile name [-] (string)","");
   cr.add("IntrinsicB.laminarR","Laminar flow around sphere R [m] (float)",defaultValue);
   cr.add("IntrinsicB.coeffDipole","Dipole coefficient [-] (float)",defaultValue);
   cr.add("IntrinsicB.coeffQuadrupole","Quadrupole coefficient [-] (float)",defaultValue);
   cr.add("IntrinsicB.dipoleSurfaceB","Dipole surface strength [T] (float)",defaultValue);
   cr.add("IntrinsicB.dipoleSurfaceR","Dipole surface radius [m] (float)",defaultValue);
   cr.add("IntrinsicB.minimumR","Minimum surface radius [m] (float)",defaultValue);
   cr.add("IntrinsicB.x","X coordinate of the origin [m] (float)",defaultValue);
   cr.add("IntrinsicB.y","Y coordinate of the origin [m] (float)",defaultValue);
   cr.add("IntrinsicB.z","Z coordinate of the origin [m] (float)",defaultValue);
   cr.add("IntrinsicB.theta","theta angle of the field [deg] (float)",defaultValue);
   cr.add("IntrinsicB.phi","phi angle of the field [deg] (float)",defaultValue);
#endif
   cr.parse();
   cr.get("Hybrid.log_interval",Hybrid::logInterval);
   cr.get("Hybrid.output_parameters",outputParams);
   cr.get("Hybrid.R_object",Hybrid::R_object);
   cr.get("Hybrid.R_fieldObstacle",Hybrid::R2_fieldObstacle);
   cr.get("Hybrid.R_particleObstacle",Hybrid::R2_particleObstacle);
#ifdef USE_XMIN_BOUNDARY
   cr.get("Hybrid.xmin",Hybrid::xMinBoundary);
#endif
   cr.get("Hybrid.M_object",Hybrid::M_object);
   cr.get("Hybrid.maxUe",Hybrid::maxUe2);
   cr.get("Hybrid.maxVi",Hybrid::maxVi2);
   cr.get("Hybrid.minRhoQi",Hybrid::minRhoQi);
#ifdef USE_ECUT
   cr.get("Hybrid.Ecut",Hybrid::Ecut2);
   Hybrid::Ecut2 = sqr(Hybrid::Ecut2);
#endif
   cr.get("Hybrid.hall_term",Hybrid::useHallElectricField);
   cr.get("Hybrid.Efilter",Hybrid::Efilter);
   cr.get("Hybrid.EfilterNodeGaussSigma",Hybrid::EfilterNodeGaussSigma);
#ifdef USE_RESISTIVITY
   cr.get("Resistivity.profile_name",resistivityProfileName);
   cr.get("Resistivity.etaC",Hybrid::resistivityEtaC);
   cr.get("Resistivity.R",Hybrid::resistivityR2);
   Hybrid::resistivityR2 = sqr(Hybrid::resistivityR2);
   Hybrid::resistivityGridUnit = constants::PERMEABILITY*sqr(Hybrid::dx)/sim.dt;
   Hybrid::resistivityEta = Hybrid::resistivityEtaC*Hybrid::resistivityGridUnit;
   if(setResistivityProfile(resistivityProfileName) == false) {
      simClasses.logger << "(HYBRID) ERROR: Given profile profile not found (" << resistivityProfileName << ")" << endl << write;
      exit(1);
   }
#endif
   cr.get("IMF.Bx",Hybrid::IMFBx);
   cr.get("IMF.By",Hybrid::IMFBy);
   cr.get("IMF.Bz",Hybrid::IMFBz);
#if defined(USE_B_INITIAL) || defined(USE_B_CONSTANT)
   cr.get("IntrinsicB.profile_name",magneticFieldProfileName);
   cr.get("IntrinsicB.laminarR",Hybrid::laminarR2);
   cr.get("IntrinsicB.coeffDipole",Hybrid::coeffDip);
   cr.get("IntrinsicB.coeffQuadrupole",Hybrid::coeffQuad);
   cr.get("IntrinsicB.dipoleSurfaceB",Hybrid::dipSurfB);
   cr.get("IntrinsicB.dipoleSurfaceR",Hybrid::dipSurfR);
   cr.get("IntrinsicB.minimumR",Hybrid::dipMinR2);
   cr.get("IntrinsicB.x",Hybrid::xDip);
   cr.get("IntrinsicB.y",Hybrid::yDip);
   cr.get("IntrinsicB.z",Hybrid::zDip);
   cr.get("IntrinsicB.theta",Hybrid::thetaDip);
   cr.get("IntrinsicB.phi",Hybrid::phiDip);
   Hybrid::laminarR3 = cube(Hybrid::laminarR2);
   Hybrid::laminarR2 =  sqr(Hybrid::laminarR2);
   Hybrid::dipMomCoeff = 3.0*Hybrid::dipSurfB*cube(Hybrid::dipSurfR);
   Hybrid::dipMinR2 = sqr(Hybrid::dipMinR2);
   if(setMagneticFieldProfile(magneticFieldProfileName) == false) {
      simClasses.logger << "(HYBRID) ERROR: Given magnetic field profile not found (" << magneticFieldProfileName << ")" << endl << write;
      exit(1);
   }
#endif
   if(Hybrid::logInterval <= 0) { Hybrid::logInterval = 0; }
   // set parameters written in vlsv files
   Hybrid::outputCellParams = {
      {"faceB",false},
      {"faceJ",false},
      {"cellRhoQi",false},
      {"cellB",false},
      {"cellJ",false},
      {"cellUe",false},
      {"cellJi",false},
      {"nodeE",false},
      {"nodeB",false},
      {"nodeJ",false},
      {"nodeUe",false},
      {"nodeJi",false},
#ifdef USE_RESISTIVITY
      {"nodeEta",false},
#endif
      {"counterCellMaxUe",false},
      {"counterCellMaxVi",false},
      {"counterCellMinRhoQi",false},
#ifdef USE_ECUT
      {"counterNodeEcut",false},
#endif
      {"prod_rate_iono",false},
      {"prod_rate_exo",false},
      {"cellBAverage",false},
      {"n_ave",false},
      {"v_ave",false},
      {"cellDivB",false},
      {"cellNPles",false},
      {"cellB0",false},
      {"n",false},
      {"T",false},
      {"v",false},
      {"n_tot",false},
      {"T_tot",false},
      {"v_tot",false}
   };
   // process output parameter selection
     {
        istringstream iss(outputParams);
        while(iss) {
           string p;
           iss >> p;
           if(p.find_first_not_of(' ') != string::npos) {
              if(Hybrid::outputCellParams.count(p) > 0) {
                 Hybrid::outputCellParams[p] = true;
              }
           }
        }
     }
   simClasses.logger << "(HYBRID) Available output parameters: ";
   for(auto p: Hybrid::outputCellParams) { simClasses.logger << p.first << " "; }
   simClasses.logger << endl;
   simClasses.logger << "(HYBRID) Selected output parameters: ";
   for(auto p: Hybrid::outputCellParams) {
      if(p.second == true) { simClasses.logger << p.first << " "; }
   }
   simClasses.logger << endl;
#ifndef WRITE_POPULATION_AVERAGES
   if(Hybrid::outputCellParams["n_ave"] == true || Hybrid::outputCellParams["v_ave"] == true || Hybrid::outputCellParams["cellBAverage"] == true) {
      simClasses.logger << "(HYBRID) WARNING: Average output parameters selected but WRITE_POPULATION_AVERAGES not defined in Makefile" << endl;
   }
#endif
#ifndef USE_B_CONSTANT
   if(Hybrid::outputCellParams["cellB0"] == true) {
      simClasses.logger << "(HYBRID) WARNING: cellB0 output parameter selected but USE_B_CONSTANT not defined in Makefile" << endl;
   }
#endif
   
   if(Hybrid::R_object < 0) { Hybrid::R_object = 1.0; }
   if(Hybrid::R2_fieldObstacle > 0) { Hybrid::R2_fieldObstacle = sqr(Hybrid::R2_fieldObstacle); }
   else { Hybrid::R2_fieldObstacle = -1; }
   if(Hybrid::R2_particleObstacle > 0) { Hybrid::R2_particleObstacle = sqr(Hybrid::R2_particleObstacle); }
   else { Hybrid::R2_particleObstacle = -1; }   
   Hybrid::maxUe2 = sqr(Hybrid::maxUe2);
   if(Hybrid::maxVi2 > Hybrid::dx/sim.dt) {
      simClasses.logger << "(HYBRID) WARNING: maxVi = " << Hybrid::maxVi2/1e3 << " km/s > dx/dt, setting maxVi = 0.9*dx/dt" << endl << write;
      Hybrid::maxVi2 = 0.9*Hybrid::dx/sim.dt;
   }
   Hybrid::maxVi2 = sqr(Hybrid::maxVi2);
   if(Hybrid::Efilter < 0) { Hybrid::Efilter = 0; }
   if(Hybrid::EfilterNodeGaussSigma <= 0) { Hybrid::EfilterNodeGaussSigma = 0; }
   else {
      // determined gaussian smoothing coefficients
      const Real C1 = getGaussianDistr(0.0,Hybrid::EfilterNodeGaussSigma); // 1 node itself to be filtered
      const Real C2 = getGaussianDistr(1.0,Hybrid::EfilterNodeGaussSigma); // 6 direct neighbors (at dx)
      const Real C3 = getGaussianDistr(sqrt(2.0),Hybrid::EfilterNodeGaussSigma); // 12 near diagonal neighbors (at sqrt(2)*dx)
      const Real C4 = getGaussianDistr(sqrt(3.0),Hybrid::EfilterNodeGaussSigma); // 8 far diagonal neighbors (at sqrt(3)*dx)
      const Real Csum = 1.0*C1 + 6.0*C2 + 12.0*C3 + 8.0*C4; // normalization such that sum_i C_i = 1 over all 27 nodes
      Hybrid::EfilterNodeGaussCoeffs[0] = C1/Csum;
      Hybrid::EfilterNodeGaussCoeffs[1] = C2/Csum;
      Hybrid::EfilterNodeGaussCoeffs[2] = C3/Csum;
      Hybrid::EfilterNodeGaussCoeffs[3] = C4/Csum;
   }
   simClasses.logger
     << "(HYBRID): Simulation parameters" << endl
     << "R_object  = " << Hybrid::R_object/1e3 << " km" << endl
     << "R_fieldObstacle = ";
   if(Hybrid::R2_fieldObstacle > 0) {
      simClasses.logger
	<< sqrt(Hybrid::R2_fieldObstacle)/1e3 << " km = "
	<< sqrt(Hybrid::R2_fieldObstacle)/Hybrid::R_object << " R_object = "
	<< (sqrt(Hybrid::R2_fieldObstacle) - Hybrid::R_object)/1e3 << " km + R_object" << endl;
   }
   else { simClasses.logger << Hybrid::R2_fieldObstacle << "" << endl; }
   simClasses.logger << "R_particleObstacle = ";
   if(Hybrid::R2_particleObstacle > 0) {
      simClasses.logger
	<< sqrt(Hybrid::R2_particleObstacle)/1e3 << " km = "
	<< sqrt(Hybrid::R2_particleObstacle)/Hybrid::R_object << " R_object = "
	<< (sqrt(Hybrid::R2_particleObstacle) - Hybrid::R_object)/1e3 << " km + R_object" << endl;
   }
   else { simClasses.logger << Hybrid::R2_particleObstacle << "" << endl; }   
   simClasses.logger
     << "M_object  = " << Hybrid::M_object     << " kg" << endl
     << "maxUe = " << sqrt(Hybrid::maxUe2)/1e3 << " km/s" << endl
     << "maxVi = " << sqrt(Hybrid::maxVi2)/1e3 << " km/s" << endl
     << "minRhoQi = " << Hybrid::minRhoQi << " C/m^3 = " << Hybrid::minRhoQi/(1e6*constants::CHARGE_ELEMENTARY) << " e/cm^3 " << endl
#ifdef USE_ECUT
     << "Ecut  = " << sqrt(Hybrid::Ecut2) << " V/m" << endl
#endif
     << "Hall term = " << Hybrid::useHallElectricField << endl
     << "dV = " << Hybrid::dV << " m^3" << endl
     << "IMF Bx  = " << Hybrid::IMFBx/1e-9 << " nT" << endl
     << "IMF By  = " << Hybrid::IMFBy/1e-9 << " nT" << endl
     << "IMF Bz  = " << Hybrid::IMFBz/1e-9 << " nT" << endl
     << "IMF |B| = " << sqrt( sqr(Hybrid::IMFBx) + sqr(Hybrid::IMFBy) + sqr(Hybrid::IMFBz) )/1e-9 << " nT" << endl
     << "IMF Bperp = sqrt(By^2 + Bz^2)     = " << sqrt( sqr(Hybrid::IMFBy) + sqr(Hybrid::IMFBz) )/1e-9 << " nT" << endl
     << "IMF Cone angle  = atan2(Bperp,Bx) = " << atan2(sqrt( sqr(Hybrid::IMFBy) + sqr(Hybrid::IMFBz) ),Hybrid::IMFBx)*180.0/M_PI << " deg" << endl
     << "IMF Clock angle = atan2(By,Bz)    = " << atan2(Hybrid::IMFBy,Hybrid::IMFBz)*180.0/M_PI << " deg" << endl
     << endl;
   simClasses.logger
     << "(FILTERING)" << endl
     << "Number of E intpol smoothings = " << Hybrid::Efilter << " (node2cell2node interpolation technique)" << endl
     << "Sigma of E gaussian smoothing = " << Hybrid::EfilterNodeGaussSigma << " dx (gaussian average technique)" << endl;
   if(Hybrid::EfilterNodeGaussSigma > 0) {
      simClasses.logger
        //<< "Number of E gaussian smoothings = " << Hybrid::EfilterNodeGaussN << " (gaussian average technique)" << endl;
        << "Kernel coefficients: " << endl
        << "C1 = " << Hybrid::EfilterNodeGaussCoeffs[0] << " (d = 0)" << endl
        << "C2 = " << Hybrid::EfilterNodeGaussCoeffs[1] << " (d = 1dx)" << endl
        << "C3 = " << Hybrid::EfilterNodeGaussCoeffs[2] << " (d = sqrt(2)dx)" << endl
        << "C4 = " << Hybrid::EfilterNodeGaussCoeffs[3] << " (d = sqrt(3)dx)" << endl;
   }
   simClasses.logger << endl;
#ifdef USE_RESISTIVITY
   simClasses.logger
     << "(RESISTIVITY)" << endl
     << "Resistivity profile = " << resistivityProfileName << endl
     << "eta = " << Hybrid::resistivityEtaC << " mu_0*dx^2/dt = "
     << Hybrid::resistivityEta << " Ohm m" << endl
     << "R = " << sqrt(Hybrid::resistivityR2)/1e3 << " km = "
     << sqrt(Hybrid::resistivityR2)/Hybrid::R_object << " R_object = "
     << (sqrt(Hybrid::resistivityR2) - Hybrid::R_object)/1e3 << " km + R_object" << endl << endl;
#endif
#if defined(USE_B_INITIAL) || defined(USE_B_CONSTANT)
   simClasses.logger
     << "(INTRINSIC MAGNETIC FIELD)" << endl
#ifdef USE_B_INITIAL
     << "Using initial field" << endl
#endif
#ifdef USE_B_CONSTANT
     << "Using constant field" << endl
#endif
     << "Magnetic field profile = " << magneticFieldProfileName << endl
     << "Laminar flow around sphere R = " << sqrt(Hybrid::laminarR2)/1e3 << " km" << endl
     << "Dipole coefficient = " << Hybrid::coeffDip << endl
     << "Quadrupole coefficient = " << Hybrid::coeffQuad << endl
     << "Dipole surface B = " << Hybrid::dipSurfB/1e-9 << " nT" << endl
     << "Dipole surface R = " << Hybrid::dipSurfR/1e3 << " km" << endl
     << "Minimum R = " << sqrt(Hybrid::dipMinR2)/1e3 << " km" << endl 
     << "x = " << Hybrid::xDip/1e3 << " km" << endl
     << "y = " << Hybrid::yDip/1e3 << " km" << endl
     << "z = " << Hybrid::zDip/1e3 << " km" << endl
     << "theta = " << Hybrid::thetaDip << " deg" << endl
     << "phi   = " << Hybrid::phiDip << " deg" << endl
     << endl;   
#endif
   
   // read particle populations: uniform
   vector<string> uniformPopulations;
   cr.addComposed("Hybrid.particle.population.uniform","Names of uniform particle populations (string)");
   cr.parse();
   cr.get("Hybrid.particle.population.uniform",uniformPopulations);
   // erase empty entries
   bool erased = false;
   do {
      erased = false;
      for (vector<string>::iterator it=uniformPopulations.begin(); it!=uniformPopulations.end(); ++it) {
	 if ((*it).size() == 0) {
	    uniformPopulations.erase(it);
	    erased = true;
	    break;
	 }
      }
   } while (erased == true);
   // read particle populations: solar wind
   vector<string> solarwindPopulations;
   cr.addComposed("Hybrid.particle.population.solarwind","Names of solar wind particle populations (string)");
   cr.parse();
   cr.get("Hybrid.particle.population.solarwind",solarwindPopulations);
   // erase empty entries
   erased = false;
   do {
      erased = false;
      for (vector<string>::iterator it=solarwindPopulations.begin(); it!=solarwindPopulations.end(); ++it) {
	 if ((*it).size() == 0) {
	    solarwindPopulations.erase(it);
	    erased = true;
	    break;
	 }
      }
   } while (erased == true);
   // read particle populations: ionosphere
   vector<string> ionospherePopulations;
   cr.addComposed("Hybrid.particle.population.ionosphere","Names of ionopsheric particle populations (string)");
   cr.parse();
   cr.get("Hybrid.particle.population.ionosphere",ionospherePopulations);
   // erase empty entries
   erased = false;
   do {
      erased = false;
      for (vector<string>::iterator it=ionospherePopulations.begin(); it!=ionospherePopulations.end(); ++it) {
	 if ((*it).size() == 0) {
	    ionospherePopulations.erase(it);
	    erased = true;
	    break;
	 }
      }
   } while (erased == true);
   // read particle populations: exosphere
   vector<string> exospherePopulations;
   cr.addComposed("Hybrid.particle.population.exosphere","Names of exosphere particle populations (string)");
   cr.parse();
   cr.get("Hybrid.particle.population.exosphere",exospherePopulations);
   // erase empty entries
   erased = false;
   do {
      erased = false;
      for (vector<string>::iterator it=exospherePopulations.begin(); it!=exospherePopulations.end(); ++it) {
	 if ((*it).size() == 0) {
	    exospherePopulations.erase(it);
	    erased = true;
	    break;
	 }
      }
   } while (erased == true);

   Hybrid::N_populations = static_cast<unsigned int>( uniformPopulations.size() + solarwindPopulations.size() + ionospherePopulations.size() + exospherePopulations.size() );
   Hybrid::N_ionospherePopulations = static_cast<unsigned int>( ionospherePopulations.size() );
   Hybrid::N_exospherePopulations = static_cast<unsigned int>( exospherePopulations.size() );

   Hybrid::dataFaceBID               = simClasses.pargrid.invalidDataID();
   Hybrid::dataFaceJID               = simClasses.pargrid.invalidDataID();
   Hybrid::dataCellRhoQiID           = simClasses.pargrid.invalidDataID();
   Hybrid::dataCellBID               = simClasses.pargrid.invalidDataID();
   Hybrid::dataCellJID               = simClasses.pargrid.invalidDataID();
   Hybrid::dataCellUeID              = simClasses.pargrid.invalidDataID();
   Hybrid::dataCellJiID              = simClasses.pargrid.invalidDataID();
   Hybrid::dataCellIonosphereID      = simClasses.pargrid.invalidDataID();
   Hybrid::dataCellExosphereID       = simClasses.pargrid.invalidDataID();
   Hybrid::dataNodeRhoQiID           = simClasses.pargrid.invalidDataID();
   Hybrid::dataNodeEID               = simClasses.pargrid.invalidDataID();
   Hybrid::dataNodeBID               = simClasses.pargrid.invalidDataID();
   Hybrid::dataNodeJID               = simClasses.pargrid.invalidDataID();
   Hybrid::dataNodeUeID              = simClasses.pargrid.invalidDataID();
   Hybrid::dataNodeJiID              = simClasses.pargrid.invalidDataID();
#ifdef USE_RESISTIVITY
   Hybrid::dataNodeEtaID             = simClasses.pargrid.invalidDataID();
#endif
   Hybrid::dataCounterCellMaxUeID    = simClasses.pargrid.invalidDataID();
   Hybrid::dataCounterCellMaxViID    = simClasses.pargrid.invalidDataID();
   Hybrid::dataCounterCellMinRhoQiID = simClasses.pargrid.invalidDataID();
#ifdef USE_ECUT
   Hybrid::dataCounterNodeEcutID     = simClasses.pargrid.invalidDataID();
#endif
   Hybrid::dataInnerFlagFieldID      = simClasses.pargrid.invalidDataID();
   Hybrid::dataInnerFlagNodeID       = simClasses.pargrid.invalidDataID();
   Hybrid::dataInnerFlagParticleID   = simClasses.pargrid.invalidDataID();
#ifdef USE_XMIN_BOUNDARY
   Hybrid::dataXminFlagID            = simClasses.pargrid.invalidDataID();
#endif
   
   // Create a parallel data arrays
   Hybrid::dataFaceBID = simClasses.pargrid.addUserData<Real>("faceB",block::SIZE*3);
   if(Hybrid::dataFaceBID == simClasses.pargrid.invalidCellID()) {
      simClasses.logger << "(USER) ERROR: Failed to add faceB array to ParGrid!" << endl << write;
      return false;
   }
   Hybrid::dataFaceJID = simClasses.pargrid.addUserData<Real>("faceJ",block::SIZE*3);
   if(Hybrid::dataFaceJID == simClasses.pargrid.invalidCellID()) {
      simClasses.logger << "(USER) ERROR: Failed to add faceJ array to ParGrid!" << endl << write;
      return false;
   }
   Hybrid::dataCellRhoQiID = simClasses.pargrid.addUserData<Real>("cellRhoQi",block::SIZE*1);
   if(Hybrid::dataCellRhoQiID == simClasses.pargrid.invalidCellID()) {
      simClasses.logger << "(USER) ERROR: Failed to add cellRhoQi array to ParGrid!" << endl << write;
      return false;
   }
   Hybrid::dataCellBID = simClasses.pargrid.addUserData<Real>("cellB",block::SIZE*3);
   if(Hybrid::dataCellBID == simClasses.pargrid.invalidCellID()) {
      simClasses.logger << "(USER) ERROR: Failed to add cellB array to ParGrid!" << endl << write;
      return false;
   }
   Hybrid::dataCellJID = simClasses.pargrid.addUserData<Real>("cellJ",block::SIZE*3);
   if(Hybrid::dataCellJID == simClasses.pargrid.invalidCellID()) {
      simClasses.logger << "(USER) ERROR: Failed to add cellJ array to ParGrid!" << endl << write;
      return false;
   }
   Hybrid::dataCellUeID = simClasses.pargrid.addUserData<Real>("cellUe",block::SIZE*3);
   if(Hybrid::dataCellUeID == simClasses.pargrid.invalidCellID()) {
      simClasses.logger << "(USER) ERROR: Failed to add cellUe array to ParGrid!" << endl << write;
      return false;
   }
   Hybrid::dataCellJiID = simClasses.pargrid.addUserData<Real>("cellJi",block::SIZE*3);
   if(Hybrid::dataCellJiID == simClasses.pargrid.invalidCellID()) {
      simClasses.logger << "(USER) ERROR: Failed to add cellJi array to ParGrid!" << endl << write;
      return false;
   }
   Hybrid::dataCellIonosphereID = simClasses.pargrid.addUserData<Real>("cellIonosphere",block::SIZE*Hybrid::N_ionospherePopulations);
   if(Hybrid::dataCellIonosphereID == simClasses.pargrid.invalidCellID()) {
      simClasses.logger << "(USER) ERROR: Failed to add cellIonosphere array to ParGrid!" << endl << write;
      return false;
   }
   Hybrid::dataCellExosphereID = simClasses.pargrid.addUserData<Real>("cellExosphere",block::SIZE*Hybrid::N_exospherePopulations);
   if(Hybrid::dataCellExosphereID == simClasses.pargrid.invalidCellID()) {
      simClasses.logger << "(USER) ERROR: Failed to add cellExosphere array to ParGrid!" << endl << write;
      return false;
   }
   Hybrid::dataNodeRhoQiID = simClasses.pargrid.addUserData<Real>("nodeRhoQi",block::SIZE*1);
   if(Hybrid::dataNodeRhoQiID == simClasses.pargrid.invalidCellID()) {
      simClasses.logger << "(USER) ERROR: Failed to add nodeRhoQi array to ParGrid!" << endl << write;
      return false;
   }
   Hybrid::dataNodeEID = simClasses.pargrid.addUserData<Real>("nodeE",block::SIZE*3);
   if(Hybrid::dataNodeEID == simClasses.pargrid.invalidCellID()) {
      simClasses.logger << "(USER) ERROR: Failed to add nodeE array to ParGrid!" << endl << write;
      return false;
   }
   Hybrid::dataNodeBID = simClasses.pargrid.addUserData<Real>("nodeB",block::SIZE*3);
   if(Hybrid::dataNodeBID == simClasses.pargrid.invalidCellID()) {
      simClasses.logger << "(USER) ERROR: Failed to add nodeB array to ParGrid!" << endl << write;
      return false;
   }
   Hybrid::dataNodeJID = simClasses.pargrid.addUserData<Real>("nodeJ",block::SIZE*3);
   if(Hybrid::dataNodeJID == simClasses.pargrid.invalidCellID()) {
      simClasses.logger << "(USER) ERROR: Failed to add nodeJ array to ParGrid!" << endl << write;
      return false;
   }
   Hybrid::dataNodeUeID = simClasses.pargrid.addUserData<Real>("nodeUe",block::SIZE*3);
   if(Hybrid::dataNodeUeID == simClasses.pargrid.invalidCellID()) {
      simClasses.logger << "(USER) ERROR: Failed to add nodeUe array to ParGrid!" << endl << write;
      return false;
   }
   Hybrid::dataNodeJiID = simClasses.pargrid.addUserData<Real>("nodeJi",block::SIZE*3);
   if(Hybrid::dataNodeJiID == simClasses.pargrid.invalidCellID()) {
      simClasses.logger << "(USER) ERROR: Failed to add nodeJi array to ParGrid!" << endl << write;
      return false;
   }
#ifdef USE_RESISTIVITY
   Hybrid::dataNodeEtaID = simClasses.pargrid.addUserData<Real>("nodeEta",block::SIZE*1);
   if(Hybrid::dataNodeEtaID == simClasses.pargrid.invalidCellID()) {
      simClasses.logger << "(USER) ERROR: Failed to add nodeEta array to ParGrid!" << endl << write;
      return false;
   }
#endif
   // counters
   Hybrid::dataCounterCellMaxUeID = simClasses.pargrid.addUserData<Real>("counterCellMaxUe",block::SIZE*1);
   if(Hybrid::dataCounterCellMaxUeID == simClasses.pargrid.invalidCellID()) {
      simClasses.logger << "(USER) ERROR: Failed to add counterCellMaxUe array to ParGrid!" << endl << write;
      return false;
   }
   Hybrid::dataCounterCellMaxViID = simClasses.pargrid.addUserData<Real>("counterCellMaxVi",block::SIZE*1);
   if(Hybrid::dataCounterCellMaxViID == simClasses.pargrid.invalidCellID()) {
      simClasses.logger << "(USER) ERROR: Failed to add counterCellMaxVi array to ParGrid!" << endl << write;
      return false;
   }
   Hybrid::dataCounterCellMinRhoQiID = simClasses.pargrid.addUserData<Real>("counterCellMinRhoQi",block::SIZE*1);
   if(Hybrid::dataCounterCellMinRhoQiID == simClasses.pargrid.invalidCellID()) {
      simClasses.logger << "(USER) ERROR: Failed to add counterCellMinRhoQi array to ParGrid!" << endl << write;
      return false;
   }
#ifdef USE_ECUT
   Hybrid::dataCounterNodeEcutID = simClasses.pargrid.addUserData<Real>("counterNodeEcut",block::SIZE*1);
   if(Hybrid::dataCounterNodeEcutID == simClasses.pargrid.invalidCellID()) {
      simClasses.logger << "(USER) ERROR: Failed to add counterNodeEcut array to ParGrid!" << endl << write;
      return false;
   }
#endif
   
   // create stencils
   Hybrid::accumulationStencilID = sim.inverseStencilID;
   
   // flags
   Hybrid::dataInnerFlagFieldID = simClasses.pargrid.addUserData<bool>("innerFlagField",block::SIZE*1);
   if(Hybrid::dataInnerFlagFieldID == simClasses.pargrid.invalidCellID()) {
      simClasses.logger << "(USER) ERROR: Failed to add innerFlagField array to ParGrid!" << endl << write;
      return false;
   }
   Hybrid::dataInnerFlagNodeID = simClasses.pargrid.addUserData<bool>("innerFlagNode",block::SIZE*1);
   if(Hybrid::dataInnerFlagNodeID == simClasses.pargrid.invalidCellID()) {
      simClasses.logger << "(USER) ERROR: Failed to add innerFlagNode array to ParGrid!" << endl << write;
      return false;
   }
   Hybrid::dataInnerFlagParticleID = simClasses.pargrid.addUserData<bool>("innerFlagParticle",1);
   if(Hybrid::dataInnerFlagParticleID == simClasses.pargrid.invalidCellID()) {
      simClasses.logger << "(USER) ERROR: Failed to add innerFlagParticle array to ParGrid!" << endl << write;
      return false;
   }
#ifdef USE_XMIN_BOUNDARY
   Hybrid::dataXminFlagID = simClasses.pargrid.addUserData<bool>("xMinFlag",block::SIZE*1);
   if(Hybrid::dataXminFlagID == simClasses.pargrid.invalidCellID()) {
      simClasses.logger << "(USER) ERROR: Failed to add xMinFlag array to ParGrid!" << endl << write;
      return false;
   }
#endif
#ifdef ION_SPECTRA_ALONG_ORBIT
   Hybrid::dataSpectraFlagID = simClasses.pargrid.addUserData<bool>("spectraFlag",1);
   if(Hybrid::dataSpectraFlagID == simClasses.pargrid.invalidCellID()) {
      simClasses.logger << "(USER) ERROR: Failed to add spectraFlag array to ParGrid!" << endl << write;
      return false;
   }
   // dynamic pargrid array for energy spectra
   /*Hybrid::dataSpectraID = simClasses.pargrid.addUserData<Dist>("spectra",0,true);
   if(Hybrid::dataSpectraID == simClasses.pargrid.invalidCellID()) {
      simClasses.logger << "(USER) ERROR: Failed to add spectra array to ParGrid!" << endl << write;
      return false;
   }*/
#endif
   
   // Create data transfers

   if(simClasses.pargrid.addDataTransfer(Hybrid::dataFaceBID,pargrid::DEFAULT_STENCIL) == false) {
      simClasses.logger << "(USER) ERROR: Failed to add faceB data transfer!" << endl << write; return false;
   }
   if(simClasses.pargrid.addDataTransfer(Hybrid::dataFaceJID,pargrid::DEFAULT_STENCIL) == false) {
      simClasses.logger << "(USER) ERROR: Failed to add faceJ data transfer!" << endl << write; return false;
   }
   if(simClasses.pargrid.addDataTransfer(Hybrid::dataCellRhoQiID,pargrid::DEFAULT_STENCIL) == false) {
      simClasses.logger << "(USER) ERROR: Failed to add cellRhoQi data transfer 1!" << endl << write; return false;
   }
   if(simClasses.pargrid.addDataTransfer(Hybrid::dataCellRhoQiID,Hybrid::accumulationStencilID) == false) {
      simClasses.logger << "(USER) ERROR: Failed to add cellRhoQi data transfer 2!" << endl << write; return false;
   }
   if(simClasses.pargrid.addDataTransfer(Hybrid::dataCellBID,pargrid::DEFAULT_STENCIL) == false) {
      simClasses.logger << "(USER) ERROR: Failed to add cellB data transfer!" << endl << write; return false;
   }
   if(simClasses.pargrid.addDataTransfer(Hybrid::dataCellJID,pargrid::DEFAULT_STENCIL) == false) {
      simClasses.logger << "(USER) ERROR: Failed to add cellJ data transfer!" << endl << write; return false;
   }
   if(simClasses.pargrid.addDataTransfer(Hybrid::dataCellUeID,pargrid::DEFAULT_STENCIL) == false) {
      simClasses.logger << "(USER) ERROR: Failed to add cellUe data transfer!" << endl << write; return false;
   }
   if(simClasses.pargrid.addDataTransfer(Hybrid::dataCellJiID,pargrid::DEFAULT_STENCIL) == false) {
      simClasses.logger << "(USER) ERROR: Failed to add cellJi data transfer 1!" << endl << write; return false;
   }
   if(simClasses.pargrid.addDataTransfer(Hybrid::dataCellJiID,Hybrid::accumulationStencilID) == false) {
      simClasses.logger << "(USER) ERROR: Failed to add cellJi data transfer 2!" << endl << write; return false;
   }
   if(simClasses.pargrid.addDataTransfer(Hybrid::dataCellIonosphereID,pargrid::DEFAULT_STENCIL) == false) {
      simClasses.logger << "(USER) ERROR: Failed to add cellIonosphere data transfer!" << endl << write; return false;
   }
   if(simClasses.pargrid.addDataTransfer(Hybrid::dataCellExosphereID,pargrid::DEFAULT_STENCIL) == false) {
      simClasses.logger << "(USER) ERROR: Failed to add cellExosphere data transfer!" << endl << write; return false;
   }
   if(simClasses.pargrid.addDataTransfer(Hybrid::dataNodeRhoQiID,pargrid::DEFAULT_STENCIL) == false) {
      simClasses.logger << "(USER) ERROR: Failed to add nodeRhoQi data transfer!" << endl << write; return false;
   }
   if(simClasses.pargrid.addDataTransfer(Hybrid::dataNodeEID,pargrid::DEFAULT_STENCIL) == false) {
      simClasses.logger << "(USER) ERROR: Failed to add nodeE data transfer!" << endl << write; return false;
   }
   if(simClasses.pargrid.addDataTransfer(Hybrid::dataNodeBID,pargrid::DEFAULT_STENCIL) == false) {
      simClasses.logger << "(USER) ERROR: Failed to add nodeB data transfer!" << endl << write; return false;
   }
   if(simClasses.pargrid.addDataTransfer(Hybrid::dataNodeJID,pargrid::DEFAULT_STENCIL) == false) {
      simClasses.logger << "(USER) ERROR: Failed to add nodeJ data transfer!" << endl << write; return false;
   }
   if(simClasses.pargrid.addDataTransfer(Hybrid::dataNodeUeID,pargrid::DEFAULT_STENCIL) == false) {
      simClasses.logger << "(USER) ERROR: Failed to add nodeUe data transfer!" << endl << write; return false;
   }
   if(simClasses.pargrid.addDataTransfer(Hybrid::dataNodeJiID,pargrid::DEFAULT_STENCIL) == false) {
      simClasses.logger << "(USER) ERROR: Failed to add nodeJi data transfer!" << endl << write; return false;
   }

   Real* faceB               = reinterpret_cast<Real*>(simClasses.pargrid.getUserData(Hybrid::dataFaceBID));
   Real* faceJ               = reinterpret_cast<Real*>(simClasses.pargrid.getUserData(Hybrid::dataFaceJID));
   Real* cellRhoQi           = reinterpret_cast<Real*>(simClasses.pargrid.getUserData(Hybrid::dataCellRhoQiID));
   Real* cellB               = reinterpret_cast<Real*>(simClasses.pargrid.getUserData(Hybrid::dataCellBID));
   Real* cellJ               = reinterpret_cast<Real*>(simClasses.pargrid.getUserData(Hybrid::dataCellJID));
   Real* cellUe              = reinterpret_cast<Real*>(simClasses.pargrid.getUserData(Hybrid::dataCellUeID));
   Real* cellJi              = reinterpret_cast<Real*>(simClasses.pargrid.getUserData(Hybrid::dataCellJiID));
   Real* cellIonosphere      = reinterpret_cast<Real*>(simClasses.pargrid.getUserData(Hybrid::dataCellIonosphereID));
   Real* cellExosphere       = reinterpret_cast<Real*>(simClasses.pargrid.getUserData(Hybrid::dataCellExosphereID));
   Real* nodeRhoQi           = reinterpret_cast<Real*>(simClasses.pargrid.getUserData(Hybrid::dataNodeRhoQiID));
   Real* nodeE               = reinterpret_cast<Real*>(simClasses.pargrid.getUserData(Hybrid::dataNodeEID));
   Real* nodeB               = reinterpret_cast<Real*>(simClasses.pargrid.getUserData(Hybrid::dataNodeBID));
   Real* nodeJ               = reinterpret_cast<Real*>(simClasses.pargrid.getUserData(Hybrid::dataNodeJID));
   Real* nodeUe              = reinterpret_cast<Real*>(simClasses.pargrid.getUserData(Hybrid::dataNodeUeID));
   Real* nodeJi              = reinterpret_cast<Real*>(simClasses.pargrid.getUserData(Hybrid::dataNodeJiID));
#ifdef USE_RESISTIVITY
   Real* nodeEta             = reinterpret_cast<Real*>(simClasses.pargrid.getUserData(Hybrid::dataNodeEtaID));
#endif
   Real* counterCellMaxUe    = reinterpret_cast<Real*>(simClasses.pargrid.getUserData(Hybrid::dataCounterCellMaxUeID));
   Real* counterCellMaxVi    = reinterpret_cast<Real*>(simClasses.pargrid.getUserData(Hybrid::dataCounterCellMaxViID));
   Real* counterCellMinRhoQi = reinterpret_cast<Real*>(simClasses.pargrid.getUserData(Hybrid::dataCounterCellMinRhoQiID));
#ifdef USE_ECUT
   Real* counterNodeEcut     = reinterpret_cast<Real*>(simClasses.pargrid.getUserData(Hybrid::dataCounterNodeEcutID));
#endif
   bool* innerFlagField      = reinterpret_cast<bool*>(simClasses.pargrid.getUserData(Hybrid::dataInnerFlagFieldID));   
   bool* innerFlagNode       = reinterpret_cast<bool*>(simClasses.pargrid.getUserData(Hybrid::dataInnerFlagNodeID));
   bool* innerFlagParticle   = reinterpret_cast<bool*>(simClasses.pargrid.getUserData(Hybrid::dataInnerFlagParticleID));
#ifdef USE_XMIN_BOUNDARY
   bool* xMinFlag            = reinterpret_cast<bool*>(simClasses.pargrid.getUserData(Hybrid::dataXminFlagID));
#endif

#ifdef ION_SPECTRA_ALONG_ORBIT
   bool* spectraFlag        = reinterpret_cast<bool*>(simClasses.pargrid.getUserData(Hybrid::dataSpectraFlagID));
   Hybrid::spectraFileLineCnt = 0;
   vector<string> orbitFiles;
   cr.add("Analysis.orbit_spectra_t_start","Simulation time to start orbit spectra analysis (real)",-1);
   cr.add("Analysis.orbit_spectra_t_end","Simulation time to end orbit spectra analysis (real)",-1);
   cr.add("Analysis.orbit_spectra_max_particles","Maximum number of recorded particles for orbit spectra (real)",1e5);
   cr.add("Analysis.orbit_spectra_write_interval_timesteps","Interval of spectra file writing (real)",10);
   cr.addComposed("Analysis.orbitfile","File names of spacecraft orbits for spectra (string)");
   cr.parse();
   cr.get("Analysis.orbit_spectra_t_start",Hybrid::tStartSpectra);
   cr.get("Analysis.orbit_spectra_t_end",Hybrid::tEndSpectra);
   cr.get("Analysis.orbit_spectra_max_particles",Hybrid::maxRecordedSpectraParticles);
   cr.get("Analysis.orbit_spectra_write_interval_timesteps",Hybrid::writeIntervalTimesteps);
   cr.get("Analysis.orbitfile",orbitFiles);
   simClasses.logger
     << "(HYBRID) CELL SPECTRA: Recording particle spectra between: t = " << Hybrid::tStartSpectra << " ... " << Hybrid::tEndSpectra << " s" << endl
     << "(HYBRID) CELL SPECTRA: Maximum number of recorded spectra particles: " << Hybrid::maxRecordedSpectraParticles << endl
     << "(HYBRID) CELL SPECTRA: Writing interval of spectra particles: " << Hybrid::writeIntervalTimesteps << " timesteps" << endl;
   
   vector< vector<Real> > orbitCoordinates;
   // only master reads orbit coordinates from files
   if(sim.mpiRank==sim.MASTER_RANK) {
      for (vector<string>::iterator it=orbitFiles.begin(); it!=orbitFiles.end(); ++it) {
         simClasses.logger << "(HYBRID) CELL SPECTRA: Reading a spacecraft orbit file: " << *it << endl;
         vector< vector<Real> > tmpCrd;
         tmpCrd = readRealsFromFile(*it);
         if(checkOrbit(tmpCrd) == false) {
            simClasses.logger << "(HYBRID) ERROR: CELL SPECTRA: Bad orbit file (" << *it << ")" << endl << write;
            return false;
         }
         orbitCoordinates.insert(orbitCoordinates.end(),tmpCrd.begin(),tmpCrd.end());
      }
      simClasses.logger << "(HYBRID) CELL SPECTRA: Total of " << orbitCoordinates.size() << " orbit points read" << endl;
   }
   if(MPI_BcastFromMaster2DVector(sim,orbitCoordinates) == false) {
      simClasses.logger << "(HYBRID) ERROR: CELL SPECTRA: failed to distribute orbit coordinates to all MPI PEs" << endl << write;
      return false;
   }
   /*cerr << sim.mpiRank << ") " << orbitCoordinates.size() << endl;
   if(sim.mpiRank==sim.MASTER_RANK) {
      for(unsigned int i=0;i<orbitCoordinates.size();++i) {
         for(unsigned int j=0;j<orbitCoordinates[i].size();++j) {
            cout << orbitCoordinates[i][j] << " ";
         }
         cout << endl;
      }
   }*/
   int N_spectraCells = 0;
   /*pargrid::DataWrapper<Dist> wrapper = simClasses.pargrid.getUserDataDynamic<Dist>(Hybrid::dataSpectraID);
   if (wrapper.valid() == false) {
      simClasses.logger << "(HYBRID) ERROR: CELL SPECTRA: dynamic user data wrapper failed" << endl << write;
      return false;
   }*/
#endif

   const size_t scalarArraySize = simClasses.pargrid.getNumberOfAllCells()*block::SIZE;
   const size_t vectorArraySize = simClasses.pargrid.getNumberOfAllCells()*block::SIZE*3;
   const size_t ionoArraySize   = simClasses.pargrid.getNumberOfAllCells()*block::SIZE*Hybrid::N_ionospherePopulations;
   const size_t exoArraySize    = simClasses.pargrid.getNumberOfAllCells()*block::SIZE*Hybrid::N_exospherePopulations;
   
   // Initial state (skip if simulation was restarted).
   // Iterate over all blocks local to this process:
   if (sim.restarted == false) {
      for(size_t i=0; i<vectorArraySize; ++i) { faceB[i] = 0.0; }
      for(size_t i=0; i<vectorArraySize; ++i) { faceJ[i] = 0.0; }
      for(size_t i=0; i<vectorArraySize; ++i) { cellB[i] = 0.0; }
      for(size_t i=0; i<vectorArraySize; ++i) { cellJ[i] = 0.0; }
      for(size_t i=0; i<vectorArraySize; ++i) { cellUe[i] = 0.0; }
      for(size_t i=0; i<vectorArraySize; ++i) { cellJi[i] = 0.0; }
      for(size_t i=0; i<vectorArraySize; ++i) { nodeE[i] = 0.0; }
      for(size_t i=0; i<vectorArraySize; ++i) { nodeB[i] = 0.0; }
      for(size_t i=0; i<vectorArraySize; ++i) { nodeJ[i] = 0.0; }
      for(size_t i=0; i<vectorArraySize; ++i) { nodeUe[i] = 0.0; }
      for(size_t i=0; i<vectorArraySize; ++i) { nodeJi[i] = 0.0; }
#ifdef USE_RESISTIVITY
      for(size_t i=0; i<scalarArraySize; ++i) { nodeEta[i] = 0.0; }
#endif
      for(size_t i=0; i<scalarArraySize; ++i) { nodeRhoQi[i] = 0.0; }
      for(size_t i=0; i<scalarArraySize; ++i) { cellRhoQi[i] = 0.0; }
      for(size_t i=0; i<ionoArraySize;   ++i) { cellIonosphere[i] = 0.0; }
      for(size_t i=0; i<exoArraySize;    ++i) { cellExosphere[i] = 0.0; }
      // counters
      for(size_t i=0; i<scalarArraySize; ++i) { counterCellMaxUe[i] = 0.0; }
      for(size_t i=0; i<scalarArraySize; ++i) { counterCellMaxVi[i] = 0.0; }
      for(size_t i=0; i<scalarArraySize; ++i) { counterCellMinRhoQi[i] = 0.0; }
#ifdef USE_ECUT
      for(size_t i=0; i<scalarArraySize; ++i) { counterNodeEcut[i] = 0.0; }
#endif

#ifdef ION_SPECTRA_ALONG_ORBIT
      // variable to record cellid and cell centroid coordinates for output
      vector< vector<Real> > spectraCellIDXYZ;
#endif      
      // create flags for inner boundary
      const Real* crd = getBlockCoordinateArray(sim,simClasses);
      for (pargrid::CellID b=0; b<simClasses.pargrid.getNumberOfLocalCells(); ++b) {
	 const size_t b3 = 3*b;
	 innerFlagParticle[b] = false;
#ifdef ION_SPECTRA_ALONG_ORBIT
         spectraFlag[b] = false;
#endif
	 for(int k=0;k<block::WIDTH_Z;++k) for(int j=0;j<block::WIDTH_Y;++j) for(int i=0;i<block::WIDTH_X;++i) {
	    const int n = (b*block::SIZE+block::index(i,j,k));
	    //faceB[n*3+1] = simClasses.pargrid.getGlobalIDs()[b];//*x/1e6; // faceBy
	    // inner boundary flags
	    const Real xCellCenter = crd[b3+0] + (i+0.5)*Hybrid::dx;
	    const Real yCellCenter = crd[b3+1] + (j+0.5)*Hybrid::dx;
	    const Real zCellCenter = crd[b3+2] + (k+0.5)*Hybrid::dx;
	    const Real r2 = sqr(xCellCenter) + sqr(yCellCenter) + sqr(zCellCenter);
	    if(r2 < Hybrid::R2_fieldObstacle) { innerFlagField[n] = true; }
	    else                              { innerFlagField[n] = false; }
	    const Real rp2 = sqr(sqrt(r2) - 0.5*sqrt(3)*Hybrid::dx);
	    if(rp2 < Hybrid::R2_particleObstacle) { innerFlagParticle[b] = true; }
	    const Real xNode = crd[b3+0] + (i+1.0)*Hybrid::dx;
	    const Real yNode = crd[b3+1] + (j+1.0)*Hybrid::dx;
	    const Real zNode = crd[b3+2] + (k+1.0)*Hybrid::dx;
	    const Real rNode2 = sqr(xNode) + sqr(yNode) + sqr(zNode);
	    if(rNode2 < Hybrid::R2_fieldObstacle) { innerFlagNode[n] = true; }
	    else                                  { innerFlagNode[n] = false; }
#ifdef USE_RESISTIVITY
            nodeEta[n] = getResistivity(sim,simClasses,xNode,yNode,zNode);
#endif
#ifdef USE_XMIN_BOUNDARY
            if(xCellCenter < Hybrid::xMinBoundary) { xMinFlag[n] = true; }
            else                                   { xMinFlag[n] = false; }
#endif
#ifdef ION_SPECTRA_ALONG_ORBIT
            const Real xmin = xCellCenter - 0.5*Hybrid::dx;
            const Real xmax = xCellCenter + 0.5*Hybrid::dx;
            const Real ymin = yCellCenter - 0.5*Hybrid::dx;
            const Real ymax = yCellCenter + 0.5*Hybrid::dx;
            const Real zmin = zCellCenter - 0.5*Hybrid::dx;
            const Real zmax = zCellCenter + 0.5*Hybrid::dx;
            for(unsigned int i=0;i<orbitCoordinates.size();++i) {
               const Real xi = orbitCoordinates[i][0];
               const Real yi = orbitCoordinates[i][1];
               const Real zi = orbitCoordinates[i][2];
               if( (xi >= xmin && xi <= xmax) && (yi >= ymin && yi <= ymax) && (zi >= zmin && zi <= zmax) ) {
                  spectraFlag[b] = true;
                  N_spectraCells += 1;
                  // introduce Dists only once per block
                  /*if(wrapper.size(b) <= 0) {
                     for(unsigned int l = 0;l<Hybrid::N_populations;++l) {
                        wrapper.push_back(b,Dist());
                     }
                  }*/
                  vector<Real> tmp1 = {simClasses.pargrid.getGlobalIDs()[b],xCellCenter,yCellCenter,zCellCenter};
                  spectraCellIDXYZ.push_back(tmp1);
                  break;
               }
            }
#endif
	 }
      }
#ifdef ION_SPECTRA_ALONG_ORBIT
      // zero energy spectra counters
      /*for (pargrid::CellID b=0; b<simClasses.pargrid.getNumberOfLocalCells(); ++b) {
         Dist* s = wrapper.data()[b];
         if(s != NULL) {
            for(unsigned int i = 0;i<wrapper.size(b);++i) {
               s[i].reset();
            }
         }
      }*/
      // sum N_spectraCells of all PEs
      int N_spectraCellsGlobalSum = 0.0;
      MPI_Reduce(&N_spectraCells,&N_spectraCellsGlobalSum,1,MPI_Type<int>(),MPI_SUM,sim.MASTER_RANK,sim.comm);
      // send (cellid,x,y,z) rows from PEs to master (this could be optimized using MPI_Gatherv)
      MPI_Barrier(sim.comm);
      if(sim.mpiRank != sim.MASTER_RANK) {
         for(unsigned int i = 0;i<spectraCellIDXYZ.size();++i) {
            if(spectraCellIDXYZ[i].size() != 4) {
               simClasses.logger << "(HYBRID) ERROR: CELL SPECTRA: Error with cell indices and coordinates row" << endl << write;
               return false;
            }
            MPI_Send(&spectraCellIDXYZ[i][0],4,MPI_Type<Real>(),sim.MASTER_RANK,0,sim.comm);
         }
      }
      if(sim.mpiRank == sim.MASTER_RANK) {
         int N_rowsToReceive = N_spectraCellsGlobalSum - N_spectraCells;
         for(unsigned int i = 0;i<N_rowsToReceive;++i) { 
            vector<Real> tmpRecv;
            tmpRecv.resize(4);
            MPI_Recv(&tmpRecv[0],4,MPI_Type<Real>(),MPI_ANY_SOURCE,0,sim.comm,MPI_STATUS_IGNORE);
            spectraCellIDXYZ.push_back(tmpRecv);
         }
      }
      MPI_Barrier(sim.comm);
      if(sim.mpiRank == sim.MASTER_RANK) {
         simClasses.logger << "(HYBRID) CELL SPECTRA: Recording ion spectra in " << N_spectraCellsGlobalSum << " cells" << endl << write;
         // write spectra cell indices file
         ofstream spectraCellIndicesFile;
         spectraCellIndicesFile.open("spectra_cell_indices.dat",ios_base::out);
         spectraCellIndicesFile.precision(3);
         spectraCellIndicesFile << scientific;
         spectraCellIndicesFile << "% globalid x y z" << endl;
         for(unsigned int i = 0;i<spectraCellIDXYZ.size();++i) {
            if(spectraCellIDXYZ[i].size() != 4) {
               simClasses.logger << "(HYBRID) ERROR: CELL SPECTRA: Error when creating cell indices file" << endl << write;
               return false;
            }
            spectraCellIndicesFile << static_cast<long long>(spectraCellIDXYZ[i][0]) << " " << spectraCellIDXYZ[i][1] << " " << spectraCellIDXYZ[i][2] << " " << spectraCellIDXYZ[i][3] << endl;
         }
         spectraCellIndicesFile << flush;
         spectraCellIndicesFile.close();
      }
#endif
#ifdef USE_B_INITIAL
      // set initial B
      for (pargrid::CellID b=0; b<simClasses.pargrid.getNumberOfLocalCells(); ++b) {
         const size_t b3 = 3*b;
         for(int k=0;k<block::WIDTH_Z;++k) for(int j=0;j<block::WIDTH_Y;++j) for(int i=0;i<block::WIDTH_X;++i) {
            const int n = (b*block::SIZE+block::index(i,j,k));
            const Real xFaceXCenter = crd[b3+0] + (i+1.0)*Hybrid::dx;
            const Real yFaceXCenter = crd[b3+1] + (j+0.5)*Hybrid::dx;
            const Real zFaceXCenter = crd[b3+2] + (k+0.5)*Hybrid::dx;
            const Real xFaceYCenter = crd[b3+0] + (i+0.5)*Hybrid::dx;
            const Real yFaceYCenter = crd[b3+1] + (j+1.0)*Hybrid::dx;
            const Real zFaceYCenter = crd[b3+2] + (k+0.5)*Hybrid::dx;
            const Real xFaceZCenter = crd[b3+0] + (i+0.5)*Hybrid::dx;
            const Real yFaceZCenter = crd[b3+1] + (j+0.5)*Hybrid::dx;
            const Real zFaceZCenter = crd[b3+2] + (k+1.0)*Hybrid::dx;
            Real B_initial[3] = {0.0,0.0,0.0};
            // +x face
            setInitialB(xFaceXCenter,yFaceXCenter,zFaceXCenter,B_initial);
            faceB[n*3+0] = B_initial[0];
            // +y face
            setInitialB(xFaceYCenter,yFaceYCenter,zFaceYCenter,B_initial);
            faceB[n*3+1] = B_initial[1];
            // +z face
            setInitialB(xFaceZCenter,yFaceZCenter,zFaceZCenter,B_initial);
            faceB[n*3+2] = B_initial[2];
	    const Real xCellCenter = crd[b3+0] + (i+0.5)*Hybrid::dx;
	    const Real yCellCenter = crd[b3+1] + (j+0.5)*Hybrid::dx;
	    const Real zCellCenter = crd[b3+2] + (k+0.5)*Hybrid::dx;
            setInitialB(xCellCenter,yCellCenter,zCellCenter,B_initial);
            cellB[n*3+0] = B_initial[0];
            cellB[n*3+1] = B_initial[1];
            cellB[n*3+2] = B_initial[2];
         }
      }
#endif
   }

   // initialize particle lists: uniform
   for (vector<string>::iterator it=uniformPopulations.begin(); it!=uniformPopulations.end(); ++it) {
      simClasses.logger << "(HYBRID) Initializing an uniform particle population: " << *it << endl << write;
      particleLists.push_back(new ParticleListHybrid<Species,Particle<Real> >);
      if (particleLists[particleLists.size()-1]->initialize(sim,simClasses,cr,objectFactories,*it) == false) { return false; }
      Hybrid::populationNames.push_back(*it);
   }
   // initialize particle lists: solar wind
   for (vector<string>::iterator it=solarwindPopulations.begin(); it!=solarwindPopulations.end(); ++it) {
      simClasses.logger << "(HYBRID) Initializing a solar wind particle population: " << *it << endl << write;
      particleLists.push_back(new ParticleListHybrid<Species,Particle<Real> >);
      if (particleLists[particleLists.size()-1]->initialize(sim,simClasses,cr,objectFactories,*it) == false) { return false; }
      Hybrid::populationNames.push_back(*it);
   }
   // initialize particle lists: ionosphere
   for (vector<string>::iterator it=ionospherePopulations.begin(); it!=ionospherePopulations.end(); ++it) {
      simClasses.logger << "(HYBRID) Initializing an ionospheric particle population: " << *it << endl << write;
      particleLists.push_back(new ParticleListHybrid<Species,Particle<Real> >);
      if (particleLists[particleLists.size()-1]->initialize(sim,simClasses,cr,objectFactories,*it) == false) { return false; }
      Hybrid::populationNames.push_back(*it);
   }   
   // initialize particle lists: exosphere
   for (vector<string>::iterator it=exospherePopulations.begin(); it!=exospherePopulations.end(); ++it) {
      simClasses.logger << "(HYBRID) Initializing an exospheric particle population: " << *it << endl << write;
      particleLists.push_back(new ParticleListHybrid<Species,Particle<Real> >);
      if (particleLists[particleLists.size()-1]->initialize(sim,simClasses,cr,objectFactories,*it) == false) { return false; }
      Hybrid::populationNames.push_back(*it);
   }
   // population output configurations
   for(unsigned int i=0;i<particleLists.size();++i) {
      const Species* species = reinterpret_cast<const Species*>(particleLists[i]->getSpecies());
      if(species->outIncludeInPlasma == true) {
         Hybrid::outputPlasmaPopId.push_back(i);
      }
      if(species->outStr == string("tot")) {
         simClasses.logger << "(USER) ERROR: Particle species cannot have output_str = tot (" << species->name << ")" << endl << write;         
         return false;
      }
      if(species->outStr == string("-")) {
         Hybrid::outputPopVarId.push_back(-1);
         continue;
      }
      bool strFound = false;
      for(unsigned int j=0;j<Hybrid::outputPopVarStr.size();++j) {
         if(species->outStr == Hybrid::outputPopVarStr[j]) {
            Hybrid::outputPopVarId.push_back(j);
            Hybrid::outputPopVarIdVector[j].push_back(i);
            strFound = true;
         }
      }
      if(strFound == false) {
         const int newOutputId = static_cast<int>( Hybrid::outputPopVarStr.size() );
         Hybrid::outputPopVarId.push_back(newOutputId);
         Hybrid::outputPopVarStr.push_back(species->outStr);
         vector<unsigned int> a;
         a.push_back(i);
         Hybrid::outputPopVarIdVector.push_back(a);
      }
   }
   Hybrid::N_outputPopVars =  Hybrid::outputPopVarStr.size();
   // check number of particle lists and populations
   if( (Hybrid::N_populations   != particleLists.size()) || 
       (Hybrid::N_populations   != Hybrid::populationNames.size()) || 
       (Hybrid::N_populations   != Hybrid::outputPopVarId.size()) ||
       (Hybrid::N_populations    < Hybrid::N_outputPopVars) ||
       (Hybrid::N_populations    < Hybrid::outputPlasmaPopId.size()) ||
       (Hybrid::N_outputPopVars != Hybrid::outputPopVarIdVector.size()) ) {
      simClasses.logger << "(HYBRID) ERROR: Something went wrong in particle list initialization" << endl << write;
      return false;
   }   

   // determine solar wind properties
   Real ne = 0.0; // total electron density
   Real rhom = 0.0; // total ion mass density
   Real Ubulk = 0.0; // bulk speed
   Real vA = 0.0; // alfven velocity
   for (size_t s=0;s<Hybrid::swPops.size();++s) {
      ne += Hybrid::swPops[s].q*Hybrid::swPops[s].n;
      rhom += Hybrid::swPops[s].m*Hybrid::swPops[s].n;
      Ubulk += Hybrid::swPops[s].m*Hybrid::swPops[s].n*Hybrid::swPops[s].U;
   }
   ne /= constants::CHARGE_ELEMENTARY;
   const Real Btot2 = sqr(Hybrid::IMFBx) + sqr(Hybrid::IMFBy) + sqr(Hybrid::IMFBz);
   const Real Btot = sqrt(Btot2);
   if(rhom > 0.0) {
      vA = Btot/( sqrt(constants::PERMEABILITY*rhom) );
      Ubulk /= rhom;
   }
   else {
      Ubulk = 0.0;
   }
   Real Esw[3] = {0.0,0.0,0.0};
   Real Bsw[3] = {Hybrid::IMFBx,Hybrid::IMFBy,Hybrid::IMFBz};
   Real Usw[3] = {-Ubulk,0.0,0.0};
   Real VExB[3] = {0.0,0.0,0.0};
   cross(Bsw,Usw,Esw); // Esw = B x (-Ubulk,0,0)
   cross(Esw,Bsw,VExB); // VExB = E x B/B^2
   VExB[0] /= Btot2;
   VExB[1] /= Btot2;
   VExB[2] /= Btot2;
   const Real VExBMagnitude = normvec(VExB);
   Real vw = 0.0; // fastest whistler signal p. 28 Alho (2016)
   if(ne > 0.0 && Hybrid::dx > 0.0) {
      vw = 2.0*Btot*M_PI/( constants::PERMEABILITY*ne*constants::CHARGE_ELEMENTARY*Hybrid::dx );
   }
   simClasses.logger
     << "(UPSTREAM CFL CONDITIONS)" << endl
     << "dt = " << sim.dt << " s = " << sim.dt/1e-3 << " ms" << endl
     << "dx = " << Hybrid::dx/1e3 << " km = R_object/" << Hybrid::R_object/Hybrid::dx << " = " << Hybrid::dx/Hybrid::R_object << " R_object" << endl
     << "dx/dt = " << Hybrid::dx/sim.dt/1e3 << " km/s" << endl
     << "bulk speed = " << Ubulk/1e3 << " km/s" << endl
     << "alfven velocity = " << vA/1e3 << " km/s" << endl
     << "ExB drift velocity = (" << VExB[0]/1e3 << "," << VExB[1]/1e3 << "," << VExB[2]/1e3 << ") km/s" << endl
     << "ExB drift speed = " << VExBMagnitude/1e3 << " km/s" << endl
     << "Pickup ion avg speed (4*VExB/pi) = " << 4.0*VExBMagnitude/M_PI/1e3 << " km/s" << endl
     << "Pickup ion max speed (2*VExB) = " << 2.0*VExBMagnitude/1e3 << " km/s" << endl
     << "Fastest whistler speed = " << vw/1e3 << " km/s" << endl
     << "==== SOLAR WIND POPULATIONS ====" << endl;
   // solar wind populations
   // electron plasma frequency
   const Real omega_pe = sqrt( ne*sqr(constants::CHARGE_ELEMENTARY)/( constants::MASS_ELECTRON*constants::PERMITTIVITY  ) );
   // electron plasma period
   Real tPe = 0.0;
   // electron inertial length
   Real le = 0.0;
   if(omega_pe > 0.0) {
      tPe = 2.0*M_PI/omega_pe;
      le = constants::SPEED_LIGHT/omega_pe;
   }
   for (size_t s=0;s<Hybrid::swPops.size();++s) {
      // ion plasma frequency
      const Real omega_pi = sqrt( ne*sqr(Hybrid::swPops[s].q)/( Hybrid::swPops[s].m*constants::PERMITTIVITY  ) );
      // ion plasma period
      Real tPi = 0.0;
      if(omega_pi > 0.0) {
         tPi = 2.0*M_PI/omega_pi;
      }
      simClasses.logger
        << "plasma period(" << Hybrid::swPops[s].name << ") = " << tPi << " s = " << tPi/sim.dt << " dt" << endl;
   }
   simClasses.logger
     << "plasma period(e-) = " << tPe << " s = " << tPe/sim.dt << " dt" << endl;
   for (size_t s=0;s<Hybrid::swPops.size();++s) {
      // ion plasma frequency
      const Real omega_pi = sqrt( ne*sqr(Hybrid::swPops[s].q)/( Hybrid::swPops[s].m*constants::PERMITTIVITY  ) );
      // ion inertial length
      Real li = 0.0;
      if(omega_pi > 0.0) {
         li = constants::SPEED_LIGHT/omega_pi;
      }
      simClasses.logger
        << "inertial length(" << Hybrid::swPops[s].name << ") = " << li/1e3 << " km = " << li/Hybrid::dx << " dx" << endl;
   }
   simClasses.logger
     << "inertial length(e-) = " << le/1e3 << " km = " << le/Hybrid::dx << " dx" << endl;
   for (size_t s=0;s<Hybrid::swPops.size();++s) {
      Real rLth = 0.0; // thermal larmor radius
      if(Btot > 0.0) {
         rLth = Hybrid::swPops[s].m*Hybrid::swPops[s].vth/(Hybrid::swPops[s].q*Btot);
      }
      simClasses.logger
        << "thermal Larmor radius(" << Hybrid::swPops[s].name << ") = " << rLth/1e3 << " km = " << rLth/Hybrid::dx << " dx" << endl;
   }
   simClasses.logger
     << "==== ALL POPULATIONS AS PICKUP IONS ====" << endl;
   for(size_t s=0;s<particleLists.size();++s) {
      const Species* species = reinterpret_cast<const Species*>(particleLists[s]->getSpecies());
      Real tL = 0.0; // larmor period
      if(Btot > 0.0) {
         tL = 2.0*M_PI*species->m/(species->q*Btot);
      }
      simClasses.logger
        << "Larmor period(" << species->name << ") = " << tL << " s = " << tL/sim.dt << " dt" << endl;
   }
   Real tLe = 0.0;
   if(Btot > 0.0) {
      tLe = 2.0*M_PI*constants::MASS_ELECTRON/(constants::CHARGE_ELEMENTARY*Btot);
   }
   simClasses.logger
     << "Larmor period(e-) = " << tLe << " s = " << tLe/sim.dt << " dt" << endl;
   for(size_t s=0;s<particleLists.size();++s) {
      const Species* species = reinterpret_cast<const Species*>(particleLists[s]->getSpecies());
      Real rL = 0.0; // larmor radius
      if(Btot > 0.0) {
         rL = species->m*VExBMagnitude/(species->q*Btot);
      }
      simClasses.logger
        << "Larmor radius(" << species->name << ") = " << rL/1e3 << " km = " << rL/Hybrid::dx << " dx" << endl;
   }
   Real rLe = 0.0;
   if(Btot > 0.0) {
      rLe = constants::MASS_ELECTRON*VExBMagnitude/(constants::CHARGE_ELEMENTARY*Btot);
   }
   simClasses.logger
     << "Larmor radius(e-) = " << rLe/1e3 << " km = " << rLe/Hybrid::dx << " dx" << endl;
   simClasses.logger << endl;
   
   // write log entry of output configs
   simClasses.logger << "(HYBRID) Particle population output configurations" << endl;
   for(unsigned int i=0;i<Hybrid::N_outputPopVars;++i) {
      simClasses.logger << Hybrid::outputPopVarStr[i] << ": ";
      for(unsigned int j=0;j<Hybrid::outputPopVarIdVector[i].size();++j) {
         simClasses.logger << Hybrid::populationNames[Hybrid::outputPopVarIdVector[i][j]] << " ";
      }
      simClasses.logger << endl;
   }
   simClasses.logger << "-: ";
   for(unsigned int i=0;i<Hybrid::outputPopVarId.size();++i) {
      if(Hybrid::outputPopVarId[i] < 0) {
         simClasses.logger << Hybrid::populationNames[i] << " ";
      }
   }
   simClasses.logger << endl;
   simClasses.logger << "tot plasma (snapshot): ";
   for(unsigned int i=0;i<Hybrid::outputPlasmaPopId.size();++i) {
      simClasses.logger << Hybrid::populationNames[Hybrid::outputPlasmaPopId[i]] << " ";
   }
   simClasses.logger << endl;
   simClasses.logger << "tot plasma (average): ";
   for(unsigned int i=0;i<Hybrid::N_outputPopVars;++i) {
      simClasses.logger << Hybrid::outputPopVarStr[i] << " ";
   }
   simClasses.logger << endl << write;

   // open log files
   if(sim.mpiRank==sim.MASTER_RANK) {
      for(size_t s=0;s<particleLists.size();++s) {
	 string zeroStr = "";
	 if(s < 10) { zeroStr = "00"; }
	 else if(s < 100) { zeroStr = "0"; }
	 const Species* species = reinterpret_cast<const Species*>(particleLists[s]->getSpecies());
	 Hybrid::plog.push_back(new ofstream());
	 //string fileName = "pop" + zeroStr + "_" + species->name + ".log";
	 stringstream ss;
	 ss << "pop" << zeroStr << s+1 << "_" << species->name << ".log";
	 Hybrid::plog[s]->open(ss.str().c_str(),ios_base::out);
	 //Hybrid::plog[s]->open("pop" + zeroStr + to_string(s+1) + "_" + species->name + ".log",ios_base::out);
	 Hybrid::plog[s]->precision(10);
	 (*Hybrid::plog[s]) << scientific << showpos;
	 (*Hybrid::plog[s])
	   << "% " << species->name << endl
	   << "% m [kg] = " << species->m << endl
	   << "% q [C] = " << species->q << endl
	   << "% columns = 11" << endl
	   << "% 01. Time [s]" << endl
	   << "% 02. Particles [#]" << endl
	   << "% 03. Macroparticles [#]" << endl
	   << "% 04. avg(Vx) [m/s]" << endl
	   << "% 05. avg(Vy) [m/s]" << endl
	   << "% 06. avg(Vz) [m/s]" << endl
	   << "% 07. avg(|V|) [m/s]" << endl
	   << "% 08. Kinetic energy [J]" << endl
	   << "% 09. Escape rate [#/s]" << endl
	   << "% 10. Impact rate [#/s]" << endl
	   << "% 11. Inject rate [#/s]" << endl
	   << "% 12. Macroparticle inject rate [#/dt]" << endl;
      }
      Hybrid::flog.open("field.log",ios_base::out);
      Hybrid::flog.precision(10);
      Hybrid::flog << scientific << showpos;
      Hybrid::flog
	<< "% field" << endl
	<< "% columns = 10" << endl
	<< "% 01. Time [s]" << endl
	<< "% 02. avg(Bx) [T]" << endl
	<< "% 03. avg(By) [T]" << endl
	<< "% 04. avg(Bz) [T]" << endl
	<< "% 05. avg(|B|) [T]" << endl
	<< "% 06. max(|B|) [T]" << endl
	<< "% 07. avg(div(B)) [T/m]" << endl
	<< "% 08. max(div(B)) [T/m]" << endl
	<< "% 09. max(dx*div(B)/B) [-]" << endl
	<< "% 10. energy(sum(dV*B^2/2*mu0)) [J]" << endl;
   }

   // counters
   for(size_t s=0;s<particleLists.size();++s) {
      Hybrid::particleCounterEscape.push_back(0.0);
      Hybrid::particleCounterImpact.push_back(0.0);
      Hybrid::particleCounterInject.push_back(0.0);
      Hybrid::particleCounterInjectMacroparticles.push_back(0.0);
      Hybrid::particleCounterTimeStart = sim.t;
   }
   
#ifdef WRITE_POPULATION_AVERAGES
   // magnetic field
   Hybrid::dataCellAverageBID  = simClasses.pargrid.invalidDataID();
   Hybrid::dataCellAverageBID = simClasses.pargrid.addUserData<Real>("cellAverageB",block::SIZE*3);
   if(Hybrid::dataCellAverageBID == simClasses.pargrid.invalidCellID()) {
      simClasses.logger << "(USER) ERROR: Failed to add cellAverageB array to ParGrid!" << endl << write;
      return false;
   }
   if(simClasses.pargrid.addDataTransfer(Hybrid::dataCellAverageBID,pargrid::DEFAULT_STENCIL) == false) {
      simClasses.logger << "(USER) ERROR: Failed to add cellAverageB data transfer!" << endl << write; return false;
   }
   Real* cellAverageB = reinterpret_cast<Real*>(simClasses.pargrid.getUserData(Hybrid::dataCellAverageBID));
   // particle populations
   vector<Real*> nAve;
   vector<Real*> vAve;
   for(unsigned int i=0;i<Hybrid::N_outputPopVars;++i) {
      Hybrid::dataCellAverageDensityID.push_back(simClasses.pargrid.invalidDataID());
      Hybrid::dataCellAverageVelocityID.push_back(simClasses.pargrid.invalidDataID());
      Hybrid::dataCellAverageDensityID[i] = simClasses.pargrid.addUserData<Real>("cellDensityAverage_pop" + to_string(i),block::SIZE);
      if(Hybrid::dataCellAverageDensityID[i] == simClasses.pargrid.invalidCellID()) {
         simClasses.logger << "(USER) ERROR: Failed to add cellDensityAverage_pop" + to_string(i) + " array to ParGrid!" << endl << write;
         return false;
      }
      Hybrid::dataCellAverageVelocityID[i] = simClasses.pargrid.addUserData<Real>("cellVelocityAverage_pop" + to_string(i),block::SIZE*3);
      if(Hybrid::dataCellAverageVelocityID[i] == simClasses.pargrid.invalidCellID()) {
         simClasses.logger << "(USER) ERROR: Failed to add cellVelocityAverage_pop" + to_string(i) + " array to ParGrid!" << endl << write;
         return false;
      }
      if(simClasses.pargrid.addDataTransfer(Hybrid::dataCellAverageDensityID[i],pargrid::DEFAULT_STENCIL) == false) {
         simClasses.logger << "(USER) ERROR: Failed to add cellDensityAverage_pop" + to_string(i) + " data transfer 1!" << endl << write; return false;
      }
      if(simClasses.pargrid.addDataTransfer(Hybrid::dataCellAverageDensityID[i],Hybrid::accumulationStencilID) == false) {
         simClasses.logger << "(USER) ERROR: Failed to add cellDensityAverage_pop" + to_string(i) + " data transfer 2!" << endl << write; return false;
      }
      if(simClasses.pargrid.addDataTransfer(Hybrid::dataCellAverageVelocityID[i],pargrid::DEFAULT_STENCIL) == false) {
         simClasses.logger << "(USER) ERROR: Failed to add cellVelocityAverage_pop" + to_string(i) + " data transfer 1!" << endl << write; return false;
      }
      if(simClasses.pargrid.addDataTransfer(Hybrid::dataCellAverageVelocityID[i],Hybrid::accumulationStencilID) == false) {
         simClasses.logger << "(USER) ERROR: Failed to add cellVelocityAverage_pop" + to_string(i) + " data transfer 2!" << endl << write; return false;
      }
      nAve.push_back(reinterpret_cast<Real*>(simClasses.pargrid.getUserData(Hybrid::dataCellAverageDensityID[i])));
      vAve.push_back(reinterpret_cast<Real*>(simClasses.pargrid.getUserData(Hybrid::dataCellAverageVelocityID[i])));
   }
   // initial values
   if(sim.restarted == false) {
      for(size_t i=0; i<vectorArraySize;   ++i) { cellAverageB[i] = 0.0; }
      for(size_t i=0;i<Hybrid::N_outputPopVars;++i) {
         for(size_t j=0; j<scalarArraySize;++j) { nAve[i][j] = 0.0; }
         for(size_t j=0; j<vectorArraySize;++j) { vAve[i][j] = 0.0; }
      }
      Hybrid::averageCounter = 0;
   }
#endif
   return true;
}

/** Finalization function that should deallocate or finalize memory that 
 * has been allocated by userEarlyInitialization or userLateInitialization functions.
 * @return If true, finalization completed successfully.*/
bool userFinalization(Simulation& sim,SimulationClasses& simClasses,vector<ParticleListBase*>& particleLists) {
   bool success = true;
   if(simClasses.pargrid.removeUserData(Hybrid::dataFaceBID)               == false) { success = false; }
   if(simClasses.pargrid.removeUserData(Hybrid::dataFaceJID)               == false) { success = false; }
   if(simClasses.pargrid.removeUserData(Hybrid::dataCellRhoQiID)           == false) { success = false; }
   if(simClasses.pargrid.removeUserData(Hybrid::dataCellBID)               == false) { success = false; }
   if(simClasses.pargrid.removeUserData(Hybrid::dataCellJID)               == false) { success = false; }
   if(simClasses.pargrid.removeUserData(Hybrid::dataCellUeID)              == false) { success = false; }
   if(simClasses.pargrid.removeUserData(Hybrid::dataCellJiID)              == false) { success = false; }
   if(simClasses.pargrid.removeUserData(Hybrid::dataCellIonosphereID)      == false) { success = false; }
   if(simClasses.pargrid.removeUserData(Hybrid::dataCellExosphereID)       == false) { success = false; }
   if(simClasses.pargrid.removeUserData(Hybrid::dataNodeRhoQiID)           == false) { success = false; }
   if(simClasses.pargrid.removeUserData(Hybrid::dataNodeEID)               == false) { success = false; }
   if(simClasses.pargrid.removeUserData(Hybrid::dataNodeBID)               == false) { success = false; }
   if(simClasses.pargrid.removeUserData(Hybrid::dataNodeJID)               == false) { success = false; }
   if(simClasses.pargrid.removeUserData(Hybrid::dataNodeUeID)              == false) { success = false; }
   if(simClasses.pargrid.removeUserData(Hybrid::dataNodeJiID)              == false) { success = false; }
#ifdef USE_RESISTIVITY
   if(simClasses.pargrid.removeUserData(Hybrid::dataNodeEtaID)             == false) { success = false; }
#endif
   if(simClasses.pargrid.removeUserData(Hybrid::dataCounterCellMaxUeID)    == false) { success = false; }
   if(simClasses.pargrid.removeUserData(Hybrid::dataCounterCellMaxViID)    == false) { success = false; }
   if(simClasses.pargrid.removeUserData(Hybrid::dataCounterCellMinRhoQiID) == false) { success = false; }
#ifdef USE_ECUT
   if(simClasses.pargrid.removeUserData(Hybrid::dataCounterNodeEcutID)     == false) { success = false; }
#endif
   if(simClasses.pargrid.removeUserData(Hybrid::dataInnerFlagFieldID)      == false) { success = false; }
   if(simClasses.pargrid.removeUserData(Hybrid::dataInnerFlagParticleID)   == false) { success = false; }
#ifdef USE_XMIN_BOUNDARY
   if(simClasses.pargrid.removeUserData(Hybrid::dataXminFlagID)            == false) { success = false; }
#endif
#ifdef ION_SPECTRA_ALONG_ORBIT
   //if(simClasses.pargrid.removeUserData(Hybrid::dataSpectraID            == false) { success = false; }
   if(simClasses.pargrid.removeUserData(Hybrid::dataSpectraFlagID)         == false) { success = false; }
#endif
#ifdef WRITE_POPULATION_AVERAGES
   if(simClasses.pargrid.removeUserData(Hybrid::dataCellAverageBID)        == false) { success = false; }
   for(size_t i=0;i<Hybrid::N_outputPopVars;++i) {
      if(simClasses.pargrid.removeUserData(Hybrid::dataCellAverageDensityID[i])  == false) { success = false; }
      if(simClasses.pargrid.removeUserData(Hybrid::dataCellAverageVelocityID[i]) == false) { success = false; }
   }
#endif
   // close log files
   if(sim.mpiRank==sim.MASTER_RANK) {
      for(size_t i=0;i<Hybrid::plog.size();++i) {
	 Hybrid::plog[i]->flush();
	 Hybrid::plog[i]->close();
	 delete Hybrid::plog[i];
      }
      Hybrid::plog.clear();
      Hybrid::flog.flush();
      Hybrid::flog.close();
   }
   return success;
}

bool userRunTests(Simulation& sim,SimulationClasses& simClasses,std::vector<ParticleListBase*>& particleLists) {
   return true;
}
