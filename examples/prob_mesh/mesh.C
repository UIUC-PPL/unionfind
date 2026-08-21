#include <iostream>
#include "unionFindLib.h"
#include "mesh.decl.h"

/*readonly*/ CProxy_UnionFindLib libProxy;
/*readonly*/ CProxy_Main mainProxy;
/*readonly*/ int MESH_SIZE;
/*readonly*/ int MESHPIECE_SIZE;
/*readonly*/ float PROBABILITY;

class Main : public CBase_Main {
    CProxy_MeshPiece mpProxy;
    double start_time;
    double componentDetectionStartTime;

    public:
    Main(CkArgMsg *m) {
        if (m->argc != 4) {
            CkPrintf("Usage: ./mesh <mesh_size> <mesh_piece_size> <probability>");
            CkExit();
        }

        MESH_SIZE = atoi(m->argv[1]);
        MESHPIECE_SIZE = atoi(m->argv[2]);
        if (MESH_SIZE % MESHPIECE_SIZE != 0)
            CkAbort("Invalid input: Mesh piece size must divide the mesh size!\n");
        PROBABILITY = atof(m->argv[3]);

        if (MESH_SIZE % MESHPIECE_SIZE != 0) {
            CkAbort("Mesh piece size should divide mesh size\n");
        }

        int numMeshPieces = (MESH_SIZE/MESHPIECE_SIZE) * (MESH_SIZE/MESHPIECE_SIZE);
        mpProxy = CProxy_MeshPiece::ckNew(numMeshPieces);
        // callback for library to return to after inverted tree construction
        CkCallback cb(CkIndex_Main::doneInvertedTree(), thisProxy);
        libProxy = UnionFindLib::unionFindInit(mpProxy, numMeshPieces);
        CkPrintf("[Main] Library array with %d chares created and proxy obtained\n", numMeshPieces);
        libProxy[0].register_phase_one_cb(cb);
        start_time = CkWallTimer();
        mpProxy.initializeLibVertices();
    }

    void doneInvertedTree() {
        CkPrintf("[Main] Inverted trees constructed. Notify library to do component detection\n");
        CkPrintf("[Main] Tree construction time: %f\n", CkWallTimer()-start_time);
        componentDetectionStartTime = CkWallTimer();
       /* // ask the lib group chares to contribute counts
        CProxy_UnionFindLibGroup libGroup(libGroupID);
        libGroup.contribute_count();*/
        CkCallback cb(CkIndex_Main::doneFindComponents(), thisProxy);
        libProxy.find_components(cb);
    }

    void doneFindComponents() {
        CkPrintf("[Main] Components identified, prune unecessary ones now\n");
        CkPrintf("[Main] Components detection time: %f\n", CkWallTimer()-componentDetectionStartTime);
        // callback for library to report to after pruning
        CkExit();
        //CkCallback cb(CkIndex_MeshPiece::printVertices(), mpProxy);
        //libProxy.prune_components(1, cb);
    }

    void donePrinting() {
        CkPrintf("[Main] Final runtime: %f\n", CkWallTimer()-start_time);
        CkExit();
    }
};

class MeshPiece : public CBase_MeshPiece {
    struct meshVertex {
        int x,y;
        int id;
        int data = -1;
    };
    
    meshVertex *myVertices;
    int numMyVertices;
    UnionFindLib *libPtr;
    unionFindVertex *libVertices;

    public:
    MeshPiece() {
        myVertices = new meshVertex[MESHPIECE_SIZE*MESHPIECE_SIZE];
        numMyVertices = MESHPIECE_SIZE*MESHPIECE_SIZE;
        libVertices = new unionFindVertex[MESHPIECE_SIZE*MESHPIECE_SIZE];

        //conversion of thisIndex to 2D array indices
        int chare_x = thisIndex / (MESH_SIZE/MESHPIECE_SIZE);
        int chare_y = thisIndex % (MESH_SIZE/MESHPIECE_SIZE);

        // populate myVertices and libVertices
        for (int i = 0; i < MESHPIECE_SIZE; i++) {
            for (int j = 0; j < MESHPIECE_SIZE; j++) {
                // i & j are local x & local y for the vertices here
                int global_x = chare_x*MESHPIECE_SIZE + i;
                int global_y = chare_y*MESHPIECE_SIZE + j;
                myVertices[i*MESHPIECE_SIZE+j].x = global_x;
                myVertices[i*MESHPIECE_SIZE+j].y = global_y;
                myVertices[i*MESHPIECE_SIZE+j].id = global_x*MESH_SIZE + global_y;

                // convert global x & y to unique id for libVertices
                libVertices[i*MESHPIECE_SIZE+j].vertexID = global_x*MESH_SIZE + global_y;
#ifndef ANCHOR_ALGO
                libVertices[i*MESHPIECE_SIZE+j].parent = -1;
#else
                libVertices[i*MESHPIECE_SIZE+j].parent = libVertices[i*MESHPIECE_SIZE+j].vertexID;
#endif
            }
        }

    }

    MeshPiece(CkMigrateMessage *m) { }

    // function needed by library for quick lookup of
    // vertices location
    // uint64_t vertex IDs since the FoF-campaign API change (types.h)
    static std::pair<int,uint64_t> getLocationFromID(uint64_t vid);
        
    void initializeLibVertices() {    
        libPtr = libProxy[thisIndex].ckLocal();
        libPtr->initialize_vertices(libVertices, MESHPIECE_SIZE*MESHPIECE_SIZE);
        libPtr->registerGetLocationFromID(getLocationFromID);
        contribute(CkCallback(CkReductionTarget(MeshPiece, doWork), thisProxy));
    }

    void doWork() {
        for (int i = 0; i < numMyVertices; i++) {
            // check probability for east edge
            float eastProb = 0.0;
            if (myVertices[i].y + 1 < MESH_SIZE) {
                eastProb = checkProbabilityEast(myVertices[i].y, myVertices[i].y+1);

                if (eastProb < PROBABILITY) {
                    // edge found, make library union_request call
                    int eastID = (myVertices[i].x*MESH_SIZE) + (myVertices[i].y+1);
                    libPtr->union_request(myVertices[i].id, eastID);
                }
            }

            // check probability for south edge
            float southProb = 0.0;
            if (myVertices[i].x + 1 < MESH_SIZE) {
                southProb = checkProbabilitySouth(myVertices[i].x, myVertices[i].x+1);

                if (southProb < PROBABILITY) {
                    // edge found, make library union_request call
                    int southID = (myVertices[i].x+1)*MESH_SIZE + myVertices[i].y;
                    libPtr->union_request(myVertices[i].id, southID);
                }
            }
        }
    }

    float checkProbabilityEast(int val1, int val2) {
        float t = ((132967*val1) + (969407*val2)) % 100;
        return t/100;
    }

    float checkProbabilitySouth(int val1, int val2) {
        float t = ((379721*val1) + (523927*val2)) % 100;
        return t/100;
    }

    void printVertices() {
        for (int i = 0; i < numMyVertices; i++) {
            //CkPrintf("[mpProxy %d] libVertices[%d] - vertexID: %ld, parent: %ld, component: %d\n", thisIndex, i, libVertices[i].vertexID, libVertices[i].parent, libVertices[i].componentNumber);
        }
        contribute(CkCallback(CkReductionTarget(Main, donePrinting), mainProxy));
    }
};

std::pair<int,uint64_t>
MeshPiece::getLocationFromID(uint64_t vid) {
    int global_y = (int)(vid % MESH_SIZE);
    int global_x = (int)((vid - global_y)/MESH_SIZE);

    int local_x = global_x % MESHPIECE_SIZE;
    int local_y = global_y % MESHPIECE_SIZE;
    
    int chare_x = (global_x-local_x) / MESHPIECE_SIZE;
    int chare_y = (global_y-local_y) / MESHPIECE_SIZE;

    int chareIdx = chare_x * (MESH_SIZE/MESHPIECE_SIZE) + chare_y;
    int arrIdx = local_x * MESHPIECE_SIZE + local_y;
    return std::make_pair(chareIdx, arrIdx);
}

#include "mesh.def.h"
