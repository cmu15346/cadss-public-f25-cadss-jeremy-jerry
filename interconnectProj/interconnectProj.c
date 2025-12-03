#include <getopt.h>
#include <stdio.h>
#include <stdbool.h>
#include <memory.h>
#include <interconnect.h>
//TODO: interconnect does not send ack, coher will send ack with data/shared if needed

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
    int numAcks;
} bus_req;

bus_req* pendingRequest = NULL;
bus_req** activeRequests = NULL;
bus_req** queuedRequests = NULL;
coher* coherComp;
memory* memComp;
interconn* self = NULL;

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
    bus_req* linkQueue1; // queue of requests waiting to use link, one for each processor
    bus_req* linkQueue2;
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
    int procNum = br->procNum;
    if (procNum != lnk->proc1 && procNum != lnk->proc2) {
        printf("Error: trying to enqueue request from proc %d on link between %d and %d\n",
               procNum, lnk->proc1, lnk->proc2);
        return;
    }
    if (procNum == lnk->proc1) {
        if (lnk->linkQueue1 == NULL) {
            lnk->linkQueue1 = br;
            return;
        }
        iter = lnk->linkQueue1;
    }
    else {
        if (lnk->linkQueue2 == NULL) {
            lnk->linkQueue2 = br;
            return;
        }
        iter = lnk->linkQueue2;
    }

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
    if (lnk->p1Sent) {
        //last time proc1 sent, so now proc2 gets to send if it has something
        if (lnk->linkQueue2 == NULL) {
            //nothing to send, so let proc1 send again
            lnk->p1Sent = true;
            ret = lnk->linkQueue1;
            if (ret)
            {
                lnk->linkQueue1 = ret->next;
            }
        }
        else {
            //proc2 gets to send
            lnk->p1Sent = false;
            ret = lnk->linkQueue2;
            if (ret)
            {
                lnk->linkQueue2 = ret->next;
            }
        }
    }
    else {
        //last time proc2 sent, so now proc1 gets to send if it has something
        if (lnk->linkQueue1 == NULL) {
            //nothing to send, so let proc2 send again
            lnk->p1Sent = false;
            ret = lnk->linkQueue2;
            if (ret)
            {
                lnk->linkQueue2 = ret->next;
            }
        }
        else {
            //proc1 gets to send
            lnk->p1Sent = true;
            ret = lnk->linkQueue1;
            if (ret)
            {
                lnk->linkQueue1 = ret->next;
            }
        }
    }

    return ret;
}

static int linkRequestQueueSize(link* lnk)
{
    int count = 0;
    bus_req* iter;

    if (!lnk->linkQueue1 && !lnk->linkQueue2)
    {
        return 0;
    }

    iter = lnk->linkQueue1;
    while (iter)
    {
        iter = iter->next;
        count++;
    }
    iter = lnk->linkQueue2;
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
        links = malloc(sizeof(link*) * processorCount);
        for (int i = 0; i < processorCount; i++) {
            links[i] = malloc(sizeof(link));
            links[i]->proc1 = i;
            links[i]->proc2 = i+1;
            links[i]->countDown = 0;
            links[i]->pendingReq = NULL;
            links[i]->linkQueue1 = NULL;
            links[i]->linkQueue2 = NULL;
            links[i]->p1Sent = false;
        }
        // no live messages yet, so each list is NULL
        perProcMsgCount = calloc(sizeof(int64_t), processorCount);
        activeRequests = calloc(sizeof(bus_req*), processorCount);
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
    assert(pDest != processorCount);
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
    assert(procNum != processorCount);
    if (t == 0 || processorCount == 1) {
        busReq(brt, addr, procNum);
    }
    else if (t == 1 && processorCount > 1) {
        //the processor should check the pending requests on its own link(s) and update if this req is related to those
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

int forwardIfNeeded(bus_req* br, link* lnk) {
    int cameFrom = br->procNum;
    int goingTo;
    if (cameFrom == lnk->proc1) {
        goingTo = lnk->proc2;
    }
    else {
        goingTo = lnk->proc1;
    }
    perProcMsgCount[goingTo] = br->msgNum;
    bus_req* fwdReq = malloc(sizeof(bus_req));
    memcpy(fwdReq, br, sizeof(bus_req));
    fwdReq->procNum = goingTo;
    link* nextLink = NULL;
    for (int i = 0; i < processorCount - 1; i++) {
        link* lnk2 = links[i];
        if ((goingTo == lnk2->proc1 && cameFrom != lnk2->proc2) ||
            (goingTo == lnk2->proc2 && cameFrom != lnk2->proc1)) {
            nextLink = lnk2;
            break;
        }
    }
    if (nextLink != NULL) {
        enqLinkRequest(fwdReq, nextLink);
    }
    //if broadcast, goinTo should still act, otherwise just forward and ignore
    if (!br->broadcast) {
        return -1;
    }
    return goingTo;
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
    for (int i = 0; i < processorCount; i++) {
        link* lnk = links[i];
        if (lnk->countDown > 0)
        {
            lnk->countDown--;
            if (lnk->countDown == 0 && lnk->pendingReq->ack == false) {
                bus_req* completedReq = lnk->pendingReq;
                int goingTo = forwardIfNeeded(completedReq, lnk);
                assert(goingTo != completedReq->procNum);
                assert(goingTo == lnk->proc1 || goingTo == lnk->proc2);
                if (goingTo != -1 && goingTo < processorCount) {
                    //if not just forwarding, have the coher component process it
                    coherComp->busReq(completedReq->brt, completedReq->addr,
                                      goingTo);
                    //send ack
                    bus_req* ackReq = malloc(sizeof(bus_req));
                    memcpy(ackReq, completedReq, sizeof(bus_req));
                    ackReq->ack = true;
                    ackReq->pDest = completedReq->pSrc;
                    ackReq->pSrc = goingTo;
                    ackReq->procNum = goingTo;
                    ackReq->broadcast = false;
                    enqLinkRequest(ackReq, lnk);
                }
                else if (goingTo != -1 && goingTo == processorCount) {
                    //going to memory
                    int memCountDown = memComp->busReq(completedReq->addr,
                                      completedReq->pSrc, memReqCallback);
                }
                free(completedReq);

            }
            if (lnk->countDown == 0 && lnk->pendingReq->ack == true) {
                bus_req* completedReq = lnk->pendingReq;
                assert(completedReq->pDest < processorCount); //no acks to memory
                int goingTo = forwardIfNeeded(completedReq, lnk);
                assert(goingTo != completedReq->procNum);
                assert(goingTo == lnk->proc1 || goingTo == lnk->proc2);
                assert(completedReq->broadcast == false);
                if (goingTo == completedReq->pDest) {
                    //ack reached destination
                    bus_req* prev = NULL;
                    bus_req* iter = activeRequests[completedReq->pDest];
                    assert(iter != NULL);
                    if (iter->msgNum == completedReq->msgNum) {
                        //first in chain
                        activeRequests[completedReq->pDest] = iter->next;
                    }
                    else {
                        //find in chain
                        prev = iter;
                        iter = iter->next;
                        assert(iter != NULL);
                        while (iter->msgNum != completedReq->msgNum) {
                            prev = iter;
                            iter = iter->next;
                            assert(iter != NULL);
                        }
                    }
                    if (iter->broadcast) {
                        //if broadcast, need to wait for acks from all processors
                        iter->numAcks++;
                        assert(iter->numAcks <= processorCount - 1);
                        if (iter->numAcks == processorCount - 1) {
                            //all acks received, remove from active requests
                            if (prev != NULL) {
                                prev->next = iter->next;
                            }
                            else {
                                activeRequests[completedReq->pDest] = iter->next;
                            }
                            //TODO: signal completion to processor
                            free(iter);
                        }
                    }
                    else {
                        //not broadcast, so just remove from active requests
                        if (prev != NULL) {
                            prev->next = iter->next;
                        }
                        else {
                            activeRequests[completedReq->pDest] = iter->next;
                        }
                        //TODO: signal completion to processor
                        free(iter);
                    }

                }
            }
        }
        else if (lnk->countDown == 0)
        {
            if (linkRequestQueueSize(lnk) == 0) {
                continue;
            }
            bus_req* nextReq = deqLinkRequest(lnk);
            lnk->pendingReq = deqLinkRequest(lnk);
            if (lnk->pendingReq->procNum == lnk->pendingReq->pSrc) {
                bus_req* copy = malloc(sizeof(bus_req));
                memcpy(copy, lnk->pendingReq, sizeof(bus_req));
                copy->numAcks = 0;
                if (activeRequests[lnk->pendingReq->pSrc] == NULL) {
                    activeRequests[lnk->pendingReq->pSrc] = copy;
                }
                else {
                    //there is already an active request from this processor, so chain it
                    bus_req* iter = activeRequests[lnk->pendingReq->pSrc];
                    while (iter->next != NULL) {
                        iter = iter->next;
                    }
                    iter->next = copy;
                }
            }
            lnk->countDown = CACHE_DELAY;
            lnk->pendingReq->currentState = WAITING_CACHE;
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
