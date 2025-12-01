#include <getopt.h>
#include <stdio.h>
#include <stdbool.h>
#include <memory.h>
#include <interconnect.h>

typedef enum _bus_req_state
{
    NONE,
    QUEUED,
    TRANSFERING_CACHE,
    TRANSFERING_MEMORY,
    WAITING_CACHE,
    WAITING_MEMORY
} bus_req_state;

typedef struct _bus_req {
    bus_req_type brt;
    bus_req_state currentState;
    uint64_t addr;
    int procNum;
    uint8_t shared;
    uint8_t data;
    uint8_t dataAvail;
    struct _bus_req* next;
    int pSrc;           // processor that sent message originally
    int pDest;          // destination processor (not used if broadcast)
    int msgNum;         // numerical ID of msg sent (or ID of msg being ACK'd)
    bool broadcast;     // send to all other processors
    bool ack;           // packet is an ACK packet
} bus_req;

bus_req* pendingRequest = NULL;
bus_req** lnkRequests;
bus_req** queuedRequests;
interconn* self;
coher* coherComp;
memory* memComp;

int CADSS_VERBOSE = 0;
int processorCount = 1;

static const char* req_state_map[] = {
    [NONE] = "None",
    [QUEUED] = "Queued",
    [TRANSFERING_CACHE] = "Cache-to-Cache Transfer",
    [TRANSFERING_MEMORY] = "Memory Transfer",
    [WAITING_CACHE] = "Waiting for Cache",
    [WAITING_MEMORY] = "Waiting for Memory",
};

static const char* req_type_map[]
    = {[NO_REQ] = "None", [BUSRD] = "BusRd",   [BUSWR] = "BusRdX",
       [DATA] = "Data",   [SHARED] = "Shared", [MEMORY] = "Memory"};

const int CACHE_DELAY = 10;
const int CACHE_TRANSFER = 10;

void registerCoher(coher* cc);
void busReq(bus_req_type brt, uint64_t addr, int procNum);
int busReqCacheTransfer(uint64_t addr, int procNum);
void printInterconnState(void);
void interconnNotifyState(void);

// Topology: 0 = bus, 1 = line, 2 = ring, 3 = mesh, 4 = crossbar
int8_t t = 0;
int64_t* perProcMsgCount;
int64_t globalMsgCount = 0;


// Link representing a connection between two nodes
typedef struct _link {
    int proc1;          // proc on link with lower ID
    int proc2;          // proc on link with higher ID
    int countDown;      // num ticks before reuse
    bus_req* pendingReq; // current request being sent on link
    bus_req* linkQueue; // queue of requests waiting to use link
    bool p1Sent;        // used to alternate processor sending if both want to
} link;


// Metadata needed for non-bus topologies
link** links;            // array of links in the network (index is link)

/**
 * 2D array of IDs of last msgs received
 * First index by processor receiving messages, then by processor who sent
 * Needed to prevent broadcast storms in networks with cycles
 * Use for ring and mesh
 *
 * Alternatively, could do broadcast by plotting path through network
 * Start and end with node that sent the broadcast
 * Know all have received when broadcast reaches original sender
 */
int*** last_msgs;

// Helper methods for per-processor request queues.
static void enqBusRequest(bus_req* pr, int procNum)
{
    bus_req* iter;

    // No items in the queue.
    if (!queuedRequests[procNum])
    {
        queuedRequests[procNum] = pr;
        return;
    }

    // Add request to the end of the queue.
    iter = queuedRequests[procNum];
    while (iter->next)
    {
        iter = iter->next;
    }

    pr->next = NULL;
    iter->next = pr;
}

static bus_req* deqBusRequest(int procNum)
{
    bus_req* ret;

    ret = queuedRequests[procNum];

    // Move the head to the next request (if there is one).
    if (ret)
    {
        queuedRequests[procNum] = ret->next;
    }

    return ret;
}

static int busRequestQueueSize(int procNum)
{
    int count = 0;
    bus_req* iter;

    if (!queuedRequests[procNum])
    {
        return 0;
    }

    iter = queuedRequests[procNum];
    while (iter)
    {
        iter = iter->next;
        count++;
    }

    return count;
}

//helpers for link queues
static void enqLinkRequest(bus_req* br, link* lnk)
{
    bus_req* iter;

    // No items in the queue.
    if (!lnk->linkQueue)
    {
        lnk->linkQueue = br;
        return;
    }

    // Add request to the end of the queue.
    iter = lnk->linkQueue;
    while (iter->next)
    {
        iter = iter->next;
    }

    br->next = NULL;
    iter->next = br;
}

static bus_req* deqLinkRequest(link* lnk)
{
    bus_req* ret;

    ret = lnk->linkQueue;

    // Move the head to the next request (if there is one).
    if (ret)
    {
        lnk->linkQueue = ret->next;
    }

    return ret;
}

static int linkRequestQueueSize(link* lnk)
{
    int count = 0;
    bus_req* iter;

    if (!lnk->linkQueue)
    {
        return 0;
    }

    iter = lnk->linkQueue;
    while (iter)
    {
        iter = iter->next;
        count++;
    }

    return count;
}

interconn* init(inter_sim_args* isa)
{
    int op;

    while ((op = getopt(isa->arg_count, isa->arg_list, "t:")) != -1)
    {
        switch (op)
        {
            // Topology
            case 't':
                t = atoi(optarg);

            default:
                break;
        }
    }

    if (t == 0 || processorCount == 1) {
        queuedRequests = malloc(sizeof(bus_req*) * processorCount);
        for (int i = 0; i < processorCount; i++)
        {
            queuedRequests[i] = NULL;
        }
    }
    if ((t == 1 && processorCount > 1) || (t > 1 && processorCount == 2)) {
        // n-1 links for the line topology, each connect i and i+1
        links = malloc(sizeof(link*) * (processorCount - 1));
        lnkRequests = calloc(sizeof(bus_req*), (processorCount - 1));
        for (int i = 0; i < processorCount - 1; i++) {
            links[i] = malloc(sizeof(link));
            links[i]->proc1 = i;
            links[i]->proc2 = i+1;
            links[i]->countDown = 0;
            links[i]->pendingReq = NULL;
            links[i]->linkQueue = NULL;
            links[i]->p1Sent = false;
        }
        // no live messages yet, so each list is NULL
        perProcMsgCount = calloc(sizeof(int64_t), processorCount);
        globalMsgCount = 0;
    }

    self = malloc(sizeof(interconn));
    self->busReq = busReq;
    self->registerCoher = registerCoher;
    self->busReqCacheTransfer = busReqCacheTransfer;
    self->si.tick = tick;
    self->si.finish = finish;
    self->si.destroy = destroy;

    memComp = isa->memory;
    memComp->registerInterconnect(self);

    return self;
}

int countDown = 0;
int lastProc = 0; // for round robin arbitration

void registerCoher(coher* cc)
{
    coherComp = cc;
}

void memReqCallback(int procNum, uint64_t addr)
{
    if (!pendingRequest)
    {
        return;
    }

    if (addr == pendingRequest->addr && procNum == pendingRequest->procNum)
    {
        pendingRequest->dataAvail = 1;
    }
}

void busReq(bus_req_type brt, uint64_t addr, int procNum)
{
    if (pendingRequest == NULL)
    {
        assert(brt != SHARED);

        bus_req* nextReq = calloc(1, sizeof(bus_req));
        nextReq->brt = brt;
        nextReq->currentState = WAITING_CACHE;
        nextReq->addr = addr;
        nextReq->procNum = procNum;
        nextReq->dataAvail = 0;

        pendingRequest = nextReq;
        countDown = CACHE_DELAY;

        return;
    }
    else if (brt == SHARED && pendingRequest->addr == addr)
    {
        pendingRequest->shared = 1;
        return;
    }
    else if (brt == DATA && pendingRequest->addr == addr)
    {
        assert(pendingRequest->currentState == WAITING_MEMORY);
        pendingRequest->data = 1;
        pendingRequest->currentState = TRANSFERING_CACHE;
        countDown = CACHE_TRANSFER;
        return;
    }
    else
    {
        assert(brt != SHARED);

        bus_req* nextReq = calloc(1, sizeof(bus_req));
        nextReq->brt = brt;
        nextReq->currentState = QUEUED;
        nextReq->addr = addr;
        nextReq->procNum = procNum;
        nextReq->dataAvail = 0;

        enqBusRequest(nextReq, procNum);
    }
}

link* findLink(int procNum, int pDest){
    if (t == 1) {
        //pDest is not necessarily next to procNum, so find correct link in the direction of pDest
        for (int i = 0; i < processorCount - 1; i++) {
            link* lnk = links[i];
            if ((procNum == lnk->proc1 && pDest >= lnk->proc2) ||
                (procNum == lnk->proc2 && pDest <= lnk->proc1)) {
                return lnk;
            }
        }
    }
}

void req(bus_req_type brt, uint64_t addr, int procNum, int pDest, bool broadcast) {
    if (t == 0 || processorCount == 1) {
        busReq(brt, addr, procNum);
    }
    else if (t == 1 && processorCount > 1) {
        bus_req* nextReq = calloc(1, sizeof(bus_req));
        nextReq->brt = brt;
        nextReq->currentState = QUEUED;
        nextReq->addr = addr;
        nextReq->procNum = procNum;
        nextReq->dataAvail = 0;
        nextReq->pSrc = procNum;
        nextReq->pDest = pDest;
        nextReq->broadcast = broadcast;
        nextReq->msgNum = globalMsgCount++; //msgCount increment

        if (broadcast) {
            //send both ways so add to queue of both links
            for (int i = 0; i < processorCount - 1; i++) {
                link* lnk = links[i];
                if (procNum == lnk->proc1 || procNum == lnk->proc2) {
                    enqLinkRequest(nextReq, lnk);
                }
            }
        }
        else {
            //send one way, find the direction that is shortest for current topology
            link* lnk = findLink(procNum, pDest);
            enqLinkRequest(nextReq, lnk);
        }
    }
}

bool forwardIfNeeded(bus_req* br, link* lnk) {
    int cameFrom = br->procNum;
    int goingTo;
    if (cameFrom == lnk->proc1) {
        goingTo = lnk->proc2;
    }
    else {
        goingTo = lnk->proc1;
    }
    //check of goingTo needs to pass on the message(add to queue of goingTo)
    if (br->msgNum <= perProcMsgCount[goingTo]) {
        //stale message, do not forward
        //this seems sketch, need to check again
        return true;
    }
    if (goingTo == br->pDest) {
        perProcMsgCount[goingTo] = br->msgNum;
        return false;
    }
    else {
        //forward
        perProcMsgCount[goingTo] = br->msgNum;
        req(br->brt, br->addr, goingTo, br->pDest, br->broadcast);
        //if broadcast, goinTo should still act, otherwise just forward and ignore
        if (!br->broadcast) {
            return true;
        }
        return false;
    }
}

void busTick() {
    if (countDown > 0)
    {
        assert(pendingRequest != NULL);
        countDown--;

        // If the count-down has elapsed (or there hasn't been a
        // cache-to-cache transfer, the memory will respond with
        // the data.
        if (pendingRequest->dataAvail)
        {
            pendingRequest->currentState = TRANSFERING_MEMORY;
            countDown = 0;
        }

        if (countDown == 0)
        {
            if (pendingRequest->currentState == WAITING_CACHE)
            {
                // Make a request to memory.
                countDown
                    = memComp->busReq(pendingRequest->addr,
                                      pendingRequest->procNum, memReqCallback);

                pendingRequest->currentState = WAITING_MEMORY;

                // The processors will snoop for this request as well.
                for (int i = 0; i < processorCount; i++)
                {
                    if (pendingRequest->procNum != i)
                    {
                        coherComp->busReq(pendingRequest->brt,
                                          pendingRequest->addr, i);
                    }
                }

                if (pendingRequest->data == 1)
                {
                    pendingRequest->brt = DATA;
                }
            }
            else if (pendingRequest->currentState == TRANSFERING_MEMORY)
            {
                bus_req_type brt
                    = (pendingRequest->shared == 1) ? SHARED : DATA;
                coherComp->busReq(brt, pendingRequest->addr,
                                  pendingRequest->procNum);

                interconnNotifyState();
                free(pendingRequest);
                pendingRequest = NULL;
            }
            else if (pendingRequest->currentState == TRANSFERING_CACHE)
            {
                bus_req_type brt = pendingRequest->brt;
                if (pendingRequest->shared == 1)
                    brt = SHARED;

                coherComp->busReq(brt, pendingRequest->addr,
                                  pendingRequest->procNum);

                interconnNotifyState();
                free(pendingRequest);
                pendingRequest = NULL;
            }
        }
    }
    else if (countDown == 0)
    {
        for (int i = 0; i < processorCount; i++)
        {
            int pos = (i + lastProc) % processorCount;
            if (queuedRequests[pos] != NULL)
            {
                pendingRequest = deqBusRequest(pos);
                countDown = CACHE_DELAY;
                pendingRequest->currentState = WAITING_CACHE;

                lastProc = (pos + 1) % processorCount;
                break;
            }
        }
    }
}

void lineTick() {
    for (int i = 0; i < processorCount - 1; i++) {
        link* lnk = links[i];
        if (lnk->countDown > 0)
            {
            lnk->countDown--;

            // If the count-down has elapsed (or there hasn't been a
            // cache-to-cache transfer, the memory will respond with
            // the data.
            if (lnk->pendingReq->dataAvail)
            {
                pendingRequest->currentState = TRANSFERING_MEMORY;
                lnk->countDown = 0;
            }

            if (lnk->countDown == 0)
            {
                if (lnk->pendingReq->currentState == WAITING_CACHE && 
                    lnk->pendingReq->pSrc == lnk->pendingReq->procNum)
                {
                    // Make a request to memory.
                    lnk->countDown
                        = memComp->busReq(lnk->pendingReq->addr,
                                        lnk->pendingReq->procNum, memReqCallback);

                    lnk->pendingReq->currentState = WAITING_MEMORY;

                    assert(lnk->pendingReq->procNum == lnk->proc1
                        || lnk->pendingReq->procNum == lnk->proc2);

                    if (lnk->pendingReq->procNum == lnk->proc2)
                    {
                        coherComp->busReq(lnk->pendingReq->brt,
                                        lnk->pendingReq->addr, lnk->proc1);
                    }
                    if (lnk->pendingReq->procNum == lnk->proc1)
                    {
                        coherComp->busReq(lnk->pendingReq->brt,
                                        lnk->pendingReq->addr, lnk->proc2);
                    }

                    if (lnk->pendingReq->data == 1)
                    {
                        lnk->pendingReq->brt = DATA;
                    }
                }
                else if (lnk->pendingReq->currentState == TRANSFERING_MEMORY)
                {
                    bus_req_type brt
                        = (lnk->pendingReq->shared == 1) ? SHARED : DATA;
                    coherComp->busReq(brt, lnk->pendingReq->addr,
                                    lnk->pendingReq->procNum);

                    interconnNotifyState();
                    bus_req* temp = lnk->pendingReq;
                    lnk->pendingReq = lnk->pendingReq->next;
                    free(temp);
                }
                else if (lnk->pendingReq->currentState == TRANSFERING_CACHE)
                {
                    bus_req_type brt = lnk->pendingReq->brt;
                    if (lnk->pendingReq->shared == 1)
                        brt = SHARED;

                    coherComp->busReq(brt, lnk->pendingReq->addr,
                                    lnk->pendingReq->procNum);

                    interconnNotifyState();
                    bus_req* temp = lnk->pendingReq;
                    lnk->pendingReq = lnk->pendingReq->next;
                    free(temp);
                }
            }
        }
        else if (lnk->countDown == 0)
        {
            if (lnk->linkQueue == NULL) {
                continue;
            }
            bus_req* nextReq = deqLinkRequest(lnk);
            if (forwardIfNeeded(nextReq, lnk)) {
                //just forward, no need to act, so free
                free(nextReq);
            }
            else {
                lnk->pendingReq = deqLinkRequest(lnk);
                lnk->countDown = CACHE_DELAY;
                lnk->pendingReq->currentState = WAITING_CACHE;
            }
        } 
    }
}

int tick()
{
    memComp->si.tick();

    if (self->dbgEnv.cadssDbgWatchedComp && !self->dbgEnv.cadssDbgNotifyState)
    {
        printInterconnState();
    }

    if (t == 0 || processorCount == 1) {
        busTick();
    }

    if (t == 1 && processorCount > 1) {
        // Line topology tick
        // TODO
    }
    
    return 0;
}

void printInterconnState(void)
{
    if (!pendingRequest)
    {
        return;
    }

    printf("--- Interconnect Debug State (Processors: %d) ---\n"
           "       Current Request: \n"
           "             Processor: %d\n"
           "               Address: 0x%016lx\n"
           "                  Type: %s\n"
           "                 State: %s\n"
           "         Shared / Data: %s\n"
           "                  Next: %p\n"
           "             Countdown: %d\n"
           "    Request Queue Size: \n",
           processorCount, pendingRequest->procNum, pendingRequest->addr,
           req_type_map[pendingRequest->brt],
           req_state_map[pendingRequest->currentState],
           pendingRequest->shared ? "Shared" : "Data", pendingRequest->next,
           countDown);

    for (int p = 0; p < processorCount; p++)
    {
        printf("       - Processor[%02d]: %d\n", p, busRequestQueueSize(p));
    }
}

void interconnNotifyState(void)
{
    if (!pendingRequest)
        return;

    if (self->dbgEnv.cadssDbgExternBreak)
    {
        printInterconnState();
        raise(SIGTRAP);
        return;
    }

    if (self->dbgEnv.cadssDbgWatchedComp && self->dbgEnv.cadssDbgNotifyState)
    {
        self->dbgEnv.cadssDbgNotifyState = 0;
        printInterconnState();
    }
}

// Return a non-zero value if the current request
// was satisfied by a cache-to-cache transfer.
int busReqCacheTransfer(uint64_t addr, int procNum)
{
    assert(pendingRequest);

    if (addr == pendingRequest->addr && procNum == pendingRequest->procNum)
        return (pendingRequest->currentState == TRANSFERING_CACHE);

    return 0;
}

int finish(int outFd)
{
    memComp->si.finish(outFd);
    return 0;
}

int destroy(void)
{
    // TODO
    memComp->si.destroy();
    return 0;
}
