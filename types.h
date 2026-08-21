#ifndef TYPES_H
#define TYPES_H

struct findBossData {
    uint64_t arrIdx;
    uint64_t partnerOrBossID;
    uint64_t senderID;
    uint64_t isFBOne;
    uint64_t targetChareIdx;

    void pup(PUP::er &p) {
        p|arrIdx;
        p|partnerOrBossID;
        p|senderID;
        p|isFBOne;
        p|targetChareIdx;
    }
};

struct needBossData {
    uint64_t arrIdx;
    uint64_t senderID;

    void pup(PUP::er &p) {
        p|arrIdx;
        p|senderID;
    }
};

#ifdef ANCHOR_ALGO
struct anchorData {
    uint64_t arrIdx;
    uint64_t v;
    uint64_t targetChareIdx;

    void pup(PUP::er &p) {
        p|arrIdx;
        p|v;
        p|targetChareIdx;
    }
};
#endif

// Client-facing API addition: batched union_request payload — one edge per
// entry, submitted per PE via UnionFindLib::union_requests.
struct UFEdge {
    uint64_t a;
    uint64_t b;
    void pup(PUP::er &p) { p|a; p|b; }
};

struct shortCircuitData {
    uint64_t arrIdx;
    int64_t grandparentID;

    void pup(PUP::er &p) {
        p|arrIdx;
        p|grandparentID;
    }
};

#endif
