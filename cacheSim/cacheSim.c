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
    int64_t addr;
    int64_t evictedAddr;
    int processorNum;
    void (*callback)(int, int64_t);
    trace_op* op;
    struct _pendingRequest* next;
} pendingRequest;

pendingRequest* readyReq = NULL; // ready for callback
pendingRequest* pendReq = NULL; // waiting for permReq
pendingRequest* readyPermReq = NULL; // ready to call permReq after invlReq
pendingRequest* pendPermReq = NULL; // waiting for invlReq

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
    //different from normal tag, only removes block offset as victim cache is fully associative
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

void freeCache() {
    for (int i = 0; i < sets; i++) {
        for (int j = 0; j < lines; j++) {
            free(cacheSets[i][j]);
        }
        free(cacheSets[i]);
    }
    free(cacheSets);
    if (useVictim) {
        for (int i = 0; i < victimEntries; i++) {
            free(victimCache[i]);
        }
        free(victimCache);
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

/**
 * brief Print the linked list of pending requests
 * param head The head of the linked list
 */
void print_list(pendingRequest* head) {
    printf("printing lists\n");
    pendingRequest* curr = head;
    while (curr != NULL) {
        printf("Request tag: %ld, addr: %ld, proc: %d\n", curr->tag, curr->addr, curr->processorNum);
        curr = curr->next;
    }
    printf("end of list\n");
}

// This routine is a linkage to the rest of the memory hierarchy
void coherCallback(int type, int processorNum, int64_t addr)
{
    pendingRequest* pr = NULL;
    switch (type)
    {
        case NO_ACTION:
            pr = pendPermReq;
            if (pr->evictedAddr == addr && pr->processorNum == processorNum) {
                pendPermReq = pr->next;
                pr->next = readyPermReq;
                readyPermReq = pr;
            }
            else{
                pr = pr->next;
                pendingRequest* prevPr = pendPermReq;
                assert(pr != NULL);
                while (pr->evictedAddr != addr || pr->processorNum != processorNum) {
                    prevPr = pr;
                    pr = pr->next;
                    assert(pr != NULL);
                }
                assert(pr->evictedAddr == addr);
                assert(pr->processorNum == processorNum);
                prevPr->next = pr->next;
                pr->next = readyPermReq;
                readyPermReq = pr;
            }
            break;
        case DATA_RECV:
            pr = pendReq;
            if (pr->addr == addr && pr->processorNum == processorNum) {
                pendReq = pr->next;
                pr->next = readyReq;
                readyReq = pr;
            }
            else {
                pr = pr->next;
                pendingRequest* prevPr = pendReq;
                assert(pr != NULL);
                while (pr->addr != addr || pr->processorNum != processorNum) {
                    prevPr = pr;
                    pr = pr->next;
                    assert (pr != NULL);
                }
                assert(pr->addr == addr);
                assert(pr->processorNum == processorNum);
                prevPr->next = pr->next;
                pr->next = readyReq;
                readyReq = pr;
            }
            break;

        case INVALIDATE:
            // This is taught later in the semester.
            break;

        default:
            break;
    }  
}

/**
 * brief Search for a tag in the victim cache, invalidates the line if found
 * param addr The address to search for
 * return The cache line if found, NULL otherwise
 */
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

/**
 * brief Place a cache line into the victim cache, evicting if necessary
 * param line The cache line to place into the victim cache
 * param pr The pending request associated with this operation
 * param isSwap Whether this is a swap from the main cache or a new eviction
 */
void placeInVictimCache(cacheLine *line, pendingRequest *pr, bool isSwap) {
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
            if (!isSwap){
                uint8_t perm = coherComp->permReq((pr->op->op == MEM_LOAD), pr->addr, pr->processorNum);
                if (perm == 1)
                {
                    pr->next = readyReq;
                    readyReq = pr;
                }
                else
                {
                    pr->next = pendReq;
                    pendReq = pr;
                }
            }
            return;
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
    pr->evictedAddr = victimCache[evictIndex]->addr;
    if (invl == 1){
        pr->next = pendPermReq;
        pendPermReq = pr;
    }
    else{
        pr->next = readyPermReq;
        readyPermReq = pr;
    }

    victimCache[evictIndex]->tag = tag;
    victimCache[evictIndex]->valid = true;
    victimCache[evictIndex]->addr = line->addr;
    victimCache[evictIndex]->processorNum = line->processorNum;
    victimCache[evictIndex]->dirty = line->dirty;
    victimCache[evictIndex]->timeStamp = victimCounter;
    victimCounter++;
}

/**
 * brief Handle a cache request, checking for hits/misses and managing evictions
 * param op The trace operation being performed
 * param addr The address being accessed(aligned to block size)
 * param processorNum The processor making the request
 * param tag A tag to identify the request in the callback
 * param callback The callback to invoke when the request is complete
 */
void cacheRequest (trace_op* op, uint64_t addr, int processorNum, int64_t tag,
                   void (*callback)(int, int64_t)) 
{
    pendingRequest* pr = malloc(sizeof(pendingRequest));
    pr->tag = tag;
    pr->addr = addr;
    pr->callback = callback;
    pr->processorNum = processorNum;
    pr->op = op;
    pr->evictedAddr = 0;

    unsigned long cacheTag = getTag(addr);
    cacheLine **set = cacheSets[getSet(addr)];
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
            pr->next = readyReq;
            readyReq = pr;
            accessCounter++;
            return;
        }
    }
    //miss
    bool foundInVictim = false;
    if (useVictim) {
        cacheLine *vLine = findInVictimCache(addr);
        //guarantees that the victim cache now has space for a new line
        if (vLine != NULL) {
            foundInVictim = true;
            pr->next = readyReq;
            readyReq = pr;
        }
    }
    int victimIndex = -1;
    for (int i = 0; i < lines; i++) {
        if (!set[i]->valid) {
            assert(!foundInVictim);
            set[i]->valid = true;
            set[i]->tag = cacheTag;
            set[i]->dirty = (op->op == MEM_STORE);
            set[i]->addr = addr;
            set[i]->processorNum = processorNum;
            if (useRRIP) {
                set[i]->timeStamp = (1 << rripBits) - 2;
            }
            else {
                set[i]->timeStamp = accessCounter;
            }
            uint8_t perm = coherComp->permReq((op->op == MEM_LOAD), addr, processorNum);
            accessCounter++;
            if (perm == 1)
            {
                pr->next = readyReq;
                readyReq = pr;
            }
            else
            {
                pr->next = pendReq;
                pendReq = pr;
            }
            return;
        }
        else {
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
    }
    //eviction
    if (useRRIP) {
        //increment all timestamps
        if (set[victimIndex]->timeStamp < (1 << rripBits) - 1) {
            unsigned long diff = (1 << rripBits) - 1 - set[victimIndex]->timeStamp;
            for (int i = 0; i < lines; i++) {
                set[i]->timeStamp += diff;
            }
        }
    }

    if (useVictim) {
        placeInVictimCache(set[victimIndex], pr, foundInVictim);
    }
    else { 
        uint8_t invl = coherComp->invlReq(set[victimIndex]->addr, set[victimIndex]->processorNum);
        pr->evictedAddr = set[victimIndex]->addr;
        if (invl == 1){
            pr->next = pendPermReq;
            pendPermReq = pr;
        }
        else{
            pr->next = readyPermReq;
            readyPermReq = pr;
        }
    } 
    set[victimIndex]->tag = cacheTag;
    set[victimIndex]->dirty = (op->op == MEM_STORE);
    set[victimIndex]->addr = addr;
    set[victimIndex]->processorNum = processorNum;
    if (useRRIP) {
        set[victimIndex]->timeStamp = (1 << rripBits) - 2;
    }
    else {
        set[victimIndex]->timeStamp = accessCounter;
    }

    accessCounter++;
}

/**
 * brief Handle a memory request, splitting if it crosses a block boundary
 * param op The trace operation being performed
 * param processorNum The processor making the request
 * param tag A tag to identify the request in the callback
 * param callback The callback to invoke when the request is complete
 */
void memoryRequest(trace_op* op, int processorNum, int64_t tag,
                   void (*callback)(int, int64_t))
{
    assert(op != NULL);
    assert(callback != NULL);
    //Aligns address to block size and checks if it crosses a block boundary
    uint64_t addr = op->memAddress;
    int accessSize = op->size;
    uint64_t mask = (uint64_t)(blockSize - 1);
    if ((addr & mask) && ((addr & mask) + accessSize > blockSize)) {
        uint64_t addr1 = addr & (~mask);
        uint64_t addr2 = addr1 + (uint64_t)blockSize;
        cacheRequest(op, addr1, processorNum, tag, callback);
        cacheRequest(op, addr2, processorNum, tag, callback);
    }
    else {
        addr = addr & (~mask);
        cacheRequest(op, addr, processorNum, tag, callback);
    }
}

/**
 * brief Count the number of requests in all lists
 * return The total number of requests
 */
int countList(pendingRequest* head) {
    int count = 0;
    pendingRequest* curr = head;
    while (curr != NULL) {
        printf("Request tag: %ld, addr: %ld, proc: %d\n", curr->tag, curr->addr, curr->processorNum);
        count++;
        curr = curr->next;
    }
    printf("list has %d requests\n", count);
    return count;
}

int tick()
{
    // Advance ticks in the coherence component.
    coherComp->si.tick();
    pendingRequest* pr = readyPermReq;
    while (pr != NULL)
    {
        readyPermReq = readyPermReq->next;
        trace_op* op = pr->op;
        uint8_t perm = coherComp->permReq((op->op == MEM_LOAD), pr->addr, pr->processorNum);
        if (perm == 1)
        {
            pr->next = readyReq;
            readyReq = pr;
        }
        else
        {
            pr->next = pendReq;
            pendReq = pr;
        }  
        pr = readyPermReq;
    }

    pr = readyReq;
    while (pr != NULL)
    {
        pendingRequest* t = pr;
        readyReq = readyReq->next;
        if (readyReq == NULL && pendReq == NULL && readyPermReq == NULL && pendPermReq == NULL) {
            pr->callback(pr->processorNum, pr->tag);
        }
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
    freeCache();
    free(self);
    return 0;
}