struct findBossData {
    uint64_t arrIdx;
    uint64_t partnerOrBossID;
    uint64_t senderID;
    uint64_t isFBOne;

    void pup(PUP::er &p) {
        p|arrIdx;
        p|partnerOrBossID;
        p|senderID;
        p|isFBOne;
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

    void pup(PUP::er &p) {
        p|arrIdx;
        p|v;
    }
};
#endif

struct shortCircuitData {
    uint64_t arrIdx;
    int64_t grandparentID;

    void pup(PUP::er &p) {
        p|arrIdx;
        p|grandparentID;
    }
};
