#ifndef UNION_FIND_LIB
#define UNION_FIND_LIB

#include "unionFindLib.decl.h"
#include <unordered_map>

#ifdef AGGREGATION
// htram is only pulled in when compiled with aggregation; htram-off clients
// can include this header with no htram header / -DUNIONFIND / include path.
#include <NDMeshStreamer.h>
#include "htram_group.h"

//tram
using tram_proxy_t = CProxy_HTram;
using tram_t = HTram;

extern tram_proxy_t tram_proxy;
#endif
/*readonly*/ extern CProxy_UnionFindLib _UfLibProxy;

struct unionFindVertex {
    uint64_t vertexID;
    int64_t parent;
    int64_t process_tip; //used during path compression to store the last node local vertex on the path to the root
    long int componentNumber = -1;
    int64_t componentSize = -1;
    int64_t size = 1;
    std::vector<uint64_t> need_boss_requests; //request queue for processing need_boss requests
    long int findOrAnchorCount = 0;

    void pup(PUP::er &p) {
        p|vertexID;
        p|parent;
        p|process_tip;
        p|componentNumber;
        p|componentSize;
        p|size;
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

typedef struct componentCacheEntry {
    long int compNum;
    int64_t compSize = -1;
    std::vector<long int> requestors;

    void pup(PUP::er &p) {
        p|compNum;
        p|compSize;
        p|requestors;
    }
} cacheEntry;


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
    CkCallback postPruningCb;
    std::unordered_map<long int, cacheEntry> parentCache; //maps vertex numbers to component numbers
#ifdef AGGREGATION
    // htram state, only present when compiled with aggregation
    tram_t* tram;
    tram_proxy_t myTramProxy;
#endif

    public:
    UnionFindLib() : myVertices(nullptr), numMyVertices(0) {}
    UnionFindLib(CkMigrateMessage *m) { }
    void pup(PUP::er &p) {
        CBase_UnionFindLib::pup(p);
#ifdef AGGREGATION
        p | myTramProxy; // htram proxy only exists under aggregation
#endif
    }
    void ckJustMigrated() {
#ifdef AGGREGATION
        tram = myTramProxy.ckLocalBranch();
#endif
        CBase_UnionFindLib::ckJustMigrated();
    }
    void passLibGroupID(CkGroupID lgid, CProxy_Prefix pla, CkCallback ready);
    static CProxy_UnionFindLib unionFindInit(CkArrayID clientArray, int n);
    // Client-facing API addition: one library chare per PROCESS, element i on
    // the first PE of node i (explicit insertion — NOT an array map; a
    // freshly created map group races with array construction on runtimes
    // without group-dependency buffering, e.g. reconverse). `ready` fires
    // (array reduction) once every element has been constructed and wired,
    // so the caller may safely follow with broadcasts.
    static CProxy_UnionFindLib unionFindInitOnePerNode(const CkCallback& ready);
    // Race-safe form: `node_map` must be a UFNodeMap group the CALLER created
    // EARLY (with any barrier between its ckNew and this call), so its
    // branches exist on every process before the array construction that
    // consults it. Required on runtimes without group-dependency buffering
    // (reconverse): the one-argument form above creates the map inline and
    // can abort with "Local branch of array map is NULL!" at scale.
    static CProxy_UnionFindLib unionFindInitOnePerNode(const CkCallback& ready,
                                                       CProxy_UFNodeMap node_map);
#ifdef AGGREGATION
    void set_tram_proxy(tram_proxy_t proxy); // htram wiring, aggregation only
#endif
    void registerGetLocationFromID(std::pair<int, int> (*gloc)(uint64_t vid));
    void register_phase_one_cb(CkCallback cb);
    void initialize_vertices(unionFindVertex *appVertices, int numVertices);
    uint64_t get_parent(uint64_t vertexID);
#ifndef ANCHOR_ALGO
    static void insertDataCaller(void *p, findBossData data) {
        UnionFindLib *lib = _UfLibProxy[data.targetChareIdx].ckLocal();
         if (lib != nullptr)
            lib->insertDataFindBoss(data);
        else //this fallback will undo aggregation and do a send
            _UfLibProxy[data.targetChareIdx].insertDataFindBoss(data);
    }
    void boss_send(int chare_index, findBossData data); //sends during boss finding, with or without aggregation
    void union_request(uint64_t vid1, uint64_t vid2);
    void find_boss1(int arrIdx, uint64_t partnerID, uint64_t senderID);
    void find_boss2(int arrIdx, uint64_t boss1ID, uint64_t senderID);
#else
    static void insertDataCaller(void *p, anchorData data) {
        UnionFindLib *lib = _UfLibProxy[data.targetChareIdx].ckLocal();
        if (lib != nullptr) {
            lib->insertDataAnchor(data);
        } else {
            _UfLibProxy[data.targetChareIdx].insertDataAnchor(data);
        }
    }
    void anchor_send(int chare_index, anchorData data); //sends during anchoring, with or without aggregation
    void union_request(uint64_t v, uint64_t w);
    void anchor(int w_arrIdx, uint64_t v, long int path_base_arrIdx);
#endif
    // client-facing API addition: batched union requests (one message per PE).
    // Placed outside the ANCHOR_ALGO guard: both algo variants provide a
    // two-arg union_request that this wraps.
    void union_requests(const std::vector<UFEdge>& edges);
    void flush_buffers();
    void quiesce(CkCallback cb);
    void local_union(uint64_t vid1, uint64_t vid2);
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
    void add_size(int arrIdx, int64_t delta);
    void set_component(int arrIdx, long int compNum, int64_t compSize);
    void prune_components(int threshold, CkCallback appReturnCb);
    void perform_pruning();
    void report_surviving_components(long *totalData, int numElems);
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

// Client-facing API addition: array map placing element i on the first PE of
// (SMP) node i — the one-chare-per-process layout used by
// unionFindInitOnePerNode.
class UFNodeMap : public CBase_UFNodeMap {
  public:
    UFNodeMap() {}
    int procNum(int, const CkArrayIndex &idx) {
        return CkNodeFirst(idx.data()[0]);
    }
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
