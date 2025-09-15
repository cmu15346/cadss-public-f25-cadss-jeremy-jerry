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
    int processorNum;
    void (*callback)(int, int64_t);
    struct _pendingRequest* next;
    trace_op* op;
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

int getSet(uint64_t addr) {
    return (addr >> b) & ~(~0L << s);
}

int getTag(uint64_t addr) {
    return (addr >> (b + s)) & ~(~0L << (64 - (b + s)));
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

    return self;
}

// This routine is a linkage to the rest of the memory hierarchy
void coherCallback(int type, int processorNum, int64_t addr)
{
    switch (type)
    {
        case NO_ACTION:
            if (pendPermReq->processorNum == processorNum && pendPermReq->addr == addr)
            {
                pendingRequest* pr = pendPermReq;
                pendPermReq = pendPermReq->next;
                pr->next = readyPermReq;
                readyPermReq = pr; 
            }
            else
            {
                pendingRequest* prevReq = pendPermReq;
                pendingRequest* pr = pendPermReq->next;

                while (pr != NULL)
                {
                    if (pr->processorNum == processorNum && pr->addr == addr)
                    {
                        prevReq = pr->next;
                        pr->next = readyPermReq;
                        readyPermReq = pr; 
                        break;
                    }
                    pr = pr->next;
                    prevReq = prevReq->next;
                }
            }
            break;
        case DATA_RECV:
            if (pendReq->processorNum == processorNum && pendReq->addr == addr)
            {
                pendingRequest* pr = pendReq;
                pendReq = pendReq->next;

                pr->next = readyReq;
                readyReq = pr;
            }
            else
            {
                pendingRequest* prevReq = pendReq;
                pendingRequest* pr = pendReq->next;

                while (pr != NULL)
                {
                    if (pr->processorNum == processorNum && pr->addr == addr)
                    {
                        prevReq->next = pr->next;

                        pr->next = readyReq;
                        readyReq = pr;
                        break;
                    }
                    pr = pr->next;
                    prevReq = prevReq->next;
                }

                if (pr == NULL && CADSS_VERBOSE == 1)
                {
                    pr = pendReq;
                    while (pr != NULL)
                    {
                        printf("W: %p (%lx %d)\t", pr, pr->addr, pr->processorNum);
                        pr = pr->next;
                    }
                }
                assert(pr != NULL);
            }
            break;

        case INVALIDATE:
            // This is taught later in the semester.
            break;

        default:
            break;
    }  
}

void memoryRequest(trace_op* op, int processorNum, int64_t tag,
                   void (*callback)(int, int64_t))
{
    assert(op != NULL);
    assert(callback != NULL);
    uint64_t addr = (op->memAddress & ~(blockSize - 1));

    // Simple model to only have one outstanding memory operation

    pendingRequest* pr = malloc(sizeof(pendingRequest));
    pr->tag = tag;
    pr->addr = addr;
    pr->callback = callback;
    pr->processorNum = processorNum;
    pr->op = op;

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
    int victimIndex = -1;
    for (int i = 0; i < lines; i++) {
        if (!set[i]->valid) {
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
            uint8_t perm = coherComp->permReq((op->op == MEM_LOAD), op->memAddress, processorNum);
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

    set[victimIndex]->tag = cacheTag;
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

    // Tell memory about this request
    // TODO: only do this if this is a miss
    // TODO: evictions will also need a call to memory with
    uint8_t invl = coherComp->invlReq(set[victimIndex]->addr, set[victimIndex]->processorNum);
    if (invl == 1){
        pr->next = pendPermReq;
        pendPermReq = pr;
    }
    else{
        pr->next = readyPermReq;
        readyPermReq = pr;
    }
    accessCounter++;
}

int tick()
{
    // Advance ticks in the coherence component.
    printf("Tick\n");
    coherComp->si.tick();
    pendingRequest* pr = readyPermReq;
    while (pr != NULL)
    {
        trace_op* op = pr->op;
        uint8_t perm = coherComp->permReq((op->op == MEM_LOAD), op->memAddress, pr->processorNum);
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
        pr = pr->next;
    }
    readyPermReq = NULL;

    pr = readyReq;
    while (pr != NULL)
    {
        pendingRequest* t = pr;
        pr->callback(pr->processorNum, pr->tag);
        pr = pr->next;
        free(t);
    }
    readyReq = NULL;

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
