#include "pcAdapter.h"
#include "pcError.h"
#include "pcUpdateMesh.h"
#include "pcSmooth.h"
#include "pcWriteFiles.h"
#include <SimUtil.h>
#include <SimPartitionedMesh.h>
#include <SimDiscrete.h>
#include "SimMeshMove.h"
#include "SimMeshTools.h"
#include <SimAdvMeshing.h>
#include "MeshSimAdapt.h"

#include "apfSIM.h"
#include "gmi_sim.h"
#include <PCU.h>
#include <cassert>
#include <phastaChef.h>
#include <maStats.h>
#include <apfShape.h>
#include <math.h>
#include <ctime>

extern void MSA_setBLSnapping(pMSAdapt, int onoff);
extern void MSA_setAdaptExtrusion(pMSAdapt, int onoff);

namespace pc {
 
  apf::Field* convertField(apf::Mesh* m,
    const char* inFieldname,
    const char* outFieldname) {
    apf::Field* inf = m->findField(inFieldname);
    assert(inf);
    int size = apf::countComponents(inf);
    apf::Field* outf = m->findField(outFieldname);
    if (outf)
      apf::destroyField(outf);
    outf = apf::createPackedField(m, outFieldname, size);
    apf::NewArray<double> inVal(size);
    apf::NewArray<double> outVal(size);
    apf::MeshEntity* vtx;
    apf::MeshIterator* it = m->begin(0);
    while ((vtx = m->iterate(it))) {
      apf::getComponents(inf, vtx, 0, &inVal[0]);
      for (int i = 0; i < size; i++){
        outVal[i] = inVal[i];
      }
      apf::setComponents(outf,vtx, 0, &outVal[0]);
    }
    m->end(it);
    apf::destroyField(inf);
    return outf;
  }

  apf::Field* convertVtxFieldToElm(apf::Mesh* m,
                  const char* vFieldname,
                  const char* eFieldname) {
    apf::Field* vf = m->findField(vFieldname);
    assert(vf);
    int size = apf::countComponents(vf);
    apf::Field* ef = m->findField(eFieldname);
    if (ef) apf::destroyField(ef);
    ef = apf::createPackedField(m, eFieldname, size, apf::getConstant(m->getDimension()));
    apf::NewArray<double> vVal(size);
    apf::NewArray<double> eVal(size);
    apf::MeshEntity* elm;
    apf::MeshIterator* it = m->begin(m->getDimension());
    while ((elm = m->iterate(it))) {
      for (int i = 0; i < size; i++) eVal[i] = 0.0;
      apf::Downward vtx;
      int nbv = m->getDownward(elm, 0, vtx);
      for (int j = 0; j < nbv; j++){
        apf::getComponents(vf, vtx[j], 0, &vVal[0]);
        for (int i = 0; i < size; i++){
          eVal[i] += vVal[i]/(double)nbv;
        }
      }
      apf::setComponents(ef, elm, 0, &eVal[0]);
    }
    m->end(it);
    apf::destroyField(vf);
    return ef;
  }

  void attachMeshSizeField(apf::Mesh2*& m, ph::Input& in, phSolver::Input& inp) {
    /* create a field to store mesh size */
    if(m->findField("sizes")) apf::destroyField(m->findField("sizes"));
    apf::Field* sizes = apf::createSIMFieldOn(m, "sizes", apf::VECTOR);
    /* switch between VMS error mesh size and initial mesh size */
    if((string)inp.GetValue("Error Estimation Option") != "False") {
      pc::attachVMSSizeField(m, in, inp);
    }
    else {
      if(m->findField("frames")) apf::destroyField(m->findField("frames"));
      apf::Field* frames = apf::createSIMFieldOn(m, "frames", apf::MATRIX);
      ph::attachSIMSizeField(m, sizes, frames);
    }
  }

  int getNumOfMappedFields(apf::Mesh2*& m) {
    /* initially, we have 7 fields: pressure, velocity, temperature,
       time der of pressure, time der of velocity, time der of temperature,
       ,mesh velocity and 1 optional field: time resource bound factor field */
    int numOfMappedFields;
    if (m->findField("ctcn_elm")) numOfMappedFields = 8;
    else numOfMappedFields = 7;
    return numOfMappedFields;
  }

  /* remove all fields except for solution, time
     derivative of solution, mesh velocity and keep
     certain field if corresponding option is on */
  void removeOtherFields(apf::Mesh2*& m, phSolver::Input& inp) {
    int index = 0;
    int numOfPackFields = 4;
    while (m->countFields() > numOfPackFields) {
      apf::Field* f = m->getField(index);
      if ( f == m->findField("solution") ||
           f == m->findField("time derivative of solution") ||
           f == m->findField("mesh_vel") ||
           f == m->findField("ctcn_elm") ) {
        index++;
        continue;
      }
      m->removeField(f);
      apf::destroyField(f);
    }
    m->verify();
  }

  int getSimFields(apf::Mesh2*& m, int simFlag, pField* sim_flds, phSolver::Input& inp) {
    int num_flds = 0;
    if (m->findField("solution")) {
      num_flds += 3;
      sim_flds[0] = apf::getSIMField(chef::extractField(m,"solution","pressure",1,apf::SCALAR,simFlag));
      sim_flds[1] = apf::getSIMField(chef::extractField(m,"solution","velocity",2,apf::VECTOR,simFlag));
      sim_flds[2] = apf::getSIMField(chef::extractField(m,"solution","temperature",5,apf::SCALAR,simFlag));
      apf::destroyField(m->findField("solution"));
    }

    if (m->findField("time derivative of solution")) {
      num_flds += 3;
      sim_flds[3] = apf::getSIMField(chef::extractField(m,"time derivative of solution","der_pressure",1,apf::SCALAR,simFlag));
      sim_flds[4] = apf::getSIMField(chef::extractField(m,"time derivative of solution","der_velocity",2,apf::VECTOR,simFlag));
      sim_flds[5] = apf::getSIMField(chef::extractField(m,"time derivative of solution","der_temperature",5,apf::SCALAR,simFlag));
      apf::destroyField(m->findField("time derivative of solution"));
    }

    if (m->findField("mesh_vel")) {
      num_flds += 1;
      sim_flds[6] = apf::getSIMField(chef::extractField(m,"mesh_vel","mesh_vel_sim",1,apf::VECTOR,simFlag));
      apf::destroyField(m->findField("mesh_vel"));
    }

    if (m->findField("ctcn_elm")) {
      num_flds += 1;
      sim_flds[7] = apf::getSIMField(chef::extractField(m,"ctcn_elm","ctcn_elm_sim",1,apf::SCALAR,simFlag));
      apf::destroyField(m->findField("ctcn_elm"));
    }

    return num_flds;
  }

  /* unpacked solution into serveral fields,
     put these field explicitly into pPList */
  pPList getSimFieldList(ph::Input& in, apf::Mesh2*& m){
    /* load input file for solver */
    phSolver::Input inp("solver.inp", "input.config");
    int num_flds = getNumOfMappedFields(m);
    removeOtherFields(m,inp);
    pField* sim_flds = new pField[num_flds];
    getSimFields(m, in.simmetrixMesh, sim_flds, inp);
    pPList sim_fld_lst = PList_new();
    for (int i = 0; i < num_flds; i++) {
      PList_append(sim_fld_lst, sim_flds[i]);
    }
    assert(num_flds == PList_size(sim_fld_lst));
    delete [] sim_flds;
    return sim_fld_lst;
  }

  void transferSimFields(apf::Mesh2*& m) {
    if (m->findField("pressure")) // assume we had solution before
      chef::combineField(m,"solution","pressure","velocity","temperature");
    if (m->findField("der_pressure")) // assume we had time derivative of solution before
      chef::combineField(m,"time derivative of solution","der_pressure","der_velocity","der_temperature");
    if (m->findField("mesh_vel_sim"))
      convertField(m, "mesh_vel_sim", "mesh_vel");
    if (m->findField("ctcn_elm_sim"))
      convertVtxFieldToElm(m, "ctcn_elm_sim", "err_tri_f");
    // destroy mesh size field
    if(m->findField("sizes"))  apf::destroyField(m->findField("sizes"));
    if(m->findField("frames")) apf::destroyField(m->findField("frames"));
  }

  void attachCurrentSizeField(apf::Mesh2*& m) {
    int  nsd = m->getDimension();
    if(m->findField("cur_size")) apf::destroyField(m->findField("cur_size"));
    apf::Field* cur_size = apf::createField(m, "cur_size", apf::SCALAR, apf::getConstant(nsd));
    // loop over non-BL elements
    apf::MeshEntity* e;
    apf::MeshIterator* eit = m->begin(nsd);
    while ((e = m->iterate(eit))) {
      pRegion meshRegion = reinterpret_cast<pRegion>(e);
      if (EN_isBLEntity(meshRegion)) continue;
      // set mesh size field
      double h = 0.0;
      if (m->getType(e) == apf::Mesh::TET)
        h = apf::computeShortestHeightInTet(m,e) * sqrt(3.0);
      else
        h = pc::getShortestEdgeLength(m,e);
      apf::setScalar(cur_size, e, 0, h);
    }
    m->end(eit);

    // get sim model
    apf::MeshSIM* sim_m = dynamic_cast<apf::MeshSIM*>(m);
    pParMesh sim_pm = sim_m->getMesh();
    pMesh pm = PM_mesh(sim_pm,0);

    gmi_model* gmiModel = sim_m->getModel();
    pGModel model = gmi_export_sim(gmiModel);

    // loop over model faces
    pGFace modelFace;
    pFace meshFace;
    pFace blFace;
    pRegion blRegion;
    FIter fIter;
    pEntity seed;
    pPList growthRegions = PList_new();
    pPList growthFaces = PList_new();
    GFIter gfIter = GM_faceIter(model);
    while((modelFace=GFIter_next(gfIter))){
      // loop over mesh faces on model face
      fIter = M_classifiedFaceIter(pm, modelFace, 1);
      while((meshFace = FIter_next(fIter))){
        apf::MeshEntity* apf_f = reinterpret_cast<apf::MeshEntity*>(meshFace);
        // check if BL base
        if (BL_isBaseEntity(meshFace, modelFace)) {
          // loop over BL regions and layers
          for(int faceSide = 0; faceSide < 2; faceSide++){
            int hasSeed = BL_stackSeedEntity(meshFace, modelFace, faceSide, NULL, &seed);
            if (hasSeed) {
              BL_growthRegionsAndLayerFaces((pRegion)seed, growthRegions, growthFaces, Layer_Entity);
              if (PList_size(growthRegions) >= (PList_size(growthFaces)-1)*3) { // tet
                for(int i = 0; i < PList_size(growthFaces); i++) {
                  blFace = (pFace)PList_item(growthFaces,i);
                  apf::MeshEntity* apf_f = reinterpret_cast<apf::MeshEntity*>(blFace);
                  double h = apf::computeShortestHeightInTri(m,apf_f) * sqrt(2.0);
                  for(int j = 0; j < 3; j++) {
                    if (i*3+j == PList_size(growthRegions)) break;
                    blRegion = (pRegion)PList_item(growthRegions,i*3+j);
                    // set mesh size field
                    apf::MeshEntity* apf_r = reinterpret_cast<apf::MeshEntity*>(blRegion);
                    apf::setScalar(cur_size, apf_r, 0, h);
                  }
                }
              }
              else if (PList_size(growthRegions) >= (PList_size(growthFaces)-1)) { // wedge
                for(int i = 0; i < PList_size(growthFaces); i++) {
                  if (i == PList_size(growthRegions)) break;
                  blFace = (pFace)PList_item(growthFaces,i);
                  apf::MeshEntity* apf_f = reinterpret_cast<apf::MeshEntity*>(blFace);
                  double h = apf::computeShortestHeightInTri(m,apf_f) * sqrt(2.0);
                  blRegion = (pRegion)PList_item(growthRegions,i);
                  // set mesh size field
                  apf::MeshEntity* apf_r = reinterpret_cast<apf::MeshEntity*>(blRegion);
                  apf::setScalar(cur_size, apf_r, 0, h);
                }
              }
            }
            else if (hasSeed < 0) {
              printf("not support blending BL mesh or miss some info!\n");
              exit(0);
            }
          }
        }
      }
      FIter_delete(fIter);
    }
    GFIter_delete(gfIter);
  }

  double estimateAdaptedMeshElements(apf::Mesh2*& m, apf::Field* sizes) {
    attachCurrentSizeField(m);
    apf::Field* cur_size = m->findField("cur_size");
    assert(cur_size);

    double estElm = 0.0;
    apf::Vector3 v_mag  = apf::Vector3(0.0, 0.0, 0.0);
    int num_dims = m->getDimension();
    assert(num_dims == 3); // only work for 3D mesh
    apf::Vector3 xi = apf::Vector3(0.25, 0.25, 0);
    apf::MeshEntity* en;
    apf::MeshIterator* eit = m->begin(num_dims);
    while ((en = m->iterate(eit))) {
      apf::MeshElement* elm = apf::createMeshElement(m,en);
      apf::Element* fd_elm = apf::createElement(sizes,elm);
      apf::getVector(fd_elm,xi,v_mag);
      double h_old = apf::getScalar(cur_size,en,0);
      if(EN_isBLEntity(reinterpret_cast<pEntity>(en))) {
        estElm = estElm + (h_old/v_mag[0])*(h_old/v_mag[0]);
      }
      else {
        estElm = estElm + (h_old/v_mag[0])*(h_old/v_mag[0])*(h_old/v_mag[0]);
      }
    }
    m->end(eit);

    apf::destroyField(cur_size);

    double estTolElm = PCU_Add_Double(estElm);
    return estTolElm;
  }

  void initializeCtCn(apf::Mesh2*& m) {
    apf::Field* ctcn = apf::createSIMFieldOn(m, "ctcn_elm", apf::SCALAR);
    apf::MeshEntity* v;
    apf::MeshIterator* vit = m->begin(0);
    while ((v = m->iterate(vit))) {
      apf::setScalar(ctcn,v,0,1.0);
    }
    m->end(vit);
  }

  void applyMaxSizeBound(apf::Mesh2*& m, apf::Field* sizes, ph::Input& in) {
    apf::Vector3 v_mag = apf::Vector3(0.0,0.0,0.0);
    apf::MeshEntity* v;
    apf::MeshIterator* vit = m->begin(0);
    while ((v = m->iterate(vit))) {
      apf::getVector(sizes,v,0,v_mag);
      for (int i = 0; i < 3; i++)
        if(v_mag[i] > in.simSizeUpperBound)
          v_mag[i] = in.simSizeUpperBound;
      apf::setVector(sizes,v,0,v_mag);
    }
    m->end(vit);
  }

  double applyMaxNumberElement(apf::Mesh2*& m, apf::Field* sizes, ph::Input& in)  {
    /* scale mesh if number of elements exceeds threshold */
    double N_est = estimateAdaptedMeshElements(m, sizes);
    double cn = N_est / (double)in.simMaxAdaptMeshElements;
    cn = (cn>1.0)?cbrt(cn):1.0;
    if(!PCU_Comm_Self())
      printf("Estimated No. of Elm: %f and c_N = %f\n", N_est, cn);
    apf::Field* sol = m->findField("solution");
    apf::Field* ctcn = m->findField("ctcn_elm");
    assert(sol);
    assert(ctcn);
    apf::Vector3 v_mag = apf::Vector3(0.0,0.0,0.0);
    apf::MeshEntity* v;
    apf::MeshIterator* vit = m->begin(0);
    while ((v = m->iterate(vit))) {
      apf::getVector(sizes,v,0,v_mag);
      for (int i = 0; i < 3; i++)
        v_mag[i] = v_mag[i] * cn;
      apf::setVector(sizes,v,0,v_mag);

      double f = apf::getScalar(ctcn,v,0);
      f = f * cn;
      apf::setScalar(ctcn,v,0,f);
    }
    m->end(vit);
    return cn;
  }

  void applyMaxTimeResource(apf::Mesh2*& m, apf::Field* sizes,
                            ph::Input& in, phSolver::Input& inp) {
    apf::Field* sol = m->findField("solution");
    apf::Field* ctcn = m->findField("ctcn_elm");
    assert(sol);
    assert(ctcn);
    apf::Vector3 v_mag = apf::Vector3(0.0,0.0,0.0);
    apf::NewArray<double> s(in.ensa_dof);
    double maxCt = 1.0;
    double minCtH = 1.0e16;
    apf::MeshEntity* v;
    apf::MeshIterator* vit = m->begin(0);
    while ((v = m->iterate(vit))) {
      apf::getComponents(sol, v, 0, &s[0]);
      double u = sqrt(s[1]*s[1]+s[2]*s[2]+s[3]*s[3]);
      double c = sqrt(1.4*8.3145*s[4]/0.029); // ideal air assumed here
      double t = inp.GetValue("Time Step Size");
      double h_min = (u+c)*t/in.simCFLUpperBound;
      if (h_min < in.simSizeLowerBound) h_min = in.simSizeLowerBound;
      apf::getVector(sizes,v,0,v_mag);
      double f = apf::getScalar(ctcn,v,0);
      for (int i = 0; i < 3; i++) {
        if(v_mag[i] < h_min) {
          if(h_min/(v_mag[i]) > maxCt) maxCt = h_min/(v_mag[i]);
          apf::setScalar(ctcn,v,0,h_min/v_mag[i]*f);
          if(h_min < minCtH) minCtH = h_min;
          v_mag[i] = h_min;
        }
      }
      apf::setVector(sizes,v,0,v_mag);
    }
    m->end(vit);

    double maxCtAll  = PCU_Max_Double(maxCt);
    double minCtHAll = PCU_Min_Double(minCtH);
    if (!PCU_Comm_Self())
      printf("max time resource bound factor and min reached size: %f and %f\n",maxCtAll,minCtHAll);
  }

  void syncMeshSize(apf::Mesh2*& m, apf::Field* sizes) {
    PCU_Comm_Begin();
    apf::Copies remotes;
    apf::Vector3 v_mag = apf::Vector3(0.0,0.0,0.0);
    apf::MeshEntity* v;
    apf::MeshIterator* vit = m->begin(0);
    while ((v = m->iterate(vit))) {
      apf::getVector(sizes,v,0,v_mag);
      if(m->isShared(v)) {
        m->getRemotes(v, remotes);
        APF_ITERATE(apf::Copies, remotes, rit) {
          PCU_COMM_PACK(rit->first, rit->second);
          PCU_Comm_Pack(rit->first, &(v_mag[0]), sizeof(double));
        }
      }
    }
    m->end(vit);

    PCU_Comm_Send();
    while (PCU_Comm_Receive()) {
      apf::MeshEntity* rv;
      PCU_COMM_UNPACK(rv);
      double rv_mag;
      PCU_Comm_Unpack(&(rv_mag), sizeof(double));
      apf::getVector(sizes,v,0,v_mag);
      if(rv_mag < v_mag[0]) { // smaller wins
        v_mag = apf::Vector3(rv_mag,rv_mag,rv_mag);
        apf::setVector(sizes,rv,0,v_mag);
      }
    }
  }

  void setupSimImprover(pVolumeMeshImprover vmi, pPList sim_fld_lst) {
    VolumeMeshImprover_setModifyBL(vmi, 1);
    VolumeMeshImprover_setShapeMetric(vmi, ShapeMetricType_VolLenRatio, 0.3);
    VolumeMeshImprover_setSmoothType(vmi, 1); // 0:Laplacian-based; 1:Gradient-based

    /* set fields to be mapped */
    if (PList_size(sim_fld_lst))
      VolumeMeshImprover_setMapFields(vmi, sim_fld_lst);
  }

  void setupSimAdapter(pMSAdapt adapter, ph::Input& in, apf::Mesh2*& m, pPList& sim_fld_lst) {
    MSA_setAdaptBL(adapter, 1);
    MSA_setExposedBLBehavior(adapter,BL_DisallowExposed);
    MSA_setBLSnapping(adapter, 0); // currently needed for parametric model
    MSA_setAdaptExtrusion(adapter, 1);
    MSA_setBLMinLayerAspectRatio(adapter, 0.0); // needed in parallel
    MSA_setSizeGradation(adapter, 1, 0.0);
    /* attach mesh size field */
    phSolver::Input inp("solver.inp", "input.config");
    attachMeshSizeField(m, in, inp);
    apf::Field* sizes = m->findField("sizes");
    assert(sizes);

    /* initial ctcn field */
    pc::initializeCtCn(m);

    /* apply upper bound */
    pc::applyMaxSizeBound(m, sizes, in);

    /* apply max number of element */
    double cn = pc::applyMaxNumberElement(m, sizes, in);

    /* scale mesh if reach time resource bound */
    pc::applyMaxTimeResource(m, sizes, in, inp);

    /* apply upper bound */
    pc::applyMaxSizeBound(m, sizes, in);

    /* add mesh smooth/gradation function here */
    pc::addSmoother(m, in.gradingFactor);

    /* sync mesh size over partitions */
//    pc::syncMeshSize(m, sizes);

    /* use current size field */
    if(!PCU_Comm_Self())
      printf("Start mesh adapt of setting size field\n");

    apf::Vector3 v_mag = apf::Vector3(0.0,0.0,0.0);
    apf::MeshEntity* v;
    double xyz[3];
    apf::MeshIterator* vit = m->begin(0);
    while ((v = m->iterate(vit))) {
      apf::getVector(sizes,v,0,v_mag);
      pVertex meshVertex = reinterpret_cast<pVertex>(v);
//      SA_setVertexSize(adapter, meshVertex, v_mag[0]);
      V_coord(meshVertex, xyz);
      if(xyz[0]>= 0.051 && xyz[0] <=0.088 ){     
        MSA_setVertexSize(adapter, meshVertex, 0.0005);
//      }else if(xyz[0]>= 0.0473 && xyz[0] <0.051 || xyz[0]> 0.088 && xyz[0] < 0.0917){
//        MSA_setVertexSize(adapter, meshVertex, 0.001);
//      }else if(xyz[0]>= 0.0436 && xyz[0] <0.0473 || xyz[0]> 0.0917 && xyz[0] < 0.0954){
//        MSA_setVertexSize(adapter, meshVertex, 0.002);
//      }else if(xyz[0]>= 0 && xyz[0]<0.051 || xyz[0]> 0.088 ){
//        MSA_setVertexSize(adapter, meshVertex, 0.001);
      } else if (xyz[0] < 0 ){
	MSA_setVertexSize(adapter, meshVertex, 0.002);
      }else{
	MSA_setVertexSize(adapter, meshVertex, 0.001); 
      }
    }
    m->end(vit);

    if(!PCU_Comm_Self())
      printf("Size field hacked\n");
    /* write error and mesh size */
    pc::writeSequence(m, in.timeStepNumber, "error_mesh_size_");

    /* set fields to be mapped */
    PList_clear(sim_fld_lst);
    if (in.solutionMigration) {
      sim_fld_lst = getSimFieldList(in, m);
      MSA_setMapFields(adapter, sim_fld_lst);
    }
  }

  void runMeshAdapter(ph::Input& in, apf::Mesh2*& m, apf::Field*& orgSF, int step) {
    /* use the size field of the mesh before mesh motion */
    apf::Field* szFld = orgSF;
    void MSA_setAdaptExtrusion(pMSAdapt, int);

    if(in.simmetrixMesh == 1) {
      if (in.writeSimLog)
        Sim_logOn("sim_mesh_adaptation.log");
      pProgress progress = Progress_new();
      Progress_setDefaultCallback(progress);

      apf::MeshSIM* sim_m = dynamic_cast<apf::MeshSIM*>(m);
      pParMesh sim_pm = sim_m->getMesh();
      pMesh pm = PM_mesh(sim_pm,0);

      gmi_model* gmiModel = sim_m->getModel();
      pGModel model = gmi_export_sim(gmiModel);

      // declaration
      VIter vIter;
      pVertex meshVertex;

      /* create the Simmetrix adapter */
      if(!PCU_Comm_Self())
        printf("Start mesh adapt\n");
      pMSAdapt adapter = MSA_new(sim_pm, 1);
      pPList sim_fld_lst = PList_new();
      setupSimAdapter(adapter, in, m, sim_fld_lst);
  
//      while(meshVertex = VIter_next(vIter)){
 //    MSA_scaleVertexSize(msa, meshVertex, 0.5);        
 //       MSA_setVertexSize(adapter, meshVertex, 0.0012);
//      } 
      VIter_delete(vIter);



      /* run the adapter */
      if(!PCU_Comm_Self())
        printf("do real mesh adapt\n");
      MSA_adapt(adapter, progress);
      MSA_delete(adapter);


      time_t now = time(0);
      char* dt = ctime(&now);
//      std::cout << "The local date and time is: " << dt << endl;

      /* create Simmetrix improver */
      pVolumeMeshImprover vmi = VolumeMeshImprover_new(sim_pm);
      setupSimImprover(vmi, sim_fld_lst);

      /* run the improver */
      VolumeMeshImprover_execute(vmi, progress);
      VolumeMeshImprover_delete(vmi);

      PList_clear(sim_fld_lst);
      PList_delete(sim_fld_lst);

      /* load balance */
      pc::balanceEqualWeights(sim_pm, progress);

      /* write mesh */
      if(!PCU_Comm_Self())
        printf("write mesh after mesh adaptation\n");
      writeSIMMesh(sim_pm, in.timeStepNumber, "sim_mesh_");
      Progress_delete(progress);

      /* transfer data back to apf */
      if (in.solutionMigration)
        transferSimFields(m);
    }
    else {
      assert(szFld);
      apf::synchronize(szFld);
      apf::synchronize(m->getCoordinateField());
      /* do SCOREC mesh adaptation */
      chef::adapt(m,szFld,in);
      chef::balance(in,m);
    }
    m->verify();
  }

}
