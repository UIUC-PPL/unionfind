#include <assert.h>
#include "prefixBalance.h"
#include "unionFindLib.h"

/*readonly*/ CProxy_UnionFindLib _UfLibProxy;
/*readonly*/ CProxy_Prefix prefixLibArray;
/*readonly*/ CkGroupID libGroupID;
CkReduction::reducerType mergeCountMapsReductionType;

// custom reduction for merging local count maps
CkReductionMsg* merge_count_maps(int nMsgs, CkReductionMsg **msgs) {
    std::unordered_map<long int,int> merged_temp_map;
    for (int i = 0; i < nMsgs; i++) {
        // any sanity check for map size?
        // extract this message's local map
        componentCountMap *curr_map = (componentCountMap*)msgs[i]->getData();
        int numComps = msgs[i]->getSize();
        numComps = numComps / sizeof(componentCountMap);

        // convert custom map to STL map for easier lookup
        for (int j = 0; j < numComps; j++) {
            merged_temp_map[curr_map[j].compNum] += curr_map[j].count;
        }
    } // all messages processed

    // convert the STL back to custom map for messaging
    componentCountMap *merged_map = new componentCountMap[merged_temp_map.size()];
    std::unordered_map<long int,int>::iterator iter = merged_temp_map.begin();
    for (int i = 0; i < merged_temp_map.size(); i++) {
        componentCountMap entry;
        entry.compNum = iter->first;
        entry.count = iter->second;
        merged_map[i] = entry;
        iter++;
    }

    int retSize = sizeof(componentCountMap) * merged_temp_map.size();
    return CkReductionMsg::buildNew(retSize, merged_map);
}

// initnode function to register reduction
static void register_merge_count_maps_reduction() {
    mergeCountMapsReductionType = CkReduction::addReducer(merge_count_maps);
}

// class function implementations

/**
 * @brief registers a function that takes a vertexID and returns its location
 * 
 * unionFindLib allows users to specify a vertexID scheme that suits their
 * usecase, as long as it encodes the chare index of the vertex and array index
 * of the vertex on the chare's myVertices field. This function registers the
 * function provided by the user that achieves this decoding, so that the user's
 * function may be used by unionFindLib for internal functionality
 * 
 * @param gloc a function that takes a uint64_t vertexID and returns its chare
 * index and local array index on that chare's myVertices field as
 * a std::pair<int, int>
 */
void UnionFindLib::
registerGetLocationFromID(std::pair<int, int> (*gloc)(uint64_t vid)) {
    getLocationFromID = gloc;
}

void UnionFindLib::
register_phase_one_cb(CkCallback cb) {
    if (thisIndex != 0)
        CkAbort("[UnionFindLib] Phase 1 callback must be registered on first chare only!");

    CkStartQD(cb);
}

/**
 * @brief Adds vertices to this union find chare
 * 
 * Takes an array of unionFindVertex with vertex info populated (ID, etc.)
 * and the length of that array, and stores locally the vertex info
 * that should be associated with this union find chare.
 * 
 * @param appVertices an array of unionFindVertex storing the vertices on the
 * corresponding partition chare
 * @param numVertices the number of vertices in the appVertices array
 */
void UnionFindLib::
initialize_vertices(unionFindVertex *appVertices, int numVertices) {
    // local vertices corresponding to one treepiece in application
    numMyVertices = numVertices;
    myVertices = appVertices;
}

/**
 * @brief performs a union on two vertices given their vertexIDs
 * 
 * assumes the vertexIDs encode the information about the location of the vertex
 * (it's chare index in the union find lib proxy and the local array index
 * of the vertex). performs the actual union operation and carries
 * it's associated runtime cost (cost depends on implementation selected)
 */
#ifndef ANCHOR_ALGO
void UnionFindLib::
union_request(uint64_t vid1, uint64_t vid2) {
    assert(vid1!=vid2);
    if (vid2 < vid1) {
        // found a back edge, flip and reprocess
        union_request(vid2, vid1);
    }
    else {
        std::pair<int, int> vid1_loc = getLocationFromID(vid1);

        //message the chare containing first vertex to find boss1
        //pass the initilizer ID for initiating path compression

        findBossData d;
        d.arrIdx = vid1_loc.second;
        d.partnerOrBossID = vid2;
        d.senderID = -1;
        d.isFBOne = 1;
        this->thisProxy[vid1_loc.first].insertDataFindBoss(d);

        //for profiling
        CProxy_UnionFindLibGroup libGroup(libGroupID);
        libGroup.ckLocalBranch()->increase_message_count();
    }
}
#else
void UnionFindLib::
union_request(uint64_t v, uint64_t w) {
    std::pair<int, int> w_loc = getLocationFromID(w);
    // message w to anchor to v
    anchorData d;
    d.arrIdx = w_loc.second;
    d.v = v;
    thisProxy[w_loc.first].insertDataAnchor(d);
}
#endif

#ifndef ANCHOR_ALGO
void UnionFindLib::
find_boss1(int arrIdx, uint64_t partnerID, uint64_t senderID) {
    unionFindVertex *src = &myVertices[arrIdx];
    CkAssert(src->vertexID != src->parent);
    src->findOrAnchorCount++;

    if (src->parent == -1) {
        //boss1 found
        std::pair<int, int> partner_loc = getLocationFromID(partnerID);
        //message the chare containing the partner
        //senderID for first find_boss2 is not relevant, similar to first find_boss1

        findBossData d;
        d.arrIdx = partner_loc.second;
        d.partnerOrBossID = src->vertexID;
        d.senderID = -1;
        d.isFBOne = 0;
        this->thisProxy[partner_loc.first].insertDataFindBoss(d);

        CProxy_UnionFindLibGroup libGroup(libGroupID);
        libGroup.ckLocalBranch()->increase_message_count();
        //message the initID to kick off path compression in boss1's chain
        /*std::pair<int,int> init_loc = appPtr->getLocationFromID(initID);
        this->thisProxy[init_loc.first].compress_path(init_loc.second, src->vertexID);
        libGroup.ckLocalBranch()->increase_message_count();*/
    }
    else {
        //boss1 not found, move to parent
        std::pair<int, int> parent_loc = getLocationFromID(src->parent);
        unionFindVertex *path_base = src;
        unionFindVertex *parent, *curr = src;

        /* Locality based optimization code:
           instead of using messages to traverse the tree, this
           technique uses a while loop to reach the top of "local" tree i.e
           the last node in the tree path that is locally present on current chare
           We combine this with a local path compression optimization to make
           all local trees completely shallow
        */
        while (parent_loc.first == this->thisIndex) {
            parent = &myVertices[parent_loc.second];

            // entire tree is local to chare
            if (parent->parent ==  -1) {
                local_path_compression(path_base, parent->vertexID);

                findBossData d;
                d.arrIdx = parent_loc.second;
                d.partnerOrBossID = partnerID;
                d.senderID = curr->vertexID;
                d.isFBOne = 1;
                this->insertDataFindBoss(d);

                return;
            }

            // move pointers to traverse tree
            curr = parent;
            parent_loc = getLocationFromID(curr->parent);
        } //end of local tree climbing

        if (path_base->vertexID != curr->vertexID) {
            local_path_compression(path_base, curr->vertexID);
        }
        else {
            //CkPrintf("Self-pointing bug avoided\n");
        }

        CkAssert(parent_loc.first != this->thisIndex);
        //message remote chare containing parent, set the senderID to curr

        findBossData d;
        d.arrIdx = parent_loc.second;
        d.partnerOrBossID = partnerID;
        d.senderID = curr->vertexID;
        d.isFBOne = 1;
        this->thisProxy[parent_loc.first].insertDataFindBoss(d);

        // check if sender and current vertex are on different chares
        if (senderID != -1 && !check_same_chares(senderID, curr->vertexID)) {
            // short circuit the sender to point to grandparent
            std::pair<int,int> sender_loc = getLocationFromID(senderID);
            shortCircuitData scd;
            scd.arrIdx = sender_loc.second;
            scd.grandparentID = curr->parent;
            thisProxy[sender_loc.first].short_circuit_parent(scd);
        }

        CProxy_UnionFindLibGroup libGroup(libGroupID);
        libGroup.ckLocalBranch()->increase_message_count();
    }
}


void UnionFindLib::
find_boss2(int arrIdx, uint64_t boss1ID, uint64_t senderID) {
    unionFindVertex *src = &myVertices[arrIdx]; // vid1, other field is vid2 (boss1ID) - same for find_boss1
    CkAssert(src->vertexID != src->parent);
    src->findOrAnchorCount++;

    if (src->parent == -1) {
        if (boss1ID > src->vertexID) {
            //do not point to somebody greater than you, min-heap property (mostly a cycle edge?)
            union_request(boss1ID, src->vertexID); // flipped and reprocessed
        }
        else {
            //valid edge
            if (boss1ID != src->vertexID) {//avoid self-loop
                src->parent = boss1ID;
                //message initID to start path compression in boss2's chain
                /*std::pair<int,int> init_loc = appPtr->getLocationFromID(initID);
                this->thisProxy[init_loc.first].compress_path(init_loc.second, boss1ID);
                CProxy_UnionFindLibGroup libGroup(libGroupID);
                libGroup.ckLocalBranch()->increase_message_count();*/
            }
        }
    }
    else {
        //boss2 not found, move to parent
        //std::pair<int,int> parent_loc = appPtr->getLocationFromID(src->parent);
        std::pair<int, int> parent_loc = getLocationFromID(src->parent);
        unionFindVertex *path_base = src;
        unionFindVertex *parent, *curr = src;

        // same optimizations as in find_boss1
        while (parent_loc.first == this->thisIndex) {
            parent = &myVertices[parent_loc.second];

            if (parent->parent ==  -1) {
                local_path_compression(path_base, parent->vertexID);

                // find_boss2(parent_loc.second, boss1ID, initID);
                findBossData d;
                d.arrIdx = parent_loc.second;
                d.partnerOrBossID = boss1ID;
                d.senderID = curr->vertexID;
                d.isFBOne = 0;
                this->insertDataFindBoss(d);

                return;
            }

            curr = parent;
            parent_loc = getLocationFromID(curr->parent);
        } //end of local tree climbing

        if (path_base->vertexID != curr->vertexID) {
            local_path_compression(path_base, curr->vertexID);
        }
        else {
            //CkPrintf("Self-pointing bug avoided\n");
        }

        CkAssert(parent_loc.first != this->thisIndex);
        //message remote chare containing parent

        findBossData d;
        d.arrIdx = parent_loc.second;
        d.partnerOrBossID = boss1ID;
        d.senderID = curr->vertexID;
        d.isFBOne = 0;
        this->thisProxy[parent_loc.first].insertDataFindBoss(d);

        // check if sender and current vertex are on different chares
        if (senderID != -1 && !check_same_chares(senderID, curr->vertexID)) {
            // short circuit the sender to point to grandparent
            
            std::pair<int,int> sender_loc = getLocationFromID(senderID);
            shortCircuitData scd;
            scd.arrIdx = sender_loc.second;
            scd.grandparentID = curr->parent;
            thisProxy[sender_loc.first].short_circuit_parent(scd);
        }

        CProxy_UnionFindLibGroup libGroup(libGroupID);
        libGroup.ckLocalBranch()->increase_message_count();
    }
}
#else
void UnionFindLib::
anchor(int w_arrIdx, uint64_t v, long int path_base_arrIdx) {
    unionFindVertex *w = &myVertices[w_arrIdx];
    w->findOrAnchorCount++;

    if (w->parent == v) {
      // call local_path_compression with v as parent
      if (path_base_arrIdx != -1) {
        unionFindVertex *path_base = &myVertices[path_base_arrIdx];
        local_path_compression(path_base, v);
      }
      return;
    }

    if (w->vertexID < v) {
        // incorrect order, swap the vertices
        std::pair<int, int> v_loc = getLocationFromID(v);
        if (v_loc.first == thisIndex) {
            // vertex available locally, avoid extra message
            if (path_base_arrIdx != -1) {
              // Have to change the direction; so compress path for w
              unionFindVertex *path_base = &myVertices[path_base_arrIdx];
              // FIXME: what happens if w is not in this chare?
              local_path_compression(path_base, w->vertexID);
            }
            // start a new base since I am changing direction; can't carry the old one
            path_base_arrIdx = v_loc.second; 
            // anchor(v_loc.second, w->parent, path_base_arrIdx);
            anchor(v_loc.second, w->parent, -1);
            return;
        }
        anchorData d;
        d.arrIdx = v_loc.second;
        d.v = w->parent;
        thisProxy[v_loc.first].insertDataAnchor(d);;
    }
    else if (w->parent == w->vertexID) {
      // I have reached the root; check if I can call local_path_compression
      if (path_base_arrIdx != -1) {
        unionFindVertex *path_base = &myVertices[path_base_arrIdx];
        // Make all nodes point to this parent v
        local_path_compression(path_base, v);
      }
      w->parent = v;
    }
    else {
        // call anchor for w's parent
        std::pair<int, int> w_parent_loc = getLocationFromID(w->parent);
        if (w_parent_loc.first == thisIndex) {
            if (path_base_arrIdx == -1) {
              // Start from w; a wasted call if there is only one node and its child in the PE
              std::pair<int, int> w_loc = getLocationFromID(w->vertexID);
              path_base_arrIdx = w_loc.second; 
            }
            else {
              std::pair<int, int> w_loc = getLocationFromID(w->vertexID);
              // assert (path_base_arrIdx != w_loc.second);
            }
            // anchor(w_parent_loc.second, v, -1);
            anchor(w_parent_loc.second, v, path_base_arrIdx);
            return;
        }
        else {
          // Moving aay from this node; see if local_path_compression should be done
          if (path_base_arrIdx != -1) {
            unionFindVertex *path_base = &myVertices[path_base_arrIdx];
            // Make all nodes point to this parent w
            assert (path_base->vertexID != w->vertexID);
            local_path_compression(path_base, w->vertexID);
          }
        }
        anchorData d;
        d.arrIdx = w_parent_loc.second;
        d.v = v;
        thisProxy[w_parent_loc.first].insertDataAnchor(d);
    }
}
#endif

// perform local path compression
void UnionFindLib::
local_path_compression(unionFindVertex *src, uint64_t compressedParent) {
    unionFindVertex* tmp;
    // An infinite loop if this function is called on itself (a node which does not have itself as its parent)
    while (src->parent != compressedParent) {
        // CkPrintf("Stuck here\n");
        tmp = &myVertices[getLocationFromID(src->parent).second];
        src->parent = compressedParent;
        src =tmp;
    }
}

// check if two vertices are on same chare
bool UnionFindLib::
check_same_chares(uint64_t v1, uint64_t v2) {
    std::pair<int,int> v1_loc = getLocationFromID(v1);
    std::pair<int,int> v2_loc = getLocationFromID(v2);
    if (v1_loc.first == v2_loc.first)
        return true;
    return false;
}

// short circuit a vertex to point to grandparent
void UnionFindLib::
short_circuit_parent(shortCircuitData scd) {
    unionFindVertex *src = &myVertices[scd.arrIdx];
    //CkPrintf("[TP %d] Short circuiting %ld from current parent %ld to grandparent %ld\n", thisIndex, src->vertexID, src->parent, grandparentID);
    src->parent = scd.grandparentID;
    CkAssert(src->parent != src->vertexID); // TODO: remove assert
}

// function to implement simple path compression; currently unused
void UnionFindLib::
compress_path(int arrIdx, uint64_t compressedParent) {
    unionFindVertex *src = &myVertices[arrIdx];
    //message the parent before reseting it
    if (src->vertexID != compressedParent) {//reached the top of path
        std::pair<int, int> parent_loc = getLocationFromID(src->parent);
        this->thisProxy[parent_loc.first].compress_path(parent_loc.second, compressedParent);
        CProxy_UnionFindLibGroup libGroup(libGroupID);
        libGroup.ckLocalBranch()->increase_message_count();
        src->parent = compressedParent;
    }
}

unionFindVertex* UnionFindLib::
return_vertices() {
    return myVertices;
}

/** Functions for finding connected components **/

/**
 * @brief After performing all union_request calls, labels connected components
 * across all union find chares with coherent indexing starting with index 0 for
 * component 0
 * 
 * @param cb Callback to be invoked after this function has finished
 */
void UnionFindLib::
find_components(CkCallback cb) {
    postComponentLabelingCb = cb;
    // count local numBosses
    myLocalNumBosses = 0;
    for (int i = 0; i < numMyVertices; i++) {
#ifndef ANCHOR_ALGO
        if (myVertices[i].parent == -1) {
#else
        // for Anchor algo, each vertex is ititially the parent of itself
        if (myVertices[i].parent == myVertices[i].vertexID) {
#endif
            myLocalNumBosses += 1;
        }
    }

    // send local count to prefix library
    CkCallback doneCb(CkReductionTarget(UnionFindLib, boss_count_prefix_done), thisProxy);
    prefixLibArray[thisIndex].startPrefixCalculation(myLocalNumBosses, doneCb);
    //CkPrintf("[%d] Local num bosses: %d\n", thisIndex, myLocalNumBosses);
}

// Recveive total boss count from prefix library and start labelling phase
void UnionFindLib::
boss_count_prefix_done(int totalCount) {
    totalNumBosses = totalCount;
    // access value from prefix lib elem to find starting index
    Prefix* myPrefixElem = prefixLibArray[thisIndex].ckLocal();
    int v = myPrefixElem->getValue();
    int myStartIndex = v - myLocalNumBosses;
    CkAssert(myStartIndex >= 0);

    // start labeling my local bosses from myStartIndex
    // ensures sequential numbering of components
    if (myLocalNumBosses != 0) {
        for (int i = 0; i < numMyVertices; i++) {
#ifndef ANCHOR_ALGO
            if (myVertices[i].parent == -1) {
#else
            if (myVertices[i].parent == myVertices[i].vertexID) {
#endif
                myVertices[i].componentNumber = myStartIndex;
                myStartIndex++;
            }
        }
    }

    CkAssert(myStartIndex == v);

    // start the labeling phase for all vertices
    start_component_labeling();
}

void UnionFindLib::
start_component_labeling() {
    for (int i = 0; i < numMyVertices; i++) {
        unionFindVertex *v = &myVertices[i];
#ifndef ANCHOR_ALGO
        if (v->parent == -1) {
#else
        if (v->parent == v->vertexID) {
#endif
            // one of the bosses/root found
            CkAssert(v->componentNumber != -1); // phase 2a assigned serial numbers
            set_component(i, v->componentNumber);
        }

        if (v->componentNumber == -1) {
            // an internal node or leaf node, request parent for boss
            std::pair<int, int> parent_loc = getLocationFromID(v->parent);
            //this->thisProxy[parent_loc.first].need_boss(parent_loc.second, v->vertexID);
            // uint64_t data = ((uint64_t) parent_loc.second) << 32 | ((uint64_t) v->vertexID);
            needBossData data;
            data.arrIdx = parent_loc.second;
            data.senderID = v->vertexID;
            this->thisProxy[parent_loc.first].insertDataNeedBoss(data);
        }
    }

    if (this->thisIndex == 0) {
        // return back to application after completing all messaging related to
        // connected components algorithm
        CkStartQD(postComponentLabelingCb);
    }
}

void UnionFindLib::
insertDataFindBoss(const findBossData & data) {
#ifndef ANCHOR_ALGO
    if (data.isFBOne == 1) {
        this->find_boss1(data.arrIdx, data.partnerOrBossID, data.senderID);
    }
    else {
        this->find_boss2(data.arrIdx, data.partnerOrBossID, data.senderID);
    }
#endif
}

void UnionFindLib::
insertDataNeedBoss(const needBossData & data) {
    int arrIdx = data.arrIdx;
    uint64_t fromID = data.senderID;
    this->need_boss(arrIdx, fromID);
}

#ifdef ANCHOR_ALGO
void UnionFindLib::
insertDataAnchor(const anchorData & data) {
    anchor(data.arrIdx, data.v, -1);
}
#endif

void UnionFindLib::
need_boss(int arrIdx, uint64_t fromID) {
    // one of children of this node needs boss, handle by either 
    // replying immediately or queueing the request

    if (myVertices[arrIdx].componentNumber != -1) {
        // component already set, reply back
        std::pair<int, int> requestor_loc = getLocationFromID(fromID);
        if (requestor_loc.first == thisIndex) {
            set_component(requestor_loc.second, myVertices[arrIdx].componentNumber);
        } else {
            this->thisProxy[requestor_loc.first].set_component(requestor_loc.second, myVertices[arrIdx].componentNumber);
        }
    }
    else {
        // boss still not found, queue the request
        myVertices[arrIdx].need_boss_requests.push_back(fromID);
    }
}

void UnionFindLib::
set_component(int arrIdx, long int compNum) {
    myVertices[arrIdx].componentNumber = compNum;

    // since component number is set, respond to your requestors
    std::vector<uint64_t> need_boss_queue = myVertices[arrIdx].need_boss_requests;
    while (!need_boss_queue.empty()) {
        uint64_t requestorID = (need_boss_queue).back();
        std::pair<int, int> requestor_loc = getLocationFromID(requestorID);
        if (requestor_loc.first == thisIndex) {
            set_component(requestor_loc.second, compNum);
        } else {
            this->thisProxy[requestor_loc.first].set_component(requestor_loc.second, compNum);
        }
        // done with current requestor, delete from request queue
        need_boss_queue.pop_back();
    }
}

/**
 * @brief discards components with number of vertices less than or equal to the
 * threshold given and labels them with component number -1
 * 
 * @param threshold the minimum number of vertices for a component must be
 * strictly greater than this number
 * @param appReturnCb Callback to be invoked upon completion
 */
void UnionFindLib::
prune_components(int threshold, CkCallback appReturnCb) {
    componentPruneThreshold = threshold;

    //int *localCounts = new int[totalNumBosses]();
    std::vector<int> localCounts(totalNumBosses, 0);
    //if (localCounts == NULL) {
    //    CkAbort("We are out of memory!");
    //}

    for (int i = 0; i < numMyVertices; i++) {
        long int bossID = myVertices[i].componentNumber;
        CkAssert(bossID >= 0 && bossID < totalNumBosses);
        localCounts[bossID]++;
    }

    //CkPrintf("[TP %d] localCounts constructed\n", thisIndex);

    // bcast totalCounts to all group chares
    CProxy_UnionFindLibGroup libGroupProxy(libGroupID);
    CkCallback cb(CkReductionTarget(UnionFindLibGroup, build_component_count_array), libGroupProxy);
    //contribute(sizeof(int)*totalNumBosses, localCounts, CkReduction::sum_int, cb);
    contribute(localCounts, CkReduction::sum_int, cb);

    //delete[] localCounts;

    // start QD to return back to application
    if (thisIndex == 0) {
        CkStartQD(appReturnCb);
    }
}

// reductiontarget from group => all component count arrays are ready
void UnionFindLib::
perform_pruning() {

    CProxy_UnionFindLibGroup libGroup(libGroupID);

    for (int i = 0; i < numMyVertices; i++) {
        int myComponentCount = libGroup.ckLocalBranch()->get_component_count(myVertices[i].componentNumber);
        if (myComponentCount <= componentPruneThreshold) {
            myVertices[i].componentNumber = -1;
        }
#ifdef PROFILING
        //CkPrintf("Vertex ID : %d, count : %ld\n", myVertices[i].vertexID, myVertices[i].findOrAnchorCount);
#endif
    }

    if (thisIndex == 0) {
        CkPrintf("Number of components found: %d\n", totalNumBosses);
        int numPrunedComponents = 0;
        for (int i = 0; i < totalNumBosses; i++) {
            int compCount = libGroup.ckLocalBranch()->get_component_count(i);
            if (compCount <= componentPruneThreshold) {
                numPrunedComponents++;
            }
        }
        //CkPrintf("Number of components after pruning: %d\n", totalNumBosses-numPrunedComponents);
    }

#ifdef PROFILING
    long int maxCount = -1;
    for (int i = 0; i < numMyVertices; i++) {
        if (myVertices[i].findOrAnchorCount > maxCount)
            maxCount = myVertices[i].findOrAnchorCount;
    }
    CkCallback cb(CkReductionTarget(UnionFindLib, profiling_count_max), thisProxy[0]);
    contribute(sizeof(long int), &maxCount, CkReduction::max_long, cb);
#endif
}

#ifdef PROFILING
void UnionFindLib::
profiling_count_max(long int maxCount) {
    CkAssert(thisIndex == 0);
    CkPrintf("Max number of find/anchor messages per vertex: %ld\n", maxCount);
}
#endif

// library group chare class definitions
void UnionFindLibGroup::
build_component_count_array(int *totalCounts, int numElems) {
    //CkPrintf("[PE %d] Count array size: %d\n", thisIndex, numElems);
    component_count_array = new int[numElems];
    memcpy(component_count_array, totalCounts, sizeof(int)*numElems);
    contribute(CkCallback(CkReductionTarget(UnionFindLib, perform_pruning), _UfLibProxy));
}

int UnionFindLibGroup::
get_component_count(long int component_id) {
    return component_count_array[component_id];
}

void UnionFindLibGroup::
increase_message_count() {
    thisPeMessages++;
}

void UnionFindLibGroup::
contribute_count() {
    CkCallback cb(CkReductionTarget(UnionFindLibGroup, done_profiling), thisProxy);
    contribute(sizeof(int), &thisPeMessages, CkReduction::sum_int, cb);
}

void UnionFindLibGroup::
done_profiling(int total_count) {
    if (CkMyPe() == 0) {
        CkPrintf("Phase 1 profiling done. Total number of messages is : %d\n", total_count);
        CkExit();
    }
}

/**
 * @brief initializes unionFindLib and returns a union find lib proxy
 * 
 * Takes a chare array where vertices are stored and creates a union find chare
 * array that is a shadow array of it. Intended so that when accessing vertices
 * on the application level, one can easily make a invoke a local function
 * on the corresponding union find chare using CkLocal()
 * 
 * @param clientArray chare array that union find proxy will become shadow array
 * of
 * @param n number of chares in the clientArray
 * @return CProxy_UnionFindLib the chare array union find proxy
 */
CProxy_UnionFindLib UnionFindLib::
unionFindInit(CkArrayID clientArray, int n) {
    CkArrayOptions opts(n);
    opts.bindTo(clientArray);
    _UfLibProxy = CProxy_UnionFindLib::ckNew(opts, NULL);

    // create prefix library array here, prefix library is used in Phase 1B
    // Binding order: prefix -> unionFind -> app array
    CkArrayOptions prefix_opts(n);
    prefix_opts.bindTo(_UfLibProxy);
    prefixLibArray = CProxy_Prefix::ckNew(n, prefix_opts);

    libGroupID = CProxy_UnionFindLibGroup::ckNew();
    return _UfLibProxy;
}

#include "unionFindLib.def.h"


/*------------------- Old Code: Reduction using custom structs & maps -----------------*/
#if 0
void UnionFindLib::
merge_count_results(int* totalCounts, int numElems) {

    CkAssert(numElems == totalNumBosses);
    for (int i = 0; i < numMyVertices; i++) {
        int myComponentCount = totalCounts[myVertices[i].componentNumber];
        if (myComponentCount <= componentPruneThreshold) {
            myVertices[i].componentNumber = -1;
        }
    }

    if (thisIndex == 0) {
        CkPrintf("Number of components found: %d\n", numElems);
        int numPrunedComponents = 0;
        for (int i = 0; i < numElems; i++) {
            if (totalCounts[i] <= componentPruneThreshold) {
                numPrunedComponents++;
            }
        }
        CkPrintf("Number of components after pruning: %d\n", numElems-numPrunedComponents);
    }
}

void UnionFindLib::
prune_components(int threshold, CkCallback appReturnCb) {
    //create a count map
    // key: componentNumber
    // value: local count of vertices belonging to component

    componentPruneThreshold = threshold;
    std::unordered_map<long int, int> temp_count;

    // populate local count map
    for (int i = 0; i < numMyVertices; i++) {
        temp_count[myVertices[i].componentNumber]++;
    }

    // Sanity check
    /*std::map<long int,int>::iterator it = temp_count.begin();
    while (it != temp_count.end()) {
        CkPrintf("[%d] %ld -> %d\n", this->thisIndex, it->first, it->second);
        it++;
    }*/

    // convert STL map to custom map (array of structures)
    // for contributing to reduction
    componentCountMap *local_map = new componentCountMap[temp_count.size()];
    std::unordered_map<long int,int>::iterator iter = temp_count.begin();
    for (int j = 0; j < temp_count.size(); j++) {
        if (iter == temp_count.end())
            CkAbort("Something corrupted in map memory!\n");

        componentCountMap entry;
        entry.compNum = iter->first;
        entry.count = iter->second;
        local_map[j] = entry;
        iter++;
    }

    CkCallback cb(CkIndex_UnionFindLib::merge_count_results(NULL), this->thisProxy);
    int contributeSize = sizeof(componentCountMap) * temp_count.size();
    this->contribute(contributeSize, local_map, mergeCountMapsReductionType, cb);

    // start QD to return back to application
    if (this->thisIndex == 0) {
        CkStartQD(appReturnCb);
    }

}

void UnionFindLib::
merge_count_results(CkReductionMsg *msg) {
    //ask lib group to build map
    CProxy_UnionFindLibGroup libGroup(libGroupID);
    libGroup.ckLocalBranch()->build_component_count_map(msg, totalNumBosses);

    for (int i = 0; i < numMyVertices; i++) {
        // query the group chare to get component count
        int myComponentCount = libGroup.ckLocalBranch()->get_component_count(myVertices[i].componentNumber);
        CkAssert(myVertices[i].componentNumber < totalNumBosses);
        if (myComponentCount <= componentPruneThreshold) {
            // vertex belongs to a minor component, ignore by setting to -1
            myVertices[i].componentNumber = -1;
        }
    }
}


// library group chare class definitions
void UnionFindLibGroup::
build_component_count_map(CkReductionMsg *msg, int numCompsOriginal) {
    if (!map_built) {
        componentCountMap *final_map = (componentCountMap*)msg->getData();
        int numComps = msg->getSize();
        numComps /= sizeof(componentCountMap);

        if (CkMyPe() == 0) {
            CkPrintf("Number of components found: %d\n", numComps);
            CkPrintf("Number of components before pruning: %d\n", numCompsOriginal);
        }

        // convert custom map back to STL for quick lookup
        for (int i = 0; i < numComps; i++) {
            component_count_map[final_map[i].compNum] = final_map[i].count;
            if (CkMyPe() == 0) {
                CkPrintf("Component %d has %d vertices\n", final_map[i].compNum, final_map[i].count);
            }
        }

        // map is built now on each PE, share among local chares
        map_built = true;
    }
}
#endif
