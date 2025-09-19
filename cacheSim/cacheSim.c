#include <cache.h>
#include <trace.h>

#include <getopt.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <stdbool.h>

#include <coherence.h>

typedef struct _pendingRequest {
    int64_t tag;
    int64_t addr1;
    int64_t addr2;
    void (*callback)(int, int64_t);
    trace_op* op;
    struct _pendingRequest* next;
    int processorNum;
    bool ready1;
    bool ready2;
    bool readyPerm1;
    bool readyPerm2;
} pendingRequest;

pendingRequest* readyReq = NULL;
pendingRequest* pendReq = NULL;
pendingRequest* readyPermReq = NULL;
pendingRequest* pendPermReq = NULL;

typedef struct _cacheLine {
    unsigned long tag;
    unsigned long timeStamp;
    unsigned long addr;
    int processorNum;
    bool valid;
    bool dirty;
} cacheLine;

cache* self = NULL;
coher* coherComp = NULL;
cacheLine*** cacheSets = NULL;
cacheLine** victimCache = NULL;

int processorCount = 1;
int CADSS_VERBOSE = 0;
int countDown = 0;
int blockSize = 1;
int s = 0;
int b = 0;
int sets = 0;
int lines = 0;
int victimEntries = 0;
int rripBits = 0;
bool useVictim = false;
bool useRRIP = false;
unsigned long accessCounter = 0;
unsigned long victimCounter = 0;

uint64_t getSet(uint64_t addr) {
    return (addr >> b) & ~(~0L << s);
}

uint64_t getTag(uint64_t addr) {
    return (addr >> (b + s)) & ~(~0L << (64 - (b + s)));
}

uint64_t getVictimTag(uint64_t addr) {
    return (addr >> b) & ~(~0L << (64 - b));
}

void memoryRequest(trace_op* op, int processorNum, int64_t tag,
                   void (*callback)(int, int64_t));
void coherCallback(int type, int procNum, int64_t addr);

void createCache(int sets, int lines) {
    cacheSets = (cacheLine***)malloc(sizeof(cacheLine**) * sets);
    for (int i = 0; i < sets; i++) {
        cacheSets[i] = (cacheLine**)malloc(sizeof(cacheLine*) * lines);
        for (int j = 0; j < lines; j++) {
            cacheSets[i][j] = (cacheLine*)calloc(sizeof(cacheLine), 1);
        }
    }
}

cache* init(cache_sim_args* csa)
{
    int op;
    int sf = 0;
    int bf = 0;
    int Ef = 0;
    int vf = 0;
    int rf = 0;


    // TODO - get argument list from assignment
    while ((op = getopt(csa->arg_count, csa->arg_list, "E:s:b:i:R:")) != -1)
    {
        switch (op)
        {
            // Lines per set
            case 'E':
                lines = strtoul(optarg, NULL, 10);
                Ef = 1;
                break;

            // Sets per cache
            case 's':
                s = strtoul(optarg, NULL, 10);
                sets = (unsigned long)(1 << s);
                sf = 1;
                break;

            // block size in bits
            case 'b':
                b = strtoul(optarg, NULL, 10);
                blockSize = 0x1 << atoi(optarg);
                bf = 1;
                break;

            // entries in victim cache
            case 'i':
                victimEntries = strtoul(optarg, NULL, 10);
                useVictim = true;
                vf = 1;
                break;

            // bits in a RRIP-based replacement policy
            case 'R':
                rripBits = strtoul(optarg, NULL, 10);
                useRRIP = true;
                rf = 1;
                break;
        }
    }
    if (sf == 0 || bf == 0 || Ef == 0)
    {
        printf("Missing required arguments\n");
        exit(-1);
    }

    self = malloc(sizeof(cache));
    self->memoryRequest = memoryRequest;
    self->si.tick = tick;
    self->si.finish = finish;
    self->si.destroy = destroy;

    coherComp = csa->coherComp;
    coherComp->registerCacheInterface(coherCallback);

    createCache(sets, lines);
    if (useVictim) {
        victimCache = (cacheLine**)malloc(sizeof(cacheLine*) * victimEntries);
        for (int i = 0; i < victimEntries; i++) {
            victimCache[i] = (cacheLine*)calloc(sizeof(cacheLine), 1);
        }
    }

    return self;
}

// This routine is a linkage to the rest of the memory hierarchy
void coherCallback(int type, int processorNum, int64_t addr)
{
    pendingRequest* pr = NULL;
    switch (type)
    {
        case NO_ACTION:
            pr = pendPermReq;
            if (pr->addr1 == addr) {
                pr->readyPerm1 = true;
            }
            else if (pr->addr2 == addr) {
                pr->readyPerm2 = true;
            }
            if (pr->readyPerm1 && pr->readyPerm2) {
                readyPermReq = pr;
                pendPermReq = pr->next;
            }
            break;
        case DATA_RECV:
            pr = pendReq;
            if (pr->addr1 == addr) {
                pr->ready1 = true;
            }
            else if (pr->addr2 == addr) {
                pr->ready2 = true;
            }
            if (pr->ready1 && pr->ready2) {
                readyReq = pr;
                pendReq = pr->next;
            }
            break;

        case INVALIDATE:
            // This is taught later in the semester.
            break;

        default:
            break;
    }  
}

cacheLine *findInVictimCache(uint64_t addr) {
    unsigned long tag = getVictimTag(addr);
    for (int i = 0; i < victimEntries; i++) {
        if (victimCache[i]->valid && victimCache[i]->tag == tag) {
            victimCache[i]->valid = false;
            return victimCache[i];
        }
    }
    return NULL;
}

int placeInVictimCache(cacheLine *line, pendingRequest *pr, bool isSwap) {
    unsigned long tag = getVictimTag(line->addr);
    int evictIndex = -1;
    for (int i = 0; i < victimEntries; i++) {
        if (!victimCache[i]->valid) {
            victimCache[i]->tag = tag;
            victimCache[i]->valid = true;
            victimCache[i]->addr = line->addr;
            victimCache[i]->processorNum = line->processorNum;
            victimCache[i]->dirty = line->dirty;
            victimCache[i]->timeStamp = victimCounter;
            victimCounter++;
            return -1;
        }
        else {
            if (evictIndex == -1) {
                evictIndex = i;
            }
            else {
                if (victimCache[i]->timeStamp < victimCache[evictIndex]->timeStamp) {
                    evictIndex = i;
                }
            }
        }
    }
    assert(!isSwap);
    //evicted from victim cache
    uint8_t invl = coherComp->invlReq(victimCache[evictIndex]->addr, victimCache[evictIndex]->processorNum);

    victimCache[evictIndex]->tag = tag;
    victimCache[evictIndex]->valid = true;
    victimCache[evictIndex]->addr = line->addr;
    victimCache[evictIndex]->processorNum = line->processorNum;
    victimCache[evictIndex]->dirty = line->dirty;
    victimCache[evictIndex]->timeStamp = victimCounter;
    victimCounter++;
    return invl;
}

bool lookUp(uint64_t addr, trace_op* op, unsigned long cacheTag, cacheLine **set) {
    for (int i = 0; i < lines; i++) {
        if (set[i]->valid && set[i]->tag == cacheTag) {
            if (op->op == MEM_STORE) {
                set[i]->dirty = true;
            }
            if (useRRIP) {
                set[i]->timeStamp = 0; // reset timestamp on access
            }
            else {
                set[i]->timeStamp = accessCounter;
            }
            accessCounter++;
            return true;
        }
    }
    return false;
}

int selectForEviction(cacheLine **set) {
    int victimIndex = -1;
    for (int i = 0; i < lines; i++) {
        if (set[i]->valid) {
            if (victimIndex == -1) {
                victimIndex = i;
            }
            else {
                if (useRRIP) {
                    if (set[i]->timeStamp > set[victimIndex]->timeStamp) {
                        victimIndex = i;
                    }
                }
                else {
                    if (set[i]->timeStamp < set[victimIndex]->timeStamp) {
                        victimIndex = i;
                    }
                }
            }
        }
        else {
            return i;
        }
    }
    return victimIndex;
}

pendingRequest *cacheRequest (trace_op* op, uint64_t addr1, uint64_t addr2, int processorNum, int64_t tag,
                   void (*callback)(int, int64_t)) 
{
    bool multiBlock = (addr2 != 0);
    pendingRequest* pr = malloc(sizeof(pendingRequest));
    pr->tag = tag;
    pr->addr1 = addr1;
    if (multiBlock) {
        pr->addr2 = addr2;
    }
    pr->callback = callback;
    pr->processorNum = processorNum;
    pr->op = op;

    pr->ready1 = false;
    pr->ready2 = true;
    unsigned long cacheTag1 = getTag(addr1);
    cacheLine **set1 = cacheSets[getSet(addr1)];
    unsigned long cacheTag2;
    cacheLine **set2;
    if (multiBlock) {
        cacheTag2 = getTag(addr2);
        set2 = cacheSets[getSet(addr2)];
        pr->ready2 = false;
    }
    pr->ready1 = lookUp(addr1, op, cacheTag1, set1);
    if (multiBlock) {
        pr->ready2 = lookUp(addr2, op, cacheTag2, set2);
    }
    if (pr->ready1 && pr->ready2) {
        pr->next = readyReq;
        readyReq = pr;
        return pr;
    }

    //miss
    bool foundInVictim1 = false;
    bool foundInVictim2 = true;
    if (multiBlock) {
        foundInVictim2 = false;
    }
    if (useVictim) {
        if (!pr->ready1) {
            cacheLine *vLine = findInVictimCache(addr1);
            //guarantees that the victim cache now has space for a new line
            if (vLine != NULL) {
                foundInVictim1 = true;
                pr->ready1 = true;
            }
        }
        if (multiBlock && !pr->ready2) {
            cacheLine *vLine = findInVictimCache(addr2);
            //guarantees that the victim cache now has space for a new line
            if (vLine != NULL) {
                foundInVictim2 = true;
                pr->ready2 = true;
            }
        }
    }

    int victimIndex1 = -1;
    int victimIndex2 = -1;
    int invl1 = -1;
    int invl2 = -1;
    if (!pr->ready1) {
        victimIndex1 = selectForEviction(set1);
        if (!set1[victimIndex1]->valid) {
            assert(!foundInVictim1);
            set1[victimIndex1]->valid = true;
            set1[victimIndex1]->tag = cacheTag1;
            set1[victimIndex1]->dirty = (op->op == MEM_STORE);
            set1[victimIndex1]->addr = addr1;
            set1[victimIndex1]->processorNum = processorNum;
            if (useRRIP) {
                set1[victimIndex1]->timeStamp = (1 << rripBits) - 2;
            }
            else {
                set1[victimIndex1]->timeStamp = accessCounter;
            }
            uint8_t perm = coherComp->permReq((op->op == MEM_LOAD), addr1, processorNum);
            accessCounter++;
            if (perm == 1)
            {
                pr->ready1 = true;
            }
        }
        else {
            if (useVictim) {
                invl1 = placeInVictimCache(set1[victimIndex1], pr, foundInVictim1);
            }
            else {
                invl1 = coherComp->invlReq(set1[victimIndex1]->addr, set1[victimIndex1]->processorNum);
            }
        }
        if (useRRIP) {
            //increment all timestamps
            if (set1[victimIndex1]->timeStamp < (1 << rripBits) - 1) {
                unsigned long diff = (1 << rripBits) - 1 - set1[victimIndex1]->timeStamp;
                for (int i = 0; i < lines; i++) {
                    set1[i]->timeStamp += diff;
                }
            }
        }
    }
    if (multiBlock && !pr->ready2) {
        victimIndex2 = selectForEviction(set2);
        if (!set2[victimIndex2]->valid) {
            assert(!foundInVictim2);
            set1[victimIndex2]->valid = true;
            set1[victimIndex2]->tag = cacheTag2;
            set1[victimIndex2]->dirty = (op->op == MEM_STORE);
            set1[victimIndex2]->addr = addr2;
            set1[victimIndex2]->processorNum = processorNum;
            if (useRRIP) {
                set1[victimIndex2]->timeStamp = (1 << rripBits) - 2;
            }
            else {
                set1[victimIndex2]->timeStamp = accessCounter;
            }
            uint8_t perm = coherComp->permReq((op->op == MEM_LOAD), addr2, processorNum);
            accessCounter++;
            if (perm == 1)
            {
                pr->ready2 = true;
            }
        }
        else {
            if (useVictim) {
                invl2 = placeInVictimCache(set2[victimIndex2], pr, foundInVictim2);
            }
            else {
                invl2 = coherComp->invlReq(set2[victimIndex2]->addr, set2[victimIndex2]->processorNum);
            }
        }
        if (useRRIP) {
            //increment all timestamps
            if (set2[victimIndex2]->timeStamp < (1 << rripBits) - 1) {
                unsigned long diff = (1 << rripBits) - 1 - set2[victimIndex2]->timeStamp;
                for (int i = 0; i < lines; i++) {
                    set2[i]->timeStamp += diff;
                }
            }
        }
    }

    if (pr->ready1 && pr->ready2) {
        pr->next = readyReq;
        readyReq = pr;
    }
    else if (invl1 == 1 || invl2 == 1) {
        pr->next = pendPermReq;
        pendPermReq = pr; 
        pr->readyPerm1 = !invl1;
        pr->readyPerm2 = !invl2;
    }
    else {
        pr->next = pendReq;
        pendReq = pr;
    }
    
    if (victimIndex1 != -1) {
        set1[victimIndex1]->tag = cacheTag1;
        set1[victimIndex1]->dirty = (op->op == MEM_STORE);
        set1[victimIndex1]->addr = addr1;
        set1[victimIndex1]->processorNum = processorNum;
        if (useRRIP) {
            set1[victimIndex1]->timeStamp = (1 << rripBits) - 2;
        }
        else {
            set1[victimIndex1]->timeStamp = accessCounter;
        }
    }
    if (multiBlock && victimIndex2 != -1) {
        set2[victimIndex2]->tag = cacheTag2;
        set2[victimIndex2]->dirty = (op->op == MEM_STORE);
        set2[victimIndex2]->addr = addr2;
        set2[victimIndex2]->processorNum = processorNum;
        if (useRRIP) {
            set2[victimIndex2]->timeStamp = (1 << rripBits) - 2;
        }
        else {
            set2[victimIndex2]->timeStamp = accessCounter;
        }
    }

    accessCounter++;
    return pr;
}

void memoryRequest(trace_op* op, int processorNum, int64_t tag,
                   void (*callback)(int, int64_t))
{
    assert(op != NULL);
    assert(callback != NULL);
    uint64_t addr = op->memAddress;
    uint64_t mask = (uint64_t)(blockSize - 1);
    if (addr & mask) {
        uint64_t addr1 = addr & (~mask);
        uint64_t addr2 = addr1 + (uint64_t)blockSize;
        pendingRequest *pr1 = cacheRequest(op, addr1, addr2, processorNum, tag, callback);
        pendingRequest *pr2 = cacheRequest(op, addr2, addr2, processorNum, tag, callback);
    }
    else {
        pendingRequest *pr1 = cacheRequest(op, addr, 0, processorNum, tag, callback);
    }
}

int tick()
{
    // Advance ticks in the coherence component.
    coherComp->si.tick();
    pendingRequest* pr = readyPermReq;
    while (pr != NULL)
    {
        trace_op* op = pr->op;
        bool multiBlock = (pr->addr2 != 0);
        if (!pr->ready1) {
            uint8_t perm1 = coherComp->permReq((op->op == MEM_LOAD), pr->addr1, pr->processorNum);
            if (perm1 == 1) {
                pr->ready1 = true;
            }
        }
        if (multiBlock && !pr->ready2) {
            uint8_t perm2 = coherComp->permReq((op->op == MEM_LOAD), pr->addr2, pr->processorNum);
            if (perm2 == 1) {
                pr->ready2 = true;
            }
        }
        
        if (pr->ready1 && pr->ready2)
        {
            pr->next = readyReq;
            readyReq = pr;
        }
        else
        {
            pr->next = pendReq;
            pendReq = pr;
        }
        readyPermReq = readyPermReq->next;  
        pr = readyPermReq;
    }

    pr = readyReq;
    while (pr != NULL)
    {
        pendingRequest* t = pr;
        readyReq = readyReq->next;
        pr->callback(pr->processorNum, pr->tag);
        free(t);
        pr = readyReq;
    }

    return 1;
}

int finish(int outFd)
{
    return 0;
}

int destroy(void)
{
    // free any internally allocated memory here
    return 0;
}
