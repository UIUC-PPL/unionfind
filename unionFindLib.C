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

// relay78: Kale's backward short-circuit. When a find chain is about to
// LEAVE this chare, tell the sender to point straight at where we are going.
// The mechanism was already written and commented out at both call sites;
// this only gates it on an env knob so both arms run from one binary.
static bool shortCircuitEnabled() {
    static const bool on = [] {
        const char* e = getenv("FOF_UF_SHORTCIRCUIT");
        return e && atoi(e) != 0;
    }();
    return on;
}

static const char* UFS_NAMES[] = {
    "fb1_root", "fb1_climb_remote", "fb1_climb_local",
    "fb2_climb_remote", "fb2_climb_local",
    "fb2_UNION", "fb2_SAMEROOT_discard", "fb2_flip",
    "addsize_root", "addsize_forward", "local_union",
    "sc_sent", "sc_applied", "sc_rejected_stale", "addsize_SKIPPED" };

// relay79: the union-find `size` field is DEAD in FoF3 -- the app seeds it,
// the library maintains it through add_size, set_component propagates it, and
// then collectComponentLabels reads componentNumber ONLY (unionFindLib.h:146).
// componentSize appears once in the app, as "= -1", and is never read; the
// app derives every size and max_size from the LABELS at the end
// (depositLabelCounts -> histogramShard -> collectTouchedCounts).  The
// size-consuming path here (merge_count_results) is inside #if 0.
// FOF_UF_SIZES=0 skips the add_size calls.  Default 1 = unchanged behaviour.
// If the analysis is right, the EXACT gate (components AND max_size) is
// untouched by this knob -- that gate is the test.
static bool sizesEnabled() {
    static const bool on = [] {
        const char* e = getenv("FOF_UF_SIZES");
        return e ? (atoi(e) != 0) : true;
    }();
    return on;
}

// Measurement arm for the sharding question (Kale, 2026-08-22): what do
// within-process chains look like when nobody shortens them? FOF_UF_LOCALCOMP=0
// disables the find-path local_path_compression calls (correctness unaffected;
// compression is an optimization). Together with the climb-hop histogram this
// bounds from above the intra-process message volume a sharded (per-treepiece /
// per-PE) element design would pay, since today's chain lengths are measured
// WITH compression on and are therefore a lower bound.
static bool localCompEnabled() {
    static const bool on = [] {
        const char* e = getenv("FOF_UF_LOCALCOMP");
        return e ? (atoi(e) != 0) : true;
    }();
    return on;
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
        ufs_[10]++;
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
        ufs_[0]++;
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
        long climb_steps = 0;
        while (parent_loc.first == this->thisIndex) {
            parent = vertexAt(parent_loc.second);
            ufs_[2]++; climb_steps++;

            // entire tree is local to chare
            if (parent->parent ==  -1) {
                if (localCompEnabled())
                    local_path_compression(path_base, parent->vertexID);
                ufh_note(climb_steps);

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
        ufh_note(climb_steps);

        if (localCompEnabled() && path_base->vertexID != curr->vertexID) {
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
        ufs_[1]++;
        boss_send(parent_loc.first, d);

        // relay78 (Kale, 2026-08-22): we are leaving this chare continuing the
        // SAME chain, so tell the sender to skip straight to where we go.
        // NOTE the pair type: the commented original used std::pair<int,int>,
        // which truncates a 64-bit local index. The race Kale named is handled
        // in short_circuit_parent by a monotone guard, not by an epoch.
        if (shortCircuitEnabled() && senderID != (uint64_t)-1 &&
            !check_same_chares(senderID, curr->vertexID)) {
            std::pair<int, uint64_t> sender_loc = getLocationFromID(senderID);
            shortCircuitData scd;
            scd.arrIdx = sender_loc.second;
            scd.grandparentID = curr->parent;
            ufs_[11]++;
            thisProxy[sender_loc.first].short_circuit_parent(scd);
        }

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
            ufs_[7]++;
            //do not point to somebody greater than you, min-heap property (mostly a cycle edge?)
            union_request(boss1ID, src->vertexID); // flipped and reprocessed
        }
        else {
            //valid edge
            if (boss1ID == src->vertexID) ufs_[6]++;   // SAME ROOT: discarded
            if (boss1ID != src->vertexID) {//avoid self-loop
                ufs_[5]++;                            // real union
                // propagate size to new root before setting parent
                if (sizesEnabled()) {
                    std::pair<int,int> boss1_loc = getLocationFromID(boss1ID);
                    if (boss1_loc.first == thisIndex) {
                        add_size(boss1_loc.second, src->size);
                    } else {
                        thisProxy[boss1_loc.first].add_size(boss1_loc.second, src->size);
                    }
                } else ufs_[14]++;
                src->parent = boss1ID;
                wave_dirty_ = true;   // structural union: chain may deepen
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
        long climb_steps = 0;
        while (parent_loc.first == this->thisIndex) {
            parent = vertexAt(parent_loc.second);
            ufs_[4]++; climb_steps++;

            if (parent->parent ==  -1) {
                if (localCompEnabled())
                    local_path_compression(path_base, parent->vertexID);
                ufh_note(climb_steps);

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
        ufh_note(climb_steps);

        if (localCompEnabled() && path_base->vertexID != curr->vertexID) {
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
        ufs_[3]++;
        boss_send(parent_loc.first, d);

        // relay78: same backward short-circuit on the boss2 chain.
        if (shortCircuitEnabled() && senderID != (uint64_t)-1 &&
            !check_same_chares(senderID, curr->vertexID)) {
            std::pair<int, uint64_t> sender_loc = getLocationFromID(senderID);
            shortCircuitData scd;
            scd.arrIdx = sender_loc.second;
            scd.grandparentID = curr->parent;
            ufs_[11]++;
            thisProxy[sender_loc.first].short_circuit_parent(scd);
        }

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
      if (sizesEnabled()) {
      std::pair<int,int> v_loc_size = getLocationFromID(v);
      if (v_loc_size.first == thisIndex) {
          add_size(v_loc_size.second, w->size);
      } else {
          thisProxy[v_loc_size.first].add_size(v_loc_size.second, w->size);
      }
      } else ufs_[14]++;
      w->parent = v; //anchor algo guarantees that v will be smaller here
      wave_dirty_ = true;   // structural union: chain may deepen
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
    // Size merges gated with the remote add_size flow (FOF_UF_SIZES): under
    // the no-sizes mode the field is uniformly unmaintained, not partially.
    if (tip1 == tip2) return; // already same component
    if (tip1 < tip2) {
        if (sizesEnabled())
            vertexAt(arrIdx(tip1))->size += vertexAt(arrIdx(tip2))->size;
        vertexAt(arrIdx(tip2))->parent = (int64_t)tip1;
    } else {
        if (sizesEnabled())
            vertexAt(arrIdx(tip2))->size += vertexAt(arrIdx(tip1))->size;
        vertexAt(arrIdx(tip1))->parent = (int64_t)tip2;
    }
    wave_dirty_ = true;   // structural union: chain may deepen
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
    // relay78 MONOTONE GUARD for the race Kale named. Between sending the find
    // and this answer arriving, src->parent may already have moved. In this
    // library parent ids strictly DECREASE toward the root (find_boss2 refuses
    // to point at a larger id -- the min-heap property), so "closer to the
    // root" is exactly "smaller". Accepting only a strictly smaller id can
    // never move a pointer away from the root, so no epoch or version is
    // needed; a stale answer is simply dropped.
    if (src->parent == -1 || (int64_t)scd.grandparentID >= src->parent) {
        ufs_[13]++;
        return;
    }
    ufs_[12]++;
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
        ufs_[8]++;
        vertexAt(arrIdx)->size += delta;
    } else {
        ufs_[9]++;
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

/** Mid-stream global path compression -- the "wave" **/

// Env-selected mode: 0 = off, 1 = guarded direct parent rewrites,
// 2 = hedge (union_request instead of writing; unconditionally correct).
static int waveMode() {
    static const int m = [] {
        const char* e = getenv("FOF_WAVE");
        return e ? atoi(e) : 0;
    }();
    return m;
}

// One element's wave pass: every non-root touched vertex asks its
// parent's owner for the parent's current root; roots answer
// immediately; answers cascade down through parked requesters,
// compressing every vertex on the way (wave_apply). Batched one message
// per destination element, like the labeling scatter. Repeat-safe:
// wave_epoch invalidates cached roots; a stale answer still installs a
// then-ancestor (guarded), so overlapping waves degrade to extra
// messages, never to wrong state.
// Periodic mid-cascade firing (v2, 2026-08-22). The single post-flush
// wave measured NO benefit at 2B (relay74/75): at the fireUF2Edges
// barrier the forest is shallow under BOTH streaming settings — with
// -E 16 the cascade already ran during the walk, with -E 0 it has not
// run yet — so there is no moment at that barrier when the forest is
// both deep and stable. The drain the wave targets (334-409 ms at 2B,
// <1% utilization, 128 PEs each ~97% idle on remote round trips) runs
// DURING the cascade, so the wave must fire there: each element
// self-fires a pass every FOF_WAVE_MS while armed. Concurrency with
// live unions is exactly what wave_apply's two rules were designed for.
//
// QD-SAFETY INVARIANT (a message-per-tick heartbeat deadlocks the
// driver, measured 2026-08-22: CkWaitQD never fires and the disarm in
// find_components sits behind it): the timer chain itself is Converse-
// level (CcdCallFnAfter), invisible to QD; a tick runs wave_pass — and
// hence sends messages — ONLY when wave_dirty_ says a structural union
// happened since the last pass. At fixpoint every tick is message-free,
// QD fires, find_components clears wave_armed_, and the next tick ends
// the chain.
static int waveMs() {
    static const int v = [] {
        const char* e = getenv("FOF_WAVE_MS");
        return e ? atoi(e) : 0;   // 0 = no periodic firing
    }();
    return v;
}

static void wavePeriodicThunk(void* elem, double) {
    ((UnionFindLib*)elem)->wave_periodic_tick();
}

void UnionFindLib::
wave_arm() {
    if (waveMode() == 0 || waveMs() <= 0) return;
    if (wave_armed_) return;
    wave_armed_ = true;
    CcdCallFnAfter(wavePeriodicThunk, this, (double)waveMs());
}

void UnionFindLib::
wave_periodic_tick() {
    if (!wave_armed_) return;   // disarmed by find_components; chain ends
    if (wave_dirty_) {          // clean tick = no messages = QD can fire
        wave_dirty_ = false;
        wave_pass();
    }
    CcdCallFnAfter(wavePeriodicThunk, this, (double)waveMs());
}

void UnionFindLib::
compression_wave() {
    if (waveMode() == 0) return;
    wave_pass();
}

void UnionFindLib::
wave_pass() {
    wave_epoch_++;
    long rewrote = 0;
    std::unordered_map<int, std::vector<needBossData>> dest_buf;
    forEachVertex([&](unionFindVertex& v, uint64_t) {
        if (v.parent == -1) return;              // roots have nothing to learn
        std::pair<int, uint64_t> ploc = getLocationFromID((uint64_t)v.parent);
        needBossData d;
        d.arrIdx = ploc.second;
        d.senderID = v.vertexID;
        if (ploc.first == thisIndex) {
            wave_need_root_local(ploc.second, v.vertexID, rewrote);
        } else {
            dest_buf[ploc.first].push_back(d);
        }
    });
    for (auto& kv : dest_buf)
        this->thisProxy[kv.first].wave_need_root_batch(kv.second);
    wave_rewrote_total_ += rewrote;
    if (thisIndex == 0)
        CkPrintf("[UnionFindLib] compression wave %d fired (mode %d); "
                 "element 0 rewrote %ld parents locally (run total %ld)\n",
                 wave_epoch_, waveMode(), rewrote, wave_rewrote_total_);
}

// Apply the wave's answer at vertex q (owner-side): q's chain leads to
// root `rootID` (possibly stale — then rootID is still an ancestor).
// Direct mode writes only a strict improvement on a NON-root (the two
// rules that make concurrent unions safe: never touch a root's parent —
// root links belong exclusively to find_boss2's still-root-checked
// protocol — and only install a smaller ancestor, preserving the
// parent<child invariant). The impossible-by-design cases fall through
// to the hedge, which is correct in every state.
void UnionFindLib::
wave_apply(unionFindVertex* q, long rootID, long& rewrote) {
    if ((uint64_t)rootID == q->vertexID) return;
    if (waveMode() == 2 || q->parent == -1) {
        union_request((uint64_t)rootID, q->vertexID);
    } else if (rootID < q->parent) {
        q->parent = rootID;
        rewrote++;
    }
    q->wave_root = rootID;
    q->wave_epoch = wave_epoch_;
    // Drain requesters parked on q: they get q's root (full compression).
    std::vector<uint64_t> parked;
    parked.swap(q->wave_parked);
    for (uint64_t f : parked) {
        std::pair<int, uint64_t> floc = getLocationFromID(f);
        if (floc.first == thisIndex)
            wave_apply(vertexAt(floc.second), rootID, rewrote);
        else
            this->thisProxy[floc.first].wave_set_root(floc.second, rootID);
    }
}

// A requester (fromID) wants the root above local vertex p.
void UnionFindLib::
wave_need_root_local(uint64_t arrIdx, uint64_t fromID, long& rewrote) {
    unionFindVertex* p = vertexAt(arrIdx);
    long root = -1;
    if (p->parent == -1) root = (long)p->vertexID;         // p IS the root
    else if (p->wave_epoch == wave_epoch_) root = p->wave_root; // cached
    if (root != -1) {
        std::pair<int, uint64_t> floc = getLocationFromID(fromID);
        if (floc.first == thisIndex)
            wave_apply(vertexAt(floc.second), root, rewrote);
        else
            this->thisProxy[floc.first].wave_set_root(floc.second, root);
    } else {
        // p does not know yet; park. p's own request was (or will be)
        // sent by its element's pass — every element receives the wave
        // broadcast — so this entry is always eventually drained.
        p->wave_parked.push_back(fromID);
    }
}

void UnionFindLib::
wave_need_root_batch(const std::vector<needBossData>& batch) {
    long rewrote = 0;
    for (const needBossData& d : batch)
        wave_need_root_local(d.arrIdx, d.senderID, rewrote);
}

void UnionFindLib::
wave_set_root(uint64_t arrIdx, long rootID) {
    long rewrote = 0;
    wave_apply(vertexAt(arrIdx), rootID, rewrote);
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
ufstat_mark() {
    // relay78: snapshot at the fireUF2Edges barrier. Everything counted so far
    // is walk-concurrent; everything after it is the post-walk drain.
    for (int i = 0; i < UFS_N; i++) ufs_mark_[i] = ufs_[i];
}

void UnionFindLib::
ufstat_done(long *v, int n) {
    CkPrintf("[UFSTAT] branch-outcome census, summed over the array\n");
    for (int i = 0; i < UFS_N && 2*UFS_N <= n; i++)
        CkPrintf("[UFSTAT] %-22s walk %14ld   drain %14ld   total %14ld\n",
                 UFS_NAMES[i], v[i], v[UFS_N+i] - v[i], v[UFS_N+i]);
    if (n >= 2*UFS_N + UFH_N) {
        // climb-hop histogram: bucket 0 = parent immediately remote,
        // bucket b>=1 = 2^(b-1)..2^b-1 local hops in one climb episode
        char line[512]; int off = 0;
        off += snprintf(line+off, sizeof(line)-off, "[UFSTAT] climb_local_hops_log2:");
        for (int b = 0; b < UFH_N; b++)
            if (v[2*UFS_N+b])
                off += snprintf(line+off, sizeof(line)-off, " %d:%ld", b, v[2*UFS_N+b]);
        CkPrintf("%s\n", line);
    }
}

void UnionFindLib::
find_components(CkCallback cb) {
    wave_armed_ = false;   // stop periodic waves; the forest is final
    {   // relay78 census: [0,UFS_N) = at the barrier, [UFS_N,2*UFS_N) = final,
        // [2*UFS_N,2*UFS_N+UFH_N) = climb-hop histogram (whole run)
        long both[2*UFS_N + UFH_N];
        for (int i = 0; i < UFS_N; i++) { both[i] = ufs_mark_[i]; both[UFS_N+i] = ufs_[i]; }
        for (int b = 0; b < UFH_N; b++) both[2*UFS_N+b] = ufh_[b];
        contribute(sizeof(long)*(2*UFS_N+UFH_N), both, CkReduction::sum_long,
                   CkCallback(CkReductionTarget(UnionFindLib, ufstat_done), thisProxy[0]));
    }
    postComponentLabelingCb = cb;
    // Stale entries from a previous labeling run would serve wrong labels
    // (the cache is consulted before any request is sent).
    parentCache.clear();
    // SELF-NAMING (2026-08-21; the prefix stage is REMOVED). A boss's
    // globally-unique vertexID IS its component label: no global
    // coordination is needed before labeling begins, so the prefix's
    // collective-plus-barrier — which gated every element's labeling on
    // the slowest element's boss count — is gone. Labels are therefore
    // sparse 64-bit ids rather than dense serials 0..C-1; every consumer
    // in this library keys componentNumber through maps (never arrays),
    // and the fof3 harness canonicalizes labels per group anyway. Dense
    // numbering can be reconstructed by any caller that wants it from
    // the count below. This also makes the labeling wave re-runnable
    // MID-STREAM as a global path-compression pass (design:
    // paratreet2 design/uf2-compression-wave.md), which dense per-run
    // serials never could — the component count is not stable until
    // quiescence, but a boss's own id always names it.
    // Count and self-label local bosses in one pass.
    myLocalNumBosses = 0;
    forEachVertex([&](unionFindVertex& vtx, uint64_t) {
#ifndef ANCHOR_ALGO
        if (vtx.parent == -1) {
#else
        // for Anchor algo, each vertex is initially the parent of itself
        if ((uint64_t)vtx.parent == vtx.vertexID) {
#endif
            vtx.componentNumber = (long int)vtx.vertexID;
            myLocalNumBosses += 1;
        }
    });

    // The total (the "Number of components" answer) is a plain sum
    // reduction — it OVERLAPS the labeling cascade below instead of
    // gating it. Delivered to every element so totalNumBosses stays
    // valid for the pruning/count-map paths.
    CkCallback doneCb(CkReductionTarget(UnionFindLib, component_count_done), thisProxy);
    contribute(sizeof(int), &myLocalNumBosses, CkReduction::sum_int, doneCb);

    // start the labeling phase immediately — nothing global to wait for.
    // (An ordering race here was suspected during the 2026-08-21 under-merge
    // hunt and DISPROVEN: gating labeling on the count reduction reproduced
    // the identical failure. The actual bug was set_component's .ci entry
    // marshalling compNum as int — 32-bit truncation of vertexID-valued
    // labels on every REMOTE delivery, while local deliveries kept 64 bits.
    // Dormant for years under dense serials < 2^31; fatal under
    // self-naming. If label corruption ever recurs, check ENTRY SIGNATURE
    // WIDTHS against the .h first.)
    start_component_labeling();
}

// Sum-reduction of local boss counts: the component total, for the
// answer line and the pruning paths. Purely informational; labeling
// neither waits for nor reads it.
void UnionFindLib::
component_count_done(int totalCount) {
    totalNumBosses = totalCount;
    // In lazy mode only TOUCHED vertices exist, so this total is the
    // cross-process (edge-touched) component count, not the app's final
    // component count — say so (rename rider, Kale 2026-08-21). Dense
    // mode keeps the historical string: every vertex is registered there,
    // the count IS the total, and the standalone harnesses grep for it.
    if (thisIndex == 0) {
        if (lazy_mode)
            CkPrintf("UF_2 cross-process (touched) components: %d\n",
                     totalNumBosses);
        else
            CkPrintf("Number of components found: %d\n", totalNumBosses);
    }
}

void UnionFindLib::
start_component_labeling() {
    // Per-destination request buffers: the scatter below sends at most ONE
    // message per peer chare (a vector of requests) instead of one message
    // per requesting vertex. Together with the parent cache below, which
    // collapses same-parent requests to a single entry, this is the
    // labeling-scatter batching (paratreet2 design/relabel-representative.md
    // era agenda item; observed ~1400 per-vertex sends from one chare at 2B).
    std::unordered_map<int, std::vector<needBossData>> dest_buf;
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
                    // First request for this remote parent CREATES its cache
                    // entry (compNum -1 = pending), so every later vertex with
                    // the same parent takes the requestors-queue branch above
                    // instead of sending its own request; set_component's
                    // cache fan-out drains the queue when the reply arrives.
                    // (The entry-creation step was missing before, which left
                    // the cache permanently empty and the dedup inoperative.)
                    parentCache[v->parent].compNum = -1; // pending entry
                    dest_buf[parent_loc.first].push_back(data);
                }
            }
        }
    });

    // Flush the batched scatter: one message per destination chare.
    for (auto& kv : dest_buf)
        this->thisProxy[kv.first].insertDataNeedBossBatch(kv.second);

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

// Batched form of the labeling scatter: one message per (source chare,
// destination chare) pair carrying every request the source buffered for
// this destination during start_component_labeling.
void UnionFindLib::
insertDataNeedBossBatch(const std::vector<needBossData>& batch) {
    for (const needBossData& data : batch)
        this->need_boss(data.arrIdx, data.senderID);
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
    //
    // 64-bit (2026-08-23). This queue held `int` while EVERY other id on the
    // path -- arrIdx, vertexAt(), need_boss_requests, local_requests -- was
    // already 64-bit. In lazy mode a local id is a RAW PARTICLE ORDER
    // (paratreet2 fof/FoFPhase1.h kUF2IdxBits comment), so past 2^31
    // particles push_back(arrIdx) truncated it: vertexAt(idx) then created a
    // BOGUS lazy_store entry at the wrapped key and labelled that, while the
    // real vertex kept componentNumber == -1. It surfaced as
    // applyUF2Labels' CkEnforce(it->second >= 0) firing on 6760 of 7168 PEs
    // at 24.4B particles -- ~the exact fraction of PEs holding orders above
    // 2^31 (job 5332555).
    std::vector<uint64_t> work_queue;
    work_queue.push_back(arrIdx);

    while (!work_queue.empty()) {
        uint64_t idx = work_queue.back();
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
                // Drain-once: without this clear, any later cascade through a
                // vertex sharing this parent re-pushes the same requestors —
                // including, for a requestor whose parent IS this entry, the
                // requestor itself, which cycles the work queue forever. This
                // path was unreachable while the cache was never populated;
                // the entry-creation fix above made it live and the loop
                // reproduced immediately (10k, 2 processes).
                parentCache[my_parent].requestors.clear();
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
    // Pruning is the size machinery's only consumer: it cannot work under
    // the no-sizes mode (FOF_UF_SIZES=0), where the field is unmaintained.
    if (!sizesEnabled())
        CkAbort("UnionFindLib::prune_components requires FOF_UF_SIZES=1 "
                "(size maintenance was disabled)\n");
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
