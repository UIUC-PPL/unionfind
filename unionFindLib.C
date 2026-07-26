#include <assert.h>
#include "prefixBalance.h"
#include "unionFindLib.h"

/*readonly*/ CProxy_UnionFindLib _UfLibProxy;
/*readonly*/ CProxy_Prefix prefixLibArray;
/*readonly*/ CkGroupID libGroupID;
CkReduction::reducerType mergeCountMapsReductionType;

#ifdef AGGREGATION
// htram readonly proxies, only defined when compiled with aggregation
/* readonly */ CProxy_HTramRecv nodeGrpProxy;
/* readonly */ CProxy_HTramNodeGrp srcNodeGrpProxy;
/* readonly */ tram_proxy_t tram_proxy;
#endif


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
registerGetLocationFromID(std::pair<int, uint64_t> (*gloc)(uint64_t vid)) {
    getLocationFromID = gloc;
}

void UnionFindLib::
enableLazyMode() {
#ifdef ANCHOR_ALGO
    CkAbort("[UnionFindLib] lazy vertex storage is not supported with ANCHOR_ALGO");
#endif
    lazy_mode = true;
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
 * @brief gets the parent of a vertex given its vertexID
 */

uint64_t UnionFindLib::get_parent(uint64_t vertexID) {
    std::pair<int, uint64_t> loc = getLocationFromID(vertexID);
    if (loc.first != this->thisIndex) {
        CkAbort("[UnionFindLib] get_parent called with vertexID that does not belong to this chare!");
    }
    return vertexAt(loc.second)->parent;
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

void UnionFindLib::boss_send(int chare_index, findBossData data) {
    #ifdef AGGREGATION
    //send data during boss finding, with or without aggregation based on compilation flag
    int pe;
    // Check local first: avoids using a stale lastKnown cache entry if the
    // element has migrated to this PE since the cache was last updated.
    if (_UfLibProxy[chare_index].ckLocal() != nullptr) {
        pe = CkMyPe();
    } else {
        CkArray *arr = thisProxy.ckLocalBranch();
        CkArrayIndex idx(chare_index);
        pe = arr->lastKnown(idx);
        if (pe == -1) {
            CkAbort("Location not found\n");
            pe = arr->homePe(idx);
            arr->getLocMgr()->requestLocation(idx);
        }
    }
    #endif

    //send message to the chare
    #ifndef AGGREGATION
    this->thisProxy[chare_index].insertDataFindBoss(data);
    #else
    data.targetChareIdx = chare_index;
    tram->insertValue(data, pe);
    #endif
}

void UnionFindLib::
union_request(uint64_t vid1, uint64_t vid2) {
    assert(vid1!=vid2);
    if (vid2 < vid1) {
        // found a back edge, flip and reprocess
        union_request(vid2, vid1);
        return;
    }

    std::pair<int, uint64_t> vid1_loc = getLocationFromID(vid1);
    std::pair<int, uint64_t> vid2_loc = getLocationFromID(vid2);

    // Fast path: both vertices are on this chare — skip the find_boss protocol
    // entirely and do a local sequential union with full path compression.
    if (vid1_loc.first == this->thisIndex && vid2_loc.first == this->thisIndex) {
        local_union(vid1, vid2);
        CProxy_UnionFindLibGroup libGroup(libGroupID);
        #ifdef PROFILING
        libGroup.ckLocalBranch()->increase_message_count();
        #endif
        return;
    }

    //message the chare containing first vertex to find boss1
    findBossData d;
    d.arrIdx = vid1_loc.second;
    d.partnerOrBossID = vid2;
    d.senderID = -1;
    d.isFBOne = 1;
    if(vid1_loc.first == this->thisIndex)
    {
        this->insertDataFindBoss(d);
    }
    else
    {
        //remote message to start boss1 find
        boss_send(vid1_loc.first, d);
    }

    //for profiling
    CProxy_UnionFindLibGroup libGroup(libGroupID);
    #ifdef PROFILING
    libGroup.ckLocalBranch()->increase_message_count();
    #endif
}
#else

void UnionFindLib::anchor_send(int chare_index, anchorData data) {
    #ifdef AGGREGATION
    int pe;
    // Check local first: avoids using a stale lastKnown cache entry if the
    // element has migrated to this PE since the cache was last updated.
    if (_UfLibProxy[chare_index].ckLocal() != nullptr) {
        pe = CkMyPe();
    } else {
        CkArray *arr = thisProxy.ckLocalBranch();
        CkArrayIndex idx(chare_index);
        pe = arr->lastKnown(idx);
        if (pe == -1) {
            pe = arr->homePe(idx);
            arr->getLocMgr()->requestLocation(idx);
        }
    }
    #endif

    //send data during anchoring, with or without aggregation based on compilation flag
    #ifndef AGGREGATION
    this->thisProxy[chare_index].insertDataAnchor(data);
    #else
    data.targetChareIdx = chare_index;
    tram->insertValue(data, pe);
    #endif
}

void UnionFindLib::
union_request(uint64_t v, uint64_t w) {
    std::pair<int, uint64_t> w_loc = getLocationFromID(w);
    // message w to anchor to v
    anchorData d;
    d.arrIdx = w_loc.second;
    d.v = v;
    anchor_send(w_loc.first, d);
}
#endif

// Client-facing API addition: batched union requests (one marshalled message
// per submitting PE instead of one entry invocation per edge). Outside the
// ANCHOR_ALGO guard: both algo variants provide a two-arg union_request.
void UnionFindLib::
union_requests(const std::vector<UFEdge>& edges) {
    for (const auto& e : edges) union_request(e.a, e.b);
}

#ifndef ANCHOR_ALGO
void UnionFindLib::
find_boss1(uint64_t arrIdx, uint64_t partnerID, uint64_t senderID) {
    unionFindVertex *src = vertexAt(arrIdx);
    CkAssert(src->vertexID != src->parent);
    src->findOrAnchorCount++;

    if (src->parent == -1) {
        //boss1 found
        std::pair<int, uint64_t> partner_loc = getLocationFromID(partnerID);
        //message the chare containing the partner
        //senderID for first find_boss2 is not relevant, similar to first find_boss1

        findBossData d;
        d.arrIdx = partner_loc.second;
        d.partnerOrBossID = src->vertexID;
        d.senderID = -1;
        d.isFBOne = 0;
        if(partner_loc.first == this->thisIndex)
        {
            insertDataFindBoss(d);
        }
        else
        {
            //remote message to start boss2 find
            boss_send(partner_loc.first, d);
        }
        

        CProxy_UnionFindLibGroup libGroup(libGroupID);
        #ifdef PROFILING
        libGroup.ckLocalBranch()->increase_message_count();
        #endif
        //message the initID to kick off path compression in boss1's chain
        /*std::pair<int,int> init_loc = appPtr->getLocationFromID(initID);
        this->thisProxy[init_loc.first].compress_path(init_loc.second, src->vertexID);
        libGroup.ckLocalBranch()->increase_message_count();*/
    }
    else {
        //boss1 not found, move to parent
        std::pair<int, uint64_t> parent_loc = getLocationFromID(src->parent);
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
            parent = vertexAt(parent_loc.second);

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
        //remote message to continue boss1 find
        boss_send(parent_loc.first, d);

        /*
        // check if sender and current vertex are on different chares
        if (senderID != -1 && !check_same_chares(senderID, curr->vertexID)) {
            // short circuit the sender to point to grandparent
            std::pair<int,int> sender_loc = getLocationFromID(senderID);
            shortCircuitData scd;
            scd.arrIdx = sender_loc.second;
            scd.grandparentID = curr->parent;
            //send to path compress across chares
            thisProxy[sender_loc.first].short_circuit_parent(scd);
        }
            */

        CProxy_UnionFindLibGroup libGroup(libGroupID);
        #ifdef PROFILING
        libGroup.ckLocalBranch()->increase_message_count();
        #endif
    }
}


void UnionFindLib::
find_boss2(uint64_t arrIdx, uint64_t boss1ID, uint64_t senderID) {
    unionFindVertex *src = vertexAt(arrIdx); // vid1, other field is vid2 (boss1ID) - same for find_boss1
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
                // propagate size to new root before setting parent
                std::pair<int,int> boss1_loc = getLocationFromID(boss1ID);
                if (boss1_loc.first == thisIndex) {
                    add_size(boss1_loc.second, src->size);
                } else {
                    thisProxy[boss1_loc.first].add_size(boss1_loc.second, src->size);
                }
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
        std::pair<int, uint64_t> parent_loc = getLocationFromID(src->parent);
        unionFindVertex *path_base = src;
        unionFindVertex *parent, *curr = src;

        // same optimizations as in find_boss1
        while (parent_loc.first == this->thisIndex) {
            parent = vertexAt(parent_loc.second);

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
        //remote message to continue boss2 find
        boss_send(parent_loc.first, d);

        /*
        // check if sender and current vertex are on different chares
        if (senderID != -1 && !check_same_chares(senderID, curr->vertexID)) {
            // short circuit the sender to point to grandparent
            
            std::pair<int,int> sender_loc = getLocationFromID(senderID);
            shortCircuitData scd;
            scd.arrIdx = sender_loc.second;
            scd.grandparentID = curr->parent;
            //send to path compress across chares
            thisProxy[sender_loc.first].short_circuit_parent(scd);
        }
            */

        CProxy_UnionFindLibGroup libGroup(libGroupID);
        #ifdef PROFILING
        libGroup.ckLocalBranch()->increase_message_count();
        #endif
    }
}
#else
void UnionFindLib::
anchor(uint64_t w_arrIdx, uint64_t v, long int path_base_arrIdx) {
    unionFindVertex *w = vertexAt(w_arrIdx);
    w->findOrAnchorCount++;

    //this case is if the vertices are already in the same component
    //and v is the boss
    if (w->parent == v) {
      // call local_path_compression with v as parent
      if (path_base_arrIdx != -1) {
        unionFindVertex *path_base = vertexAt(path_base_arrIdx);
        local_path_compression(path_base, v);
      }
      return;
    }

    //order correction, broken into local and remote cases
    //in the local case, if your path base is not -1 (so not first on this pe), do local compression
    //before doing a new anchor with v as the local one
    //key: efficiently maintain minheap by switching 
    if (w->vertexID < v) {
        // incorrect order, swap the vertices
        std::pair<int, uint64_t> v_loc = getLocationFromID(v);
        if (v_loc.first == thisIndex) {
            // vertex available locally, avoid extra message
            if (path_base_arrIdx != -1) {
              // Have to change the direction; so compress path for w
              unionFindVertex *path_base = vertexAt(path_base_arrIdx);
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
        //remote anchor send
        anchor_send(v_loc.first, d);
    }
    else if (w->parent == w->vertexID) {
      // I have reached the root; check if I can call local_path_compression
      if (path_base_arrIdx != -1) {
        unionFindVertex *path_base = vertexAt(path_base_arrIdx);
        // Make all nodes point to this parent v
        local_path_compression(path_base, v);
      }
      // propagate size to new root before setting parent
      std::pair<int,int> v_loc_size = getLocationFromID(v);
      if (v_loc_size.first == thisIndex) {
          add_size(v_loc_size.second, w->size);
      } else {
          thisProxy[v_loc_size.first].add_size(v_loc_size.second, w->size);
      }
      w->parent = v; //anchor algo guarantees that v will be smaller here
    }
    //correct order (w is larger) and not at the root
    else {
        // call anchor for w's parent
        std::pair<int, uint64_t> w_parent_loc = getLocationFromID(w->parent);
        if (w_parent_loc.first == thisIndex) {
            if (path_base_arrIdx == -1) {
              // Start from w; a wasted call if there is only one node and its child in the PE
              std::pair<int, uint64_t> w_loc = getLocationFromID(w->vertexID);
              path_base_arrIdx = w_loc.second; 
            }
            else {
                //looks like dead code?
              std::pair<int, uint64_t> w_loc = getLocationFromID(w->vertexID);
              // assert (path_base_arrIdx != w_loc.second);
            }
            // anchor(w_parent_loc.second, v, -1);
            anchor(w_parent_loc.second, v, path_base_arrIdx);
            return;
        }
        else {
          // Moving away from this node; see if local_path_compression should be done
          if (path_base_arrIdx != -1) {
            unionFindVertex *path_base = vertexAt(path_base_arrIdx);
            // Make all nodes point to this parent w
            assert (path_base->vertexID != w->vertexID);
            local_path_compression(path_base, w->vertexID);
          }
        }
        anchorData d;
        d.arrIdx = w_parent_loc.second;
        d.v = v;
        anchor_send(w_parent_loc.first, d);
    }
}
#endif

// Fast path for union requests where both vertices are on this chare.
// Walks each vertex's parent chain, stopping at the actual root (parent == -1)
// OR at the last local node before the chain crosses to a remote chare.
// Returns the local tip and sets *is_actual_root to indicate which case occurred.
// Compresses all traversed local nodes to point directly to the local tip.
void UnionFindLib::
local_union(uint64_t vid1, uint64_t vid2) {
    // The original hard-coded the vertexID encoding (chare = vid >> 32,
    // idx = vid & 0xFFFFFFFF), silently diverging from the registered
    // getLocationFromID. Decode through the registered function so the
    // application's encoding is authoritative on this fast path too.
    auto arrIdx = [this](uint64_t vid) -> int { return getLocationFromID(vid).second; };
    auto chareOf = [this](uint64_t vid) -> int { return getLocationFromID(vid).first; };

    // Walk parent chain staying within this chare.
    // Returns the local tip (either the actual root if parent==-1, or the last
    // local node before the chain goes remote).  Compresses the traversed path
    // to point directly to that tip.  *crossed_boundary is set to true if we
    // stopped because the next parent is on a different chare.
    auto find_local_tip = [&](uint64_t start, bool &crossed_boundary) -> uint64_t {
        uint64_t curr = start;
        while (true) {
            int64_t par = vertexAt(arrIdx(curr))->parent;
            if (par == -1) {
                // curr is the actual root of its component (locally)
                crossed_boundary = false;
                break;
            }
            uint64_t par_vid = (uint64_t)par;
            if (chareOf(par_vid) != thisIndex) {
                // next step leaves this chare — curr is the local tip
                crossed_boundary = true;
                break;
            }
            curr = par_vid;
        }
        uint64_t tip = curr;
        // Compress: point every node on the path directly to tip
        curr = start;
        while (curr != tip) {
            int64_t next = vertexAt(arrIdx(curr))->parent;
            vertexAt(arrIdx(curr))->parent = (int64_t)tip;
            curr = (uint64_t)next;
        }
        return tip;
    };

    bool crossed1, crossed2;
    uint64_t tip1 = find_local_tip(vid1, crossed1);
    uint64_t tip2 = find_local_tip(vid2, crossed2);

    // If either path left this chare, we can't resolve the actual boss locally.
    // Call insertDataFindBoss directly from the local tips to avoid re-triggering
    // the local_union fast path (which would cause infinite recursion).
    if (crossed1 || crossed2) {
        if (tip1 == tip2) return;
        if (tip2 < tip1) std::swap(tip1, tip2);
        findBossData d;
        d.arrIdx = arrIdx(tip1); // decode through the registered function
        d.partnerOrBossID = tip2;
        d.senderID = -1;
        d.isFBOne = 1;
        this->insertDataFindBoss(d);
        return;
    }

    // Both paths ended at actual roots on this chare — merge directly.
    if (tip1 == tip2) return; // already same component
    if (tip1 < tip2) {
        vertexAt(arrIdx(tip1))->size += vertexAt(arrIdx(tip2))->size;
        vertexAt(arrIdx(tip2))->parent = (int64_t)tip1;
    } else {
        vertexAt(arrIdx(tip2))->size += vertexAt(arrIdx(tip1))->size;
        vertexAt(arrIdx(tip1))->parent = (int64_t)tip2;
    }
}

void UnionFindLib::
local_path_compression(unionFindVertex *src, uint64_t compressedParent) {
    unionFindVertex* tmp;
    // An infinite loop if this function is called on itself (a node which does not have itself as its parent)
    while (src->parent != compressedParent) {
        // CkPrintf("Stuck here\n");
        tmp = vertexAt(getLocationFromID(src->parent).second);
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
    unionFindVertex *src = vertexAt(scd.arrIdx);
    //CkPrintf("[TP %d] Short circuiting %ld from current parent %ld to grandparent %ld\n", thisIndex, src->vertexID, src->parent, grandparentID);
    src->parent = scd.grandparentID;
    CkAssert(src->parent != src->vertexID); // TODO: remove assert
}

// function to implement simple path compression; currently unused
void UnionFindLib::
compress_path(uint64_t arrIdx, uint64_t compressedParent) {
    unionFindVertex *src = vertexAt(arrIdx);
    //message the parent before reseting it
    if (src->vertexID != compressedParent) {//reached the top of path
        std::pair<int, uint64_t> parent_loc = getLocationFromID(src->parent);
        this->thisProxy[parent_loc.first].compress_path(parent_loc.second, compressedParent);
        CProxy_UnionFindLibGroup libGroup(libGroupID);
        libGroup.ckLocalBranch()->increase_message_count();
        src->parent = compressedParent;
    }
}

// Adds delta to the size of the root reachable from arrIdx.
// If this vertex is no longer a root (it was merged), forwards to current parent
// so that no size contribution is lost regardless of message ordering.
void UnionFindLib::
add_size(uint64_t arrIdx, int64_t delta) {
#ifndef ANCHOR_ALGO
    bool is_root = (vertexAt(arrIdx)->parent == -1);
#else
    // Match existing root checks (e.g. line 641): use unsigned promotion so
    // high-bit vertex IDs compare correctly against int64_t parent.
    bool is_root = ((uint64_t)vertexAt(arrIdx)->parent == vertexAt(arrIdx)->vertexID);
#endif
    if (is_root) {
        vertexAt(arrIdx)->size += delta;
    } else {
        std::pair<int,int> par_loc = getLocationFromID((uint64_t)vertexAt(arrIdx)->parent);
        if (par_loc.first == thisIndex) {
            add_size(par_loc.second, delta);
        } else {
            thisProxy[par_loc.first].add_size(par_loc.second, delta);
        }
    }
}

unionFindVertex* UnionFindLib::
return_vertices() {
    if (lazy_mode)
        CkAbort("[UnionFindLib] get_vertices is dense-mode only; use collectComponentLabels in lazy mode");
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
    // count local numBosses (forEachVertex: dense = whole array, lazy =
    // touched vertices only — untouched ids are implicitly their own
    // components and are not counted here; see the lazy-mode semantics note)
    myLocalNumBosses = 0;
    forEachVertex([&](unionFindVertex& vtx, uint64_t) {
#ifndef ANCHOR_ALGO
        if (vtx.parent == -1)
#else
        // for Anchor algo, each vertex is ititially the parent of itself
        if ((uint64_t)vtx.parent == vtx.vertexID)
#endif
            myLocalNumBosses += 1;
    });

    // send local count to prefix library
    CkCallback doneCb(CkReductionTarget(UnionFindLib, boss_count_prefix_done), thisProxy);
    prefixLibArray[thisIndex].startPrefixCalculation(myLocalNumBosses, doneCb);
}

// Recveive total boss count from prefix library and start labelling phase
void UnionFindLib::
boss_count_prefix_done(int totalCount) {
    totalNumBosses = totalCount;
    if(thisIndex==0) CkPrintf("Number of components found: %d\n", totalNumBosses);
    // access value from prefix lib elem to find starting index
    Prefix* myPrefixElem = prefixLibArray[thisIndex].ckLocal();
    int v = myPrefixElem->getValue();
    int myStartIndex = v - myLocalNumBosses;
    CkAssert(myStartIndex >= 0);

    // start labeling my local bosses from myStartIndex
    // ensures sequential numbering of components
    if (myLocalNumBosses != 0) {
        forEachVertex([&](unionFindVertex& vtx, uint64_t) {
#ifndef ANCHOR_ALGO
            if (vtx.parent == -1) {
#else
            if ((uint64_t)vtx.parent == vtx.vertexID) {
#endif
                vtx.componentNumber = myStartIndex;
                myStartIndex++;
            }
        });
    }

    CkAssert(myStartIndex == v);

    // start the labeling phase for all vertices
    start_component_labeling();
}

void UnionFindLib::
start_component_labeling() {
    forEachVertex([&](unionFindVertex& vtx, uint64_t i) {
        unionFindVertex *v = &vtx;
#ifndef ANCHOR_ALGO
        if (v->parent == -1) {
#else
        if ((uint64_t)v->parent == v->vertexID) {
#endif
            // one of the bosses/root found
            CkAssert(v->componentNumber != -1); // phase 2a assigned serial numbers
            set_component(i, v->componentNumber, v->size);
        }

        if (v->componentNumber == -1) {
            //if the parent's component is cached
            //if(auto search = parentCache.find(v->parent); search != parentCache.end())
            if(parentCache.count(v->parent) != 0)
            {
                //check if the cache entry has a component number
                if(parentCache[v->parent].compNum != -1)
                {
                    //call set component on myself
                    set_component(i, parentCache[v->parent].compNum, parentCache[v->parent].compSize);
                    //then loop over and call set component on the waiting requests (should only run once per cache entry)
                    for(int j=0; j<parentCache[v->parent].requestors.size(); j++)
                    {
                        set_component(parentCache[v->parent].requestors[j], parentCache[v->parent].compNum, parentCache[v->parent].compSize);
                    }
                    parentCache[v->parent].requestors.clear();
                }
                else
                {
                    parentCache[v->parent].requestors.push_back((long int) i);
                }
            }
            else
            {
                // an internal node or leaf node, request parent for boss
                std::pair<int, uint64_t> parent_loc = getLocationFromID(v->parent);
                needBossData data;
                data.arrIdx = parent_loc.second;
                data.senderID = v->vertexID;
                if(parent_loc.first == thisIndex)
                {
                    insertDataNeedBoss(data);
                }
                else
                {
                    this->thisProxy[parent_loc.first].insertDataNeedBoss(data);
                }
            }
        }
    });

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
need_boss(uint64_t arrIdx, uint64_t fromID) {
    // one of children of this node needs boss, handle by either 
    // replying immediately or queueing the request

    if (vertexAt(arrIdx)->componentNumber != -1) {
        // component already set, reply back
        std::pair<int, uint64_t> requestor_loc = getLocationFromID(fromID);
        if (requestor_loc.first == thisIndex) {
            set_component(requestor_loc.second, vertexAt(arrIdx)->componentNumber, vertexAt(arrIdx)->componentSize);
        } else {
            this->thisProxy[requestor_loc.first].set_component(requestor_loc.second, vertexAt(arrIdx)->componentNumber, vertexAt(arrIdx)->componentSize);
        }
    }
    else {
        // boss still not found, queue the request
        vertexAt(arrIdx)->need_boss_requests.push_back(fromID);
    }
}

void UnionFindLib::
set_component(uint64_t arrIdx, long int compNum, int64_t compSize) {
    // Iterative propagation via an explicit work queue to avoid stack overflow
    // from deep recursive chains when union-find trees are not fully compressed.
    std::vector<int> work_queue;
    work_queue.push_back(arrIdx);

    while (!work_queue.empty()) {
        int idx = work_queue.back();
        work_queue.pop_back();

        vertexAt(idx)->componentNumber = compNum;
        vertexAt(idx)->componentSize = compSize;

        // Update parentCache entry if this vertex's parent is on a different chare
        int64_t my_parent = vertexAt(idx)->parent;
        std::pair<int, uint64_t> parent_loc = getLocationFromID((uint64_t)my_parent);
        if (parent_loc.first != thisIndex)
        {
            if (parentCache.count(my_parent) != 0)
            {
                parentCache[my_parent].compNum = compNum;
                parentCache[my_parent].compSize = compSize;
                for (int j = 0; j < (int)parentCache[my_parent].requestors.size(); j++)
                {
                    work_queue.push_back(parentCache[my_parent].requestors[j]);
                }
            }
        }

        // Respond to all vertices that were waiting for this vertex's component label.
        // Drain the actual queue (not a copy) so requests are not re-processed.
        std::vector<uint64_t> local_requests;
        local_requests.swap(vertexAt(idx)->need_boss_requests);
        for (uint64_t requestorID : local_requests) {
            std::pair<int, uint64_t> requestor_loc = getLocationFromID(requestorID);
            if (requestor_loc.first == thisIndex) {
                work_queue.push_back(requestor_loc.second);
            } else {
                this->thisProxy[requestor_loc.first].set_component(requestor_loc.second, compNum, compSize);
            }
        }
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
    postPruningCb = appReturnCb;

    int localSurviving = 0;
    long bucket[64] = {0}; // for component size distribution, can be removed later
    forEachVertex([&](unionFindVertex& vtx, uint64_t) {
        if (vtx.componentSize <= threshold) {
            vtx.componentNumber = -1;
        } else {
            // count surviving bosses to get total component count
#ifndef ANCHOR_ALGO
            if (vtx.parent == -1)
            {
                bucket[(int) log(vtx.componentSize)]++;
                localSurviving++;
            }
#else
            if (vtx.parent == (int64_t)vtx.vertexID) localSurviving++;
#endif
        }
    });


    // pack surviving count + bucket distribution into one array for a single reduction
    long reductionData[65] = {0};
    reductionData[0] = localSurviving;
    for (int b = 0; b < 64; b++) reductionData[b + 1] = bucket[b];

    CkCallback cb(CkReductionTarget(UnionFindLib, report_surviving_components), thisProxy[0]);
    contribute(sizeof(long) * 65, reductionData, CkReduction::sum_long, cb);
}

void UnionFindLib::
report_surviving_components(long *totalData, int numElems) {
    CkAssert(thisIndex == 0);
    CkPrintf("Number of components after pruning: %ld\n", totalData[0]);
    CkPrintf("Component size distribution:\n");
    for (int b = 0; b < 64; b++) {
        if (totalData[b + 1] > 0)
            CkPrintf("  bucket[%d]: %ld components\n", b, totalData[b + 1]);
    }
    CkStartQD(postPruningCb);
}

// reductiontarget from group => all component count arrays are ready
void UnionFindLib::
perform_pruning() {

    CProxy_UnionFindLibGroup libGroup(libGroupID);

    forEachVertex([&](unionFindVertex& vtx, uint64_t) {
        int myComponentCount = libGroup.ckLocalBranch()->get_component_count(vtx.componentNumber);
        if (myComponentCount <= componentPruneThreshold) {
            vtx.componentNumber = -1;
        }
    });

    if (thisIndex == 0) {
        //CkPrintf("Number of components found: %d\n", totalNumBosses);
        int numPrunedComponents = 0;
        for (int i = 0; i < totalNumBosses; i++) {
            int compCount = libGroup.ckLocalBranch()->get_component_count(i);
            if (compCount <= componentPruneThreshold) {
                numPrunedComponents++;
            }
        }
        CkPrintf("Number of components after pruning: %d\n", totalNumBosses-numPrunedComponents);
    }

#ifdef PROFILING
    long int maxCount = -1;
    forEachVertex([&](unionFindVertex& vtx, uint64_t) {
        if (vtx.findOrAnchorCount > maxCount)
            maxCount = vtx.findOrAnchorCount;
    });
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

void UnionFindLib::flush_buffers() {
#ifdef AGGREGATION
    myTramProxy.flush_everything();
#endif
    // no-op when not compiled with AGGREGATION
}

void UnionFindLib::quiesce(CkCallback cb) {
#ifdef AGGREGATION
    myTramProxy[0].htramQuiesce(cb);
#else
    cb.send();
#endif
}

/**
 * sets the tram proxy
 * and the func ptr for insertDataCaller
 */
#ifdef AGGREGATION
void UnionFindLib::set_tram_proxy(tram_proxy_t proxy) {
    myTramProxy = proxy;
    tram = myTramProxy.ckLocalBranch();
    tram->set_func_ptr(UnionFindLib::insertDataCaller, this);
}
#endif

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
    //tram init
    #ifdef AGGREGATION
    nodeGrpProxy = CProxy_HTramRecv::ckNew();
    srcNodeGrpProxy = CProxy_HTramNodeGrp::ckNew();
    CkCallback ignore_cb(CkCallback::ignore);
    //note buffer size: not used in smp
    tram_proxy = tram_proxy_t::ckNew(nodeGrpProxy.ckGetGroupID(), srcNodeGrpProxy.ckGetGroupID(), 1024, false, static_cast<double>(0.01)/1000, true, true, ignore_cb);
    #endif
    _UfLibProxy = CProxy_UnionFindLib::ckNew(opts, NULL);

    #ifdef AGGREGATION
    _UfLibProxy.set_tram_proxy(tram_proxy);
    #endif
    // create prefix library array here, prefix library is used in Phase 1B
    // Binding order: prefix -> unionFind -> app array
    CkArrayOptions prefix_opts(n);
    prefix_opts.bindTo(_UfLibProxy);
    prefixLibArray = CProxy_Prefix::ckNew(n, prefix_opts);

    libGroupID = CProxy_UnionFindLibGroup::ckNew();

    // unionFindInit does not order against a ready callback; the elements
    // contribute to an ignored callback (see passLibGroupID's `ready`).
    _UfLibProxy.passLibGroupID(libGroupID, prefixLibArray, CkCallback(CkCallback::ignore));

    #ifdef AGGREGATION
    // print aggregation option
    printf("UnionFindLib: Compiled with aggregation optimizations\n");
    #else
    printf("UnionFindLib: Compiled without aggregation optimizations\n");
    #endif

    return _UfLibProxy;
}

/**
 * Client-facing API addition (FoF): one library chare per PROCESS, element i
 * placed on the first PE of node i via UFNodeMap, instead of binding to a
 * client array. The prefix array is bound to the library array (prefix element
 * i co-located with library element i, as find_components requires). `ready`
 * fires once every element has executed passLibGroupID (constructed and
 * wired); callers must wait on it before any broadcast that relies on the
 * prefix/group proxies, since message delivery is not ordered.
 */
// Compatibility wrapper: creates the placement map inline, which is UNSAFE
// on runtimes without group-dependency buffering (reconverse) — see the
// overload below. Kept for classic-Converse clients; new callers should
// pre-create the map at startup and use the two-argument form.
CProxy_UnionFindLib UnionFindLib::
unionFindInitOnePerNode(const CkCallback& ready) {
    return unionFindInitOnePerNode(ready, CProxy_UFNodeMap::ckNew());
}

// Lazy-mode variant: identical placement and wiring, but every element
// switches to lazy vertex storage before `ready` fires (enableLazyMode is
// broadcast after creation; array broadcasts from the same source are
// delivered in order, so it precedes any union_request the caller sends
// after ready).
CProxy_UnionFindLib UnionFindLib::
unionFindInitOnePerNodeLazy(const CkCallback& ready, CProxy_UFNodeMap node_map) {
    CProxy_UnionFindLib lib_proxy = unionFindInitOnePerNode(ready, node_map);
    lib_proxy.enableLazyMode();
    return lib_proxy;
}

CProxy_UnionFindLib UnionFindLib::
unionFindInitOnePerNode(const CkCallback& ready, CProxy_UFNodeMap node_map) {
    int n = CkNumNodes();

    //tram init: mirror unionFindInit so the one-per-node path also works when
    //built with aggregation (htram) enabled.
    #ifdef AGGREGATION
    nodeGrpProxy = CProxy_HTramRecv::ckNew();
    srcNodeGrpProxy = CProxy_HTramNodeGrp::ckNew();
    CkCallback ignore_cb(CkCallback::ignore);
    //note buffer size: not used in smp
    tram_proxy = tram_proxy_t::ckNew(nodeGrpProxy.ckGetGroupID(), srcNodeGrpProxy.ckGetGroupID(), 1024, false, static_cast<double>(0.01)/1000, true, true, ignore_cb);
    #endif

    // Array placement uses the caller-provided, PRE-CREATED UFNodeMap group.
    // Creating the map group here and immediately ckNew'ing the array (the
    // old idiom) RACES on runtimes without group-dependency buffering — the
    // array-construction broadcast can reach a remote process before the map
    // group's branch exists there, aborting with "Local branch of array map
    // is NULL!" (reconverse, first seen at 32 processes on Anvil,
    // 2026-07-24; classic Converse delays such messages and never exposed
    // it). The caller must create the map EARLY (any barrier/QD between its
    // ckNew and this call guarantees the branches exist). The map must stay:
    // it also defines element HOMES, which boss_send's lastKnown() relies on
    // to route htram traffic only to element-hosting first-PEs.
    CkArrayOptions opts(n);
    opts.setMap(node_map);

    CProxy_UnionFindLib lib_proxy = CProxy_UnionFindLib::ckNew(opts);
    _UfLibProxy = lib_proxy;

    #ifdef AGGREGATION
    lib_proxy.set_tram_proxy(tram_proxy);
    #endif

    CkArrayOptions prefix_opts(n);
    prefix_opts.bindTo(lib_proxy);
    CProxy_Prefix pla = CProxy_Prefix::ckNew(n, prefix_opts);

    CkGroupID lgid = CProxy_UnionFindLibGroup::ckNew();

    lib_proxy.passLibGroupID(lgid, pla, ready);

    #ifdef AGGREGATION
    printf("UnionFindLib: Compiled with aggregation optimizations\n");
    #else
    printf("UnionFindLib: Compiled without aggregation optimizations\n");
    #endif

    return lib_proxy;
}

void UnionFindLib::passLibGroupID(CkGroupID lgid, CProxy_Prefix pla, CkCallback ready)
{
    prefixLibArray = pla;
    libGroupID = lgid;
    _UfLibProxy = this->thisProxy;
    // Client-facing API addition: contribute so callers (e.g.
    // unionFindInitOnePerNode) can order initialization against later
    // broadcasts. unionFindInit passes CkCallback::ignore.
    contribute(ready);
}

#include "unionFindLib.def.h"


/*------------------- Old Code: Reduction using custom structs & maps -----------------*/
#if 0
void UnionFindLib::
merge_count_results(int* totalCounts, int numElems) {

    CkAssert(numElems == totalNumBosses);
    for (int i = 0; i < numMyVertices; i++) {
        int myComponentCount = totalCounts[vertexAt(i)->componentNumber];
        if (myComponentCount <= componentPruneThreshold) {
            vertexAt(i)->componentNumber = -1;
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
    forEachVertex([&](unionFindVertex& vtx, uint64_t) {
        temp_count[vtx.componentNumber]++;
    });

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

    forEachVertex([&](unionFindVertex& vtx, uint64_t) {
        // query the group chare to get component count
        int myComponentCount = libGroup.ckLocalBranch()->get_component_count(vtx.componentNumber);
        CkAssert(vtx.componentNumber < totalNumBosses);
        if (myComponentCount <= componentPruneThreshold) {
            // vertex belongs to a minor component, ignore by setting to -1
            vtx.componentNumber = -1;
        }
    });
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
