#include <iostream>
#include "unionFindLib.h"
#include "graph.decl.h"
#include "graph-io.h"


/*readonly*/ CProxy_UnionFindLib libProxy;
/*readonly*/ CProxy_Main mainProxy;
/*readonly*/ int NUM_VERTICES;
/*readonly*/ int NUM_EDGES;
/*readonly*/ int NUM_TREEPIECES;
/*readonly*/ long int lastChareBegin;

class Main : public CBase_Main {
    CProxy_TreePiece tpProxy;
    double startTime;
    public:
    Main(CkArgMsg *m) {
        if (m->argc != 3) {
            CkPrintf("Usage: ./graph <input_file> <num_chares_per_pe>\n");
            CkExit();
        }
        std::string inputFileName(m->argv[1]);
        int charesPerPe = atoi(m->argv[2]);
        FILE *fp = fopen(inputFileName.c_str(), "r");
        char line[256];
        fgets(line, sizeof(line), fp);
        line[strcspn(line, "\n")] = 0;

        std::vector<std::string> params;
        split(line, ' ', &params);

        if (params.size() != 3) {
            CkAbort("Insufficient number of params provided in .g file\n");
        }

        NUM_VERTICES = std::stoi(params[0].substr(strlen("Vertices:")));
        NUM_EDGES = std::stoi(params[1].substr(strlen("Edges:")));
        //NUM_TREEPIECES = std::stoi(params[2].substr(strlen("Treepieces:")));
        NUM_TREEPIECES = CkNumPes() * charesPerPe;

        fclose(fp);

        if (NUM_VERTICES < NUM_TREEPIECES) {
            CkPrintf("Fewer vertices than treepieces\n");
            CkExit();
        }
        tpProxy = CProxy_TreePiece::ckNew(inputFileName, NUM_TREEPIECES);
        // find first vertex ID on last chare
        lastChareBegin = (NUM_VERTICES/NUM_TREEPIECES) * (NUM_TREEPIECES-1) + 1;
        // create a callback for library to inform application after
        // completing inverted tree construction
        CkCallback cb(CkIndex_Main::done(), thisProxy);
        libProxy = UnionFindLib::unionFindInit(tpProxy, NUM_TREEPIECES);
        CkPrintf("[Main] Library array with %d chares created and proxy obtained\n", NUM_TREEPIECES);
        libProxy[0].register_phase_one_cb(cb);
        tpProxy.initializeLibVertices();
    }

    void startWork() {
        startTime = CkWallTimer();
        tpProxy.doWork();
    }

    void done() {
        CkPrintf("[Main] Inverted trees constructed. Notify library to perform components detection\n");
        CkPrintf("[Main] Tree construction time: %f\n", CkWallTimer()-startTime);
        // callback for library to inform application after completing
        // connected components detection
        CkCallback cb(CkIndex_Main::doneFindComponents(), thisProxy);
        //CkCallback cb(CkIndex_TreePiece::requestVertices(), tpProxy); //tmp, for debugging
        libProxy.find_components(cb);
    }

    void doneFindComponents() {
        CkPrintf("[Main] Components identified, prune unecessary ones now\n");
        CkPrintf("[Main] Components detection time: %f\n", CkWallTimer()-startTime);
        // callback for library to report to after pruning
        CkCallback cb(CkIndex_TreePiece::requestVertices(), tpProxy);
        libProxy.prune_components(1, cb);
    }

    void donePrinting() {
        CkPrintf("[Main] Final runtime: %f\n", CkWallTimer()-startTime);
        CkExit();
    }

};

class TreePiece : public CBase_TreePiece {

    std::vector<proteinVertex> myVertices;
    std::vector< std::pair<long int,long int> > library_requests;
    int numMyVertices;
    int numMyEdges;
    FILE *input_file;
    UnionFindLib *libPtr;
    unionFindVertex *libVertices;

    public:
    TreePiece(std::string filename) {
        input_file = fopen(filename.c_str(), "r");

        /*numMyVertices = NUM_VERTICES / NUM_TREEPIECES;
        if (thisIndex == NUM_TREEPIECES - 1) {
            // last chare should get all remaining vertices if not equal division
            numMyVertices += NUM_VERTICES % NUM_TREEPIECES;
        }
        myVertices = new proteinVertex[numMyVertices];*/
        // populate myVertices
        populateMyVertices(myVertices, NUM_VERTICES, NUM_TREEPIECES, thisIndex, input_file);
        numMyVertices = myVertices.size();
        /*if (thisIndex == 0) {
            for (int i = 0; i < numMyVertices; i++)
                CkPrintf("myVertices[%d] = id: %ld\n", i, myVertices[i].id);
        }*/

        // reset input_file pointer
        fseek(input_file, 0, SEEK_SET);
        numMyEdges = NUM_EDGES / NUM_TREEPIECES;
        if (thisIndex == NUM_TREEPIECES - 1) {
            // last chare should get all remaining edges if not equal division
            numMyEdges += NUM_EDGES % NUM_TREEPIECES;
        }
        populateMyEdges(&library_requests, numMyEdges, (NUM_EDGES/NUM_TREEPIECES), thisIndex, input_file, NUM_VERTICES);
    }

    TreePiece(CkMigrateMessage *msg) { }

    // function that must be always defined by application
    // return type -> std::pair<int, int>
    // this specific logic assumes equal distribution of vertices across all tps
    static std::pair<int, int> getLocationFromID(uint64_t vid);

    void initializeLibVertices() {
        // provide vertices data to library
        // parent can be NULL (set to -1)
        libVertices = new unionFindVertex[numMyVertices];
        for (int i = 0; i < numMyVertices; i++) {
            libVertices[i].vertexID = myVertices[i].id;
#ifndef ANCHOR_ALGO
            libVertices[i].parent = -1;
#else
            libVertices[i].parent = libVertices[i].vertexID;
#endif
        }
        libPtr = libProxy[thisIndex].ckLocal();
        libPtr->initialize_vertices(libVertices, numMyVertices);
        libPtr->registerGetLocationFromID(getLocationFromID);
        contribute(CkCallback(CkReductionTarget(Main, startWork), mainProxy));
    }

    void doWork() {

        // vertices and edges populated, now fire union requests

        for (int i = 0; i < library_requests.size(); i++) {
            std::pair<long int,long int> req = library_requests[i];
            libPtr->union_request(req.first, req.second);
        }
    }

    void requestVertices() {
        unionFindVertex *finalVertices = libPtr->return_vertices();
        for (int i = 0; i < numMyVertices; i++) {
            //CkPrintf("[tp%d] myVertices[%d] - vertexID: %ld, parent: %ld, component: %d\n", thisIndex, i, finalVertices[i].vertexID, finalVertices[i].parent, finalVertices[i].componentNumber);
#ifndef ANCHOR_ALGO
            if (finalVertices[i].parent != -1 && finalVertices[i].componentNumber == -1) {
#else
            if (finalVertices[i].parent != finalVertices[i].vertexID && finalVertices[i].componentNumber == -1) {
#endif
                CkAbort("Something wrong in inverted-tree construction!\n");
            }
        }
        contribute(CkCallback(CkReductionTarget(Main, donePrinting), mainProxy));
    }

    void getConnectedComponents() {
        //libPtr->find_components();
        for (int i = 0; i < numMyVertices; i++) {
            CkPrintf("[tp%d] myVertices[%d] - vertexID: %ld, parent: %ld, component: %d\n", thisIndex, i, libVertices[i].vertexID, libVertices[i].parent, libVertices[i].componentNumber);
        }
    }

    void test() {
        CkPrintf("It works!\n");
        CkExit();
    }
};

/*
std::pair<int, int>
TreePiece::getLocationFromID(long int vid) {
    int chareIdx = (vid-1) / (NUM_VERTICES/NUM_TREEPIECES);
    chareIdx = std::min(chareIdx, NUM_TREEPIECES-1);
    int arrIdx;
    if (vid > lastChareBegin)
        arrIdx = vid - lastChareBegin;
    else
        arrIdx = (vid-1) % (NUM_VERTICES/NUM_TREEPIECES);
    return std::make_pair(chareIdx, arrIdx);
}
*/

std::pair<int, int>
TreePiece::getLocationFromID(uint64_t vid) {
    int chareIdx = (vid-1) % NUM_TREEPIECES;
    int arrIdx = (vid-1) / NUM_TREEPIECES;
    return std::make_pair(chareIdx, arrIdx);
}


#include "graph.def.h"