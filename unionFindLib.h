#ifndef UNION_FIND_LIB
#define UNION_FIND_LIB

#include "unionFindLib.decl.h"
#include <NDMeshStreamer.h>

struct unionFindVertex {
    uint64_t vertexID;
    int64_t parent;
    long int componentNumber = -1;
    std::vector<uint64_t> need_boss_requests; //request queue for processing need_boss requests
    long int findOrAnchorCount = 0;

    void pup(PUP::er &p) {
        p|vertexID;
        p|parent;
        p|componentNumber;
        p|need_boss_requests;
    }
};

struct componentCountMap {
    long int compNum;
    int count;

    void pup(PUP::er &p) {
        p|compNum;
        p|count;
    }
};


/* global variables */
/*readonly*/ extern CkGroupID libGroupID;
// declaration for custom reduction
extern CkReduction::reducerType mergeCountMapsReductionType;

// class definition for library chares
class UnionFindLib : public CBase_UnionFindLib {
    unionFindVertex *myVertices;
    int numMyVertices;
    int pathCompressionThreshold = 5;
    int componentPruneThreshold;
    std::pair<int, int> (*getLocationFromID)(uint64_t vid);
    int myLocalNumBosses;
    int totalNumBosses;
    CkCallback postComponentLabelingCb;

    public:
    UnionFindLib() {}
    UnionFindLib(CkMigrateMessage *m) { }
    static CProxy_UnionFindLib unionFindInit(CkArrayID clientArray, int n);
    void registerGetLocationFromID(std::pair<int, int> (*gloc)(uint64_t vid));
    void register_phase_one_cb(CkCallback cb);
    void initialize_vertices(unionFindVertex *appVertices, int numVertices);
#ifndef ANCHOR_ALGO
    void union_request(uint64_t vid1, uint64_t vid2);
    void find_boss1(int arrIdx, uint64_t partnerID, uint64_t senderID);
    void find_boss2(int arrIdx, uint64_t boss1ID, uint64_t senderID);
#else
    void union_request(uint64_t v, uint64_t w);
    void anchor(int w_arrIdx, uint64_t v, long int path_base_arrIdx);
#endif
    void local_path_compression(unionFindVertex *src, uint64_t compressedParent);
    bool check_same_chares(uint64_t v1, uint64_t v2);
    void short_circuit_parent(shortCircuitData scd);
    void compress_path(int arrIdx, uint64_t compressedParent);
    unionFindVertex *return_vertices();

    // functions and data structures for finding connected components

    public:
    void find_components(CkCallback cb);
    void boss_count_prefix_done(int totalCount);
    void start_component_labeling();
    void insertDataNeedBoss(const needBossData & data);
    void insertDataFindBoss(const findBossData & data);
#ifdef ANCHOR_ALGO
    void insertDataAnchor(const anchorData & data);
#endif
    void need_boss(int arrIdx, uint64_t fromID);
    void set_component(int arrIdx, long int compNum);
    void prune_components(int threshold, CkCallback appReturnCb);
    void perform_pruning();
    int get_total_num_bosses() {
        return totalNumBosses;
    }
    //void merge_count_results(CkReductionMsg *msg);
    //void merge_count_results(int* totalCounts, int numElems);
#ifdef PROFILING
    void profiling_count_max(long int maxCount);
#endif
};

// library group chare class declarations
class UnionFindLibGroup : public CBase_UnionFindLibGroup {
    bool map_built;
    int* component_count_array;
    int thisPeMessages; //for profiling
    public:
    UnionFindLibGroup() {
        map_built = false;
        thisPeMessages = 0;
    }
    void build_component_count_array(int* totalCounts, int numComponents);
    int get_component_count(long int component_id);
    void increase_message_count();
    void contribute_count();
    void done_profiling(int);
};





#define CK_TEMPLATES_ONLY
#include "unionFindLib.def.h"
#undef CK_TEMPLATES_ONLY

#endif

/// Some old functions for backup/reference ///

/*
void UnionFindLib::
start_boss_propagation() {
    // iterate over local bosses and send messages to requestors
    CkPrintf("Should never get executed!\n");
    std::vector<int>::iterator iter = local_boss_indices.begin();
    while (iter != local_boss_indices.end()) {
        int bossIdx = *iter;
        long int bossID = myVertices[bossIdx].vertexID;
        std::vector<long int>::iterator req_iter = myVertices[bossIdx].need_boss_requests.begin();
        while (req_iter != myVertices[bossIdx].need_boss_requests.end()) {
            long int requestorID = *req_iter;
            std::pair<int,int> requestor_loc = appPtr->getLocationFromID(requestorID);
            this->thisProxy[requestor_loc.first].set_component(requestor_loc.second, bossID);
            // done with requestor, delete from requests queue
            req_iter = myVertices[bossIdx].need_boss_requests.erase(req_iter);
        }
        // done with this local boss, delete from vector
        iter = local_boss_indices.erase(iter);
    }
}*/

/*
void UnionFindLib::
merge_count_results(CkReductionMsg *msg) {
    componentCountMap *final_map = (componentCountMap*)msg->getData();
    int numComps = msg->getSize();
    numComps = numComps/ sizeof(componentCountMap);

    if (this->thisIndex == 0) {
        CkPrintf("Number of components found: %d\n", numComps);
    }

    // convert custom map back to STL, for easier lookup
    std::map<long int,int> quick_final_map;
    for (int i = 0; i < numComps; i++) {
        if (this->thisIndex == 0) {
            //CkPrintf("Component %ld : Total vertices count = %d\n", final_map[i].compNum, final_map[i].count);
        }
        quick_final_map[final_map[i].compNum] = final_map[i].count;
    }

    for (int i = 0; i < numMyVertices; i++) {
        if (quick_final_map[myVertices[i].componentNumber] <= componentPruneThreshold) {
            // vertex belongs to a minor component, ignore by setting to -1
            myVertices[i].componentNumber = -1;
        }
    }

    delete msg;
}*/
