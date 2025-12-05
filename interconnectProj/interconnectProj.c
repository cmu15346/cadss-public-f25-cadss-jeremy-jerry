#include <getopt.h>
#include <stdio.h>
#include <stdbool.h>
#include <memory.h>
#include <interconnect.h>
#include <math.h>

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
       [DATA] = "Data",   [SHARED] = "Shared", [MEMORY] = "Memory", [ACK] = "Ack",
       [SHARED_DATA] = "Shared Data"};

const int CACHE_DELAY = 1;
const int CACHE_TRANSFER = 10;

void registerCoher(coher* cc);
void busReq(bus_req_type brt, uint64_t addr, int procNum);
void req(bus_req_type brt, uint64_t addr, int procNum, int pDest, bool broadcast, int msgNum);
int busReqCacheTransfer(uint64_t addr, int procNum);
void printInterconnState(void);
void interconnNotifyState(void);
void printInterconnForLineState(void);
void printInterconnForRingState(void);

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
int** last_msgs;
bus_req* memoryRequests = NULL;
int memoryCountdown = 0;

// used in mesh
int cols;
int rowLinks;
int colLinks;
int numLinks;

int memReqs = 0;
int memReqsReachedMemRing = 0;
int memReqsMade = 0;
int memResponses = 0;
int memRecvs = 0;

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
static void enqLinkRequest(bus_req* r, link* lnk)
{
    bus_req* br = malloc(sizeof(bus_req));
    memcpy(br, r, sizeof(bus_req));
    if (CADSS_VERBOSE) {
        printf("Enqueuing request with ID %d from proc %d (created by proc %d) of type %s on link between proc %d and proc %d\n",
               br->msgNum, br->procNum, br->pSrc, req_type_map[br->brt], lnk->proc1, lnk->proc2);
    } 
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
    ret->next = NULL;
    return ret;
}

static int linkRequestQueueSize(link* lnk)
{
    int count = 0;
    bus_req* iter;

    if (!lnk->linkQueue1 && !lnk->linkQueue2)
    {
        // if (CADSS_VERBOSE) {
        //     printf("Link between proc %d and proc %d has 0 queued requests\n",
        //            lnk->proc1, lnk->proc2);
        // }
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
    if (t == 1 && processorCount > 1) {
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
    if (t == 2 && processorCount > 1) {
        //n links for the ring topology (plus 1 because of memory)
        links = malloc(sizeof(link*) * processorCount);
        for (int i = 0; i < processorCount + 1; i++) {
            links[i] = malloc(sizeof(link));
            links[i]->proc1 = i;
            if (i < processorCount) {
                links[i]->proc2 = i+1;
            }
            else {
                links[i]->proc2 = 0;
            }
            links[i]->countDown = 0;
            links[i]->pendingReq = NULL;
            links[i]->linkQueue1 = NULL;
            links[i]->linkQueue2 = NULL;
            links[i]->p1Sent - false;
        }
        // no live messages yet, so each list is NULL
        perProcMsgCount = calloc(sizeof(int64_t), processorCount);
        activeRequests = calloc(sizeof(bus_req*), processorCount);
        globalMsgCount = 0;
        // last_msg numbers all 0 for each node, and memory
        last_msgs = malloc(sizeof(int*) * (processorCount + 1));
        for (int i = 0; i < processorCount + 1; i++) {
            last_msgs[i] = calloc(sizeof(int), processorCount + 1);
        }
    }
    if (t == 3 && processorCount > 1) {
        // links connect nodes in the same row/column
        cols = (int)sqrt(processorCount + 1);
        if (cols * cols < (processorCount + 1)) {
            cols++;
        }
        int lastRow = (processorCount + 1) % cols;
        int fullRows = (processorCount + 1) / cols;
        if (lastRow == 0) {
            lastRow = cols;
            fullRows--;
        }
        rowLinks = (cols - 1) * fullRows + (lastRow - 1);
        colLinks = cols * (fullRows - 1) + lastRow;
        numLinks = rowLinks + colLinks;
        links = malloc(sizeof(link*) * numLinks);
        int p1Row = 0;
        int p1Col = 0;
        for (int i = 0; i < rowLinks; i++) {
            links[i] = malloc(sizeof(link));
            links[i]->proc1 = p1Row * cols + p1Col;
            links[i]->proc2 = p1Row * cols + p1Col + 1;
            if (CADSS_VERBOSE) {
                printf("Link %d: p1 %d (in row %d, col %d), p2 %d\n", i, links[i]->proc1, p1Row, p1Col, links[i]->proc2);
            }
            links[i]->countDown = 0;
            links[i]->pendingReq = NULL;
            links[i]->linkQueue1 = NULL;
            links[i]->linkQueue2 = NULL;
            links[i]->p1Sent - false;
            if (p1Col < cols - 2) {
                p1Col++;
            }
            else {
                p1Col = 0;
                p1Row++;
            }
        }
        p1Row = 0;
        p1Col = 0;
        for (int i = 0; i < colLinks; i++) {
            links[rowLinks + i] = malloc(sizeof(link));
            links[rowLinks + i]->proc1 = p1Row * cols + p1Col;
            links[rowLinks + i]->proc2 = (p1Row + 1) * cols + p1Col;
            if (CADSS_VERBOSE) {
                printf("Link %d: p1 %d (in row %d, col %d), p2 %d\n", rowLinks + i, links[rowLinks + i]->proc1, p1Row, p1Col, links[rowLinks + i]->proc2);
            }
            links[rowLinks + i]->countDown = 0;
            links[rowLinks + i]->pendingReq = NULL;
            links[rowLinks + i]->linkQueue1 = NULL;
            links[rowLinks + i]->linkQueue2 = NULL;
            links[rowLinks + i]->p1Sent - false;
            if (p1Col < cols - 1) {
                p1Col++;
            }
            else {
                p1Col = 0;
                p1Row++;
            }
        }
        // no live messages yet, so each list is NULL
        perProcMsgCount = calloc(sizeof(int64_t), processorCount);
        activeRequests = calloc(sizeof(bus_req*), processorCount);
        globalMsgCount = 0;
        // last_msg numbers all 0 for each node, and memory
        last_msgs = malloc(sizeof(int*) * (processorCount + 1));
        for (int i = 0; i < processorCount + 1; i++) {
            last_msgs[i] = calloc(sizeof(int), processorCount + 1);
        }
    }

    self = malloc(sizeof(interconn));
    self->req = req;
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
    if (t == 0 || processorCount == 1) {
        if (!pendingRequest)
        {
            return;
        }

        if (addr == pendingRequest->addr && procNum == pendingRequest->procNum)
        {
            pendingRequest->dataAvail = 1;
        }
    }
    else if ((t == 1 || t == 2 || t == 3) && processorCount > 1) {
        memResponses++;
        req(DATA, addr, processorCount, procNum, false, -2);
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
        for (int i = 0; i < processorCount; i++) {
            link* lnk = links[i];
            if ((procNum == lnk->proc1 && pDest >= lnk->proc2) ||
                (procNum == lnk->proc2 && pDest <= lnk->proc1)) {
                return lnk;
            }
        }
    }
    if (t == 2) {
        for (int i = 0; i < processorCount+1; i++) {
            link* lnk = links[i];
            // go right if shorter to increase procNum than to decrease it
            int distRight = pDest - procNum;
            if (distRight < 0) {
                distRight = processorCount - distRight;
            }
            int distLeft = procNum - pDest;
            if (distLeft < 0) {
                distLeft = distLeft * -1;
            }
            bool goRight = distRight < distLeft;
            if ((procNum == lnk->proc1 && goRight) ||
                (procNum == lnk->proc2 && !goRight)) {
                return lnk;
            }
        }
    }
    if (t == 3) {
        // prioritize going to the correct column, then to the correct row
        if (procNum % cols < pDest % cols) {
            // go right if possible
            for (int i = 0; i < rowLinks; i++) {
                link* lnk = links[i];
                if (lnk->proc1 == procNum) {
                    return lnk;
                }
            }
        }
        if (procNum % cols > pDest % cols) {
            // go left if possible
            for (int i = 0; i < rowLinks; i++) {
                link* lnk = links[i];
                if (lnk->proc2 == procNum) {
                    return lnk;
                }
            }
        }
        if (procNum < pDest) {
            // go down if possible
            for (int i = rowLinks; i < numLinks; i++) {
                link* lnk = links[i];
                if (lnk->proc1 == procNum) {
                    return lnk;
                }
            }
        }
        if (procNum > pDest) {
            // go up if possible
            for (int i = rowLinks; i < numLinks; i++) {
                link* lnk = links[i];
                if (lnk->proc2 == procNum) {
                    return lnk;
                }
            }
        }
        printf("Could not find link in correct direction\n");
        assert(false);
    }
}

void req(bus_req_type brt, uint64_t addr, int procNum, int pDest, bool broadcast, int msgNum) {
    assert(procNum != pDest);
    if (t == 0 || processorCount == 1) {
        if (brt == ACK) {
            //coher component should not be sending ACKs on bus topology
            return;
        }
        busReq(brt, addr, procNum);
    }
    else if ((t == 1 || t == 2 || t == 3) && processorCount > 1) {
        //the processor should check the pending requests on its own link(s) and update if this req is related to those
        if (CADSS_VERBOSE) {
            printf("Processor %d requesting %s for address %lx via %s to proc %d\n",
                   procNum, req_type_map[brt], addr,
                   (broadcast) ? "broadcast" : "unicast", pDest);
        }
        int numToUse;
        if (brt == BUSRD || brt == BUSWR) {
            globalMsgCount++;
            numToUse = globalMsgCount;
        }
        else {
            numToUse = msgNum;
        }
        bus_req* nextReq = calloc(1, sizeof(bus_req));
        nextReq->brt = brt;
        nextReq->currentState = QUEUED;
        nextReq->addr = addr;
        nextReq->procNum = procNum;
        nextReq->dataAvail = 0;
        nextReq->pSrc = procNum;
        nextReq->pDest = pDest;
        nextReq->broadcast = broadcast;
        nextReq->msgNum = numToUse;
        if (brt == ACK || brt == DATA || brt == SHARED || brt == SHARED_DATA) {
            //ACKs should not be broadcast
            nextReq->broadcast = false;
            if (procNum != processorCount) {
                nextReq->ack = true;
            }
        }
        else {
            nextReq->ack = false;
        }
        if (brt == DATA || brt == SHARED_DATA) {
            nextReq->data = 1;
        }

        if (broadcast) {
            assert(brt != ACK); //ACKs should not be broadcast
            assert(brt != MEMORY); //memory requests should not be broadcast
            //send both ways so add to queue of both links
            if (t == 1) {
                for (int i = 0; i < processorCount - 1; i++) {
                    link* lnk = links[i];
                    if (procNum == lnk->proc1 || procNum == lnk->proc2) {
                        enqLinkRequest(nextReq, lnk);
                    }
                }
            }
            if (t == 2) {
                // send on both links, even to memory (memory will ignore and forward)
                for (int i = 0; i < processorCount + 1; i++) {
                    link* lnk = links[i];
                    if (procNum == lnk->proc1 || procNum == lnk->proc2) {
                        enqLinkRequest(nextReq, lnk);
                    }
                }
            }
            if (t == 3) {
                // send on all links other than the one you received the message on
                for (int i = 0; i < numLinks; i++) {
                    link* lnk = links[i];
                    if (procNum == lnk->proc1 || procNum == lnk->proc2) {
                        enqLinkRequest(nextReq, lnk);
                    }
                }
            }
        }
        else {
            //send one way, find the direction that is shortest for current topology
            link* lnk = findLink(procNum, pDest);
            enqLinkRequest(nextReq, lnk);
        }
        free(nextReq);
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
    if (CADSS_VERBOSE) {
        //print source and dest of request
        printf("Request is of type %s for address %lx from proc %d to proc %d (broadcast: %s)\n",
               req_type_map[br->brt], br->addr, br->pSrc, br->pDest,
               (br->broadcast) ? "true" : "false");
        //print cameFrom/goingTo
        printf("Came from proc %d, going to proc %d\n", cameFrom, goingTo);
        //print link info
        printf("Link between proc %d and proc %d\n", lnk->proc1, lnk->proc2);
    }
    perProcMsgCount[goingTo] = br->msgNum;
    bus_req* fwdReq = malloc(sizeof(bus_req));
    memcpy(fwdReq, br, sizeof(bus_req));
    fwdReq->procNum = goingTo;
    link* nextLink = NULL;
    if (br->pDest != goingTo || br->broadcast) {
        if (t == 1) {
            for (int i = 0; i < processorCount; i++) {
                if (i == processorCount - 1 && br->brt != MEMORY) {
                    break;
                }
                link* lnk2 = links[i];
                if ((goingTo == lnk2->proc1 && cameFrom != lnk2->proc2) ||
                    (goingTo == lnk2->proc2 && cameFrom != lnk2->proc1)) {
                    nextLink = lnk2;
                    break;
                }
            }
        }
        if (t == 2) {
            for (int i = 0; i < processorCount + 1; i++) {
                link* lnk2 = links[i];
                if ((goingTo == lnk2->proc1 && cameFrom != lnk2->proc2) ||
                    (goingTo == lnk2->proc2 && cameFrom != lnk2->proc1)) {
                    nextLink = lnk2;
                    break;
                }
            }
        }
        if (t == 3) {
            if (br->broadcast) {
                for (int i = 0; i < numLinks; i++) {
                    link* lnk2 = links[i];
                    if ((goingTo == lnk2->proc1 && cameFrom != lnk2->proc2) ||
                        (goingTo == lnk2->proc2 && cameFrom != lnk2->proc1)) {
                        if (nextLink == NULL) {
                            nextLink = lnk2;
                        }
                        else {
                            bus_req* fwdReq2 = malloc(sizeof(bus_req));
                            memcpy(fwdReq2, fwdReq, sizeof(bus_req));
                            enqLinkRequest(fwdReq2, lnk2);
                            free(fwdReq2);
                        }
                    }
                }
            }
            else {
                nextLink = findLink(goingTo, br->pDest);
            }
        }
    }
    if (nextLink != NULL) {
        enqLinkRequest(fwdReq, nextLink);
    }
    free(fwdReq);
    //if broadcast, goinTo should still act, otherwise just forward and ignore
    if (!br->broadcast && nextLink != NULL) {
        //not broadcast and we are also not the destination, so dont act on it
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
                                          pendingRequest->addr, i, -1, -1);
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
                                  pendingRequest->procNum, -1, -1);

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
                                  pendingRequest->procNum, -1, -1);

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

bool checkActiveRequests() {
    for (int i = 0; i < processorCount; i++) {
        //no duplicates should be in active requests
        bus_req* iter = activeRequests[i];
        while (iter != NULL) {
            bus_req* iter2 = iter->next;
            while (iter2 != NULL) {
                if (iter->msgNum == iter2->msgNum) {
                    if (t == 1) {
                        printInterconnForLineState();
                    }
                    if (t == 2) {
                        printInterconnForRingState();
                    }
                    return false;   
                }
                iter2 = iter2->next;
            }
            iter = iter->next;
        }
    }
    return true;
}

int tickCount = 0;
int lastProgressTick = 0;
void lineTick() {
    tickCount++;
    for (int i = 0; i < processorCount; i++) {
        link* lnk = links[i];
        if (lnk->countDown > 0)
        {
            lnk->countDown--;
            if (lnk->pendingReq == NULL) {
                continue;
            }
            if (lnk->countDown == 0 && lnk->pendingReq->ack == false) {
                bus_req* completedReq = lnk->pendingReq;
                lnk->pendingReq = NULL;
                int goingTo = forwardIfNeeded(completedReq, lnk);
                assert(goingTo != completedReq->procNum);
                assert(goingTo == lnk->proc1 || goingTo == lnk->proc2 || goingTo == -1);
                if (!(goingTo == completedReq->pDest || goingTo == -1 ||
                       completedReq->broadcast == true)) {
                        printf("Req that is causing problems:\n addr: 0x%016lx,\n pSrc: %d,\n pDest: %d,\n msgNum: %d,\n procNum: %d,\n link between %d and %d\n",
                               completedReq->addr, completedReq->pSrc, completedReq->pDest,
                               completedReq->msgNum, completedReq->procNum, lnk->proc1, lnk->proc2);
                        printInterconnForLineState();
                }
                assert(goingTo == completedReq->pDest || goingTo == -1 ||
                       completedReq->broadcast == true);
                if (goingTo != -1 && goingTo < processorCount) {
                    if (completedReq->pSrc == processorCount) {
                        memRecvs++;
                    }
                    //if not just forwarding, have the coher component process it
                    coherComp->busReq(completedReq->brt, completedReq->addr,
                                      goingTo, completedReq->pSrc, completedReq->msgNum);
                }
                else if (goingTo == processorCount) {
                    assert(completedReq->brt == MEMORY);
                    assert(completedReq->broadcast == false);
                    assert(completedReq->pDest == processorCount);
                    assert(completedReq->procNum == processorCount - 1);
                    memReqsMade++;
                    int memCountDown = memComp->busReq(completedReq->addr,
                                      completedReq->pSrc, memReqCallback);
                    lnk->countDown = memCountDown;
                }
                free(completedReq);

            }
            else if (lnk->countDown == 0 && lnk->pendingReq->ack == true) {
                bus_req* completedReq = lnk->pendingReq;
                lnk->pendingReq = NULL;
                assert(completedReq->pDest < processorCount); //no acks to memory
                int goingTo = forwardIfNeeded(completedReq, lnk);
                assert(goingTo != completedReq->procNum);
                assert(goingTo == lnk->proc1 || goingTo == lnk->proc2 || goingTo == -1);
                assert(completedReq->broadcast == false);
                if (goingTo == completedReq->pDest) {
                    //ack reached destination
                    if (CADSS_VERBOSE) {
                        printf("ACK for msg %d reached destination proc %d\n",
                               completedReq->msgNum, completedReq->pDest);
                    }
                    bus_req* prev = NULL;
                    bus_req* iter = activeRequests[completedReq->pDest];
                    if (iter == NULL) {
                        printInterconnForLineState();
                        printf("Req that is causing problems:\n addr: 0x%016lx,\n pSrc: %d,\n pDest: %d,\n msgNum: %d,\n procNum: %d,\n link between %d and %d\n",
                               completedReq->addr, completedReq->pSrc, completedReq->pDest,
                               completedReq->msgNum, completedReq->procNum, lnk->proc1, lnk->proc2);
                    }
                    assert(iter != NULL);
                    if (iter->msgNum != completedReq->msgNum) {
                        //find in chain
                        prev = iter;
                        iter = iter->next;
                        assert(iter != NULL);
                        while (iter->msgNum != completedReq->msgNum) {
                            prev = iter;
                            iter = iter->next;
                            if (iter == NULL) {
                                printInterconnForLineState();
                            }
                            assert(iter != NULL);
                        }
                    }
                    if (iter->broadcast) {
                        //if broadcast, need to wait for acks from all processors
                        iter->numAcks++;
                        assert(iter->numAcks <= processorCount - 1);
                        if (completedReq->brt == SHARED_DATA || completedReq->brt == DATA) {
                            //data/shared ack, so mark data as available
                            iter->dataAvail = 1;
                        }
                        if (completedReq->brt == SHARED_DATA || completedReq->brt == SHARED) {
                            //shared data ack, so mark shared as true
                            iter->shared = 1;
                        }
                        if (iter->numAcks == processorCount - 1) {
                            //all acks received, remove from active requests
                            if (CADSS_VERBOSE) {
                                printf("All ACKs for broadcast msg %d received at proc %d\n",
                                       completedReq->msgNum, completedReq->pDest);
                            }
                            if (prev != NULL) {
                                assert(prev->next == iter);
                                prev->next = iter->next;
                            }
                            else {
                                activeRequests[completedReq->pDest] = iter->next;
                            }
                            if (!iter->dataAvail) {
                                memReqs++;
                                req(MEMORY, iter->addr, iter->pSrc, processorCount, false, -2);
                            }
                            else {
                                if (iter->shared) {
                                    //data is shared
                                    coherComp->busReq(SHARED, iter->addr,
                                                      iter->pSrc, -1, -2);
                                }
                                else {
                                    //data is exclusive
                                    coherComp->busReq(DATA, iter->addr,
                                                      iter->pSrc, -1, -2);
                                }
                            }
                            free(iter);
                        }
                    }
                    else {
                        perror("should not be here with no directory\n");
                        exit(1);
                        //not broadcast, so just remove from active requests
                        if (prev != NULL) {
                            prev->next = iter->next;
                        }
                        else {
                            activeRequests[completedReq->pDest] = iter->next;
                        }
                        free(iter);
                    }
                }
                assert(checkActiveRequests());
            }
        }
        else if (lnk->countDown == 0)
        {
            if (linkRequestQueueSize(lnk) == 0) {
                continue;
            }
            lastProgressTick = tickCount;
            bus_req* nextReq = deqLinkRequest(lnk);
            lnk->pendingReq = nextReq;
            if (lnk->pendingReq->procNum == lnk->pendingReq->pSrc &&
                lnk->pendingReq->ack == false &&
                (lnk->pendingReq->brt == BUSRD || lnk->pendingReq->brt == BUSWR)) {
                //print we are waiting for acks for msg with msgnum:
                if (CADSS_VERBOSE) {
                    printf("Tracking active request from proc %d of type %s for address %lx with msgNum %d\n",
                           lnk->pendingReq->pSrc,
                           req_type_map[lnk->pendingReq->brt],
                           lnk->pendingReq->addr,
                           lnk->pendingReq->msgNum);
                }
                assert(lnk->pendingReq->broadcast == true);
                bus_req* copy = malloc(sizeof(bus_req));
                memcpy(copy, lnk->pendingReq, sizeof(bus_req));
                copy->numAcks = 0;
                if (activeRequests[lnk->pendingReq->pSrc] == NULL) {
                    activeRequests[lnk->pendingReq->pSrc] = copy;
                }
                else {
                    //there is already an active request from this processor, so chain it
                    bus_req* iter = activeRequests[lnk->pendingReq->pSrc];
                    bus_req* prev = NULL;
                    bool shouldAdd = true;
                    while (iter!= NULL) {
                        if (iter->msgNum == copy->msgNum) {
                            //already tracking this request
                            shouldAdd = false;
                            free(copy);
                            break;
                        }
                        prev = iter;
                        iter = iter->next;
                    }
                    if (shouldAdd) {
                        assert(prev != NULL);
                        prev->next = copy;
                    }
                }
                assert(checkActiveRequests());
            }
            if (CADSS_VERBOSE) {
                printf("Link between proc %d and proc %d sending req from proc %d of type %s to proc %d\n",
                       lnk->proc1, lnk->proc2, lnk->pendingReq->pSrc,
                       req_type_map[lnk->pendingReq->brt],
                       (lnk->pendingReq->broadcast) ? -1 : lnk->pendingReq->pDest);
            }
            lnk->countDown = CACHE_DELAY;
            lnk->pendingReq->currentState = WAITING_CACHE;
        } 
    }
    // if (tickCount - lastProgressTick > 10000) {
    //     printf("No progress made in 10000 ticks, possible deadlock in interconnect\n");
    //     printInterconnForLineState();
    //     printf("%d requests to memory, %d requests reached memory, and %d responses from memory, %d of which were received\n", memReqs, memReqsMade, memResponses, memRecvs);
    //     raise(SIGTRAP);
    // }
}

void ringTick() {
    tickCount++;
    for (int i = 0; i < processorCount + 1; i++) {
        link* lnk = links[i];
        if (lnk->countDown > 0)
        {
            lnk->countDown--;
            if (lnk->pendingReq == NULL) {
                continue;
            }
            if (lnk->countDown == 0 && lnk->pendingReq->ack == false) {
                bus_req* completedReq = lnk->pendingReq;
                lnk->pendingReq = NULL;
                int arrivedAt;
                if (lnk->proc1 != completedReq->procNum) {
                    assert(completedReq->procNum == lnk->proc2);
                    arrivedAt = lnk->proc1;
                }
                else {
                    assert(completedReq->procNum == lnk->proc1);
                    assert(completedReq->procNum != lnk->proc2);
                    arrivedAt = lnk->proc2;
                }
                int goingTo = -1;
                if ((last_msgs[arrivedAt][completedReq->pSrc] < completedReq->msgNum && arrivedAt != completedReq->pSrc) || !completedReq->broadcast) {
                    goingTo = forwardIfNeeded(completedReq, lnk);
                    if (completedReq->broadcast) {
                        last_msgs[arrivedAt][completedReq->pSrc] = completedReq->msgNum;
                    }
                    assert(goingTo == -1 || goingTo == arrivedAt);
                }
                assert(goingTo != completedReq->procNum);
                assert(goingTo == lnk->proc1 || goingTo == lnk->proc2 || goingTo == -1);
                if (!(goingTo == completedReq->pDest || goingTo == -1 ||
                       completedReq->broadcast == true)) {
                        printf("Req that is causing problems:\n addr: 0x%016lx,\n pSrc: %d,\n pDest: %d,\n msgNum: %d,\n procNum: %d,\n link between %d and %d\n",
                               completedReq->addr, completedReq->pSrc, completedReq->pDest,
                               completedReq->msgNum, completedReq->procNum, lnk->proc1, lnk->proc2);
                        printInterconnForRingState();
                }
                assert(goingTo == completedReq->pDest || goingTo == -1 ||
                       completedReq->broadcast == true);
                if (goingTo != -1 && goingTo < processorCount) {
                    if (completedReq->pSrc == processorCount) {
                        memRecvs++;
                    }
                    //if not just forwarding, have the coher component process it
                    coherComp->busReq(completedReq->brt, completedReq->addr,
                                      goingTo, completedReq->pSrc, completedReq->msgNum);
                }
                else if (goingTo == processorCount && completedReq->pDest == processorCount) {
                    assert(completedReq->brt == MEMORY);
                    assert(completedReq->broadcast == false);
                    assert(completedReq->pDest == processorCount);
                    assert(completedReq->procNum == processorCount - 1 || completedReq->procNum == 0);
                    // add to end of memory requests list
                    bus_req* new_mem_req = memoryRequests;
                    bus_req* prev_mem_req = NULL;
                    while (new_mem_req != NULL) {
                        prev_mem_req = new_mem_req;
                        new_mem_req = new_mem_req->next;
                    }
                    new_mem_req = malloc(sizeof(bus_req));
                    memcpy(new_mem_req, completedReq, sizeof(bus_req));
                    new_mem_req->next = NULL;
                    if (prev_mem_req != NULL) {
                        prev_mem_req->next = new_mem_req;
                    }
                    else {
                        memoryRequests = new_mem_req;
                    }
                    memReqsReachedMemRing++;
                }
                free(completedReq);

            }
            else if (lnk->countDown == 0 && lnk->pendingReq->ack == true) {
                bus_req* completedReq = lnk->pendingReq;
                lnk->pendingReq = NULL;
                int goingTo = forwardIfNeeded(completedReq, lnk);
                assert(goingTo != completedReq->procNum);
                assert(goingTo == lnk->proc1 || goingTo == lnk->proc2 || goingTo == -1);
                assert(completedReq->broadcast == false);
                if (goingTo == completedReq->pDest) {
                    assert(completedReq->pDest < processorCount); //no acks to memory
                    //ack reached destination
                    if (CADSS_VERBOSE) {
                        printf("ACK for msg %d reached destination proc %d\n",
                               completedReq->msgNum, completedReq->pDest);
                    }
                    bus_req* prev = NULL;
                    bus_req* iter = activeRequests[completedReq->pDest];
                    if (iter == NULL) {
                        printInterconnForRingState();
                        printf("Req that is causing problems:\n addr: 0x%016lx,\n pSrc: %d,\n pDest: %d,\n msgNum: %d,\n procNum: %d,\n link between %d and %d\n",
                               completedReq->addr, completedReq->pSrc, completedReq->pDest,
                               completedReq->msgNum, completedReq->procNum, lnk->proc1, lnk->proc2);
                    }
                    assert(iter != NULL);
                    if (iter->msgNum != completedReq->msgNum) {
                        //find in chain
                        prev = iter;
                        iter = iter->next;
                        assert(iter != NULL);
                        while (iter->msgNum != completedReq->msgNum) {
                            prev = iter;
                            iter = iter->next;
                            if (iter == NULL) {
                                printInterconnForRingState();
                            }
                            assert(iter != NULL);
                        }
                    }
                    if (iter->broadcast) {
                        //if broadcast, need to wait for acks from all processors
                        iter->numAcks++;
                        assert(iter->numAcks <= processorCount - 1);
                        if (completedReq->brt == SHARED_DATA || completedReq->brt == DATA) {
                            //data/shared ack, so mark data as available
                            iter->dataAvail = 1;
                        }
                        if (completedReq->brt == SHARED_DATA || completedReq->brt == SHARED) {
                            //shared data ack, so mark shared as true
                            iter->shared = 1;
                        }
                        if (iter->numAcks == processorCount - 1) {
                            //all acks received, remove from active requests
                            if (CADSS_VERBOSE) {
                                printf("All ACKs for broadcast msg %d received at proc %d\n",
                                       completedReq->msgNum, completedReq->pDest);
                            }
                            if (prev != NULL) {
                                assert(prev->next == iter);
                                prev->next = iter->next;
                            }
                            else {
                                activeRequests[completedReq->pDest] = iter->next;
                            }
                            if (!iter->dataAvail) {
                                memReqs++;
                                req(MEMORY, iter->addr, iter->pSrc, processorCount, false, -2);
                            }
                            else {
                                if (iter->shared) {
                                    //data is shared
                                    coherComp->busReq(SHARED, iter->addr,
                                                      iter->pSrc, -1, -2);
                                }
                                else {
                                    //data is exclusive
                                    coherComp->busReq(DATA, iter->addr,
                                                      iter->pSrc, -1, -2);
                                }
                            }
                            free(iter);
                        }
                    }
                    else {
                        perror("should not be here with no directory\n");
                        exit(1);
                        //not broadcast, so just remove from active requests
                        if (prev != NULL) {
                            prev->next = iter->next;
                        }
                        else {
                            activeRequests[completedReq->pDest] = iter->next;
                        }
                        free(iter);
                    }
                }
                assert(checkActiveRequests());
            }
        }
        else if (lnk->countDown == 0)
        {
            if (linkRequestQueueSize(lnk) == 0) {
                continue;
            }
            lastProgressTick = tickCount;
            bus_req* nextReq = deqLinkRequest(lnk);
            lnk->pendingReq = nextReq;
            if (lnk->pendingReq->procNum == lnk->pendingReq->pSrc &&
                lnk->pendingReq->ack == false &&
                (lnk->pendingReq->brt == BUSRD || lnk->pendingReq->brt == BUSWR)) {
                //print we are waiting for acks for msg with msgnum:
                if (CADSS_VERBOSE) {
                    printf("Tracking active request from proc %d of type %s for address %lx with msgNum %d\n",
                           lnk->pendingReq->pSrc,
                           req_type_map[lnk->pendingReq->brt],
                           lnk->pendingReq->addr,
                           lnk->pendingReq->msgNum);
                }
                assert(lnk->pendingReq->broadcast == true);
                bus_req* copy = malloc(sizeof(bus_req));
                memcpy(copy, lnk->pendingReq, sizeof(bus_req));
                copy->numAcks = 0;
                if (activeRequests[lnk->pendingReq->pSrc] == NULL) {
                    activeRequests[lnk->pendingReq->pSrc] = copy;
                }
                else {
                    //there is already an active request from this processor, so chain it
                    bus_req* iter = activeRequests[lnk->pendingReq->pSrc];
                    bus_req* prev = NULL;
                    bool shouldAdd = true;
                    while (iter!= NULL) {
                        if (iter->msgNum == copy->msgNum) {
                            //already tracking this request
                            shouldAdd = false;
                            free(copy);
                            break;
                        }
                        prev = iter;
                        iter = iter->next;
                    }
                    if (shouldAdd) {
                        assert(prev != NULL);
                        prev->next = copy;
                    }
                }
                assert(checkActiveRequests());
            }
            if (CADSS_VERBOSE) {
                printf("Link between proc %d and proc %d sending req from proc %d of type %s to proc %d\n",
                       lnk->proc1, lnk->proc2, lnk->pendingReq->pSrc,
                       req_type_map[lnk->pendingReq->brt],
                       (lnk->pendingReq->broadcast) ? -1 : lnk->pendingReq->pDest);
            }
            lnk->countDown = CACHE_DELAY;
            lnk->pendingReq->currentState = WAITING_CACHE;
        } 
    }
    if (memoryCountdown > 0) {
        memoryCountdown--;
    }
    if (memoryCountdown == 0 && memoryRequests != NULL) {
        memReqsMade++;
        bus_req* thisRequest = memoryRequests;
        int memCountDown = memComp->busReq(thisRequest->addr,
                        thisRequest->pSrc, memReqCallback);
        memoryCountdown = memCountDown;
        memoryRequests = memoryRequests->next;
        free(thisRequest);
    }
    // if (tickCount - lastProgressTick > 10000) {
    //     printf("No progress made in 10000 ticks, possible deadlock in interconnect\n");
    //     printInterconnForRingState();
    //     printf("%d requests to memory, %d requests reached memory, %d requests made to memory, and %d responses from memory, %d of which were received\n", memReqs, memReqsReachedMemRing, memReqsMade, memResponses, memRecvs);
    //     raise(SIGTRAP);
    // }
}

void meshTick() {
    tickCount++;
    for (int i = 0; i < numLinks; i++) {
        link* lnk = links[i];
        if (lnk->countDown > 0)
        {
            lnk->countDown--;
            if (lnk->pendingReq == NULL) {
                continue;
            }
            if (lnk->countDown == 0 && lnk->pendingReq->ack == false) {
                bus_req* completedReq = lnk->pendingReq;
                lnk->pendingReq = NULL;
                int arrivedAt;
                if (lnk->proc1 != completedReq->procNum) {
                    assert(completedReq->procNum == lnk->proc2);
                    arrivedAt = lnk->proc1;
                }
                else {
                    assert(completedReq->procNum == lnk->proc1);
                    assert(completedReq->procNum != lnk->proc2);
                    arrivedAt = lnk->proc2;
                }
                int goingTo = -1;
                if ((last_msgs[arrivedAt][completedReq->pSrc] < completedReq->msgNum && arrivedAt != completedReq->pSrc) || !completedReq->broadcast) {
                    goingTo = forwardIfNeeded(completedReq, lnk);
                    if (completedReq->broadcast) {
                        last_msgs[arrivedAt][completedReq->pSrc] = completedReq->msgNum;
                    }
                    assert(goingTo == -1 || goingTo == arrivedAt);
                }
                assert(goingTo != completedReq->procNum);
                assert(goingTo == lnk->proc1 || goingTo == lnk->proc2 || goingTo == -1);
                if (!(goingTo == completedReq->pDest || goingTo == -1 ||
                       completedReq->broadcast == true)) {
                        printf("Req that is causing problems:\n addr: 0x%016lx,\n pSrc: %d,\n pDest: %d,\n msgNum: %d,\n procNum: %d,\n link between %d and %d\n",
                               completedReq->addr, completedReq->pSrc, completedReq->pDest,
                               completedReq->msgNum, completedReq->procNum, lnk->proc1, lnk->proc2);
                        printInterconnForRingState();
                }
                assert(goingTo == completedReq->pDest || goingTo == -1 ||
                       completedReq->broadcast == true);
                if (goingTo != -1 && goingTo < processorCount) {
                    if (completedReq->pSrc == processorCount) {
                        memRecvs++;
                    }
                    //if not just forwarding, have the coher component process it
                    coherComp->busReq(completedReq->brt, completedReq->addr,
                                      goingTo, completedReq->pSrc, completedReq->msgNum);
                }
                else if (goingTo == processorCount && completedReq->pDest == processorCount) {
                    assert(completedReq->brt == MEMORY);
                    assert(completedReq->broadcast == false);
                    assert(completedReq->pDest == processorCount);
                    // add to end of memory requests list
                    bus_req* new_mem_req = memoryRequests;
                    bus_req* prev_mem_req = NULL;
                    while (new_mem_req != NULL) {
                        prev_mem_req = new_mem_req;
                        new_mem_req = new_mem_req->next;
                    }
                    new_mem_req = malloc(sizeof(bus_req));
                    memcpy(new_mem_req, completedReq, sizeof(bus_req));
                    new_mem_req->next = NULL;
                    if (prev_mem_req != NULL) {
                        prev_mem_req->next = new_mem_req;
                    }
                    else {
                        memoryRequests = new_mem_req;
                    }
                    memReqsReachedMemRing++;
                }
                free(completedReq);

            }
            else if (lnk->countDown == 0 && lnk->pendingReq->ack == true) {
                bus_req* completedReq = lnk->pendingReq;
                lnk->pendingReq = NULL;
                int goingTo = forwardIfNeeded(completedReq, lnk);
                assert(goingTo != completedReq->procNum);
                assert(goingTo == lnk->proc1 || goingTo == lnk->proc2 || goingTo == -1);
                assert(completedReq->broadcast == false);
                if (goingTo == completedReq->pDest) {
                    assert(completedReq->pDest < processorCount); //no acks to memory
                    //ack reached destination
                    if (CADSS_VERBOSE) {
                        printf("ACK for msg %d reached destination proc %d\n",
                               completedReq->msgNum, completedReq->pDest);
                    }
                    bus_req* prev = NULL;
                    bus_req* iter = activeRequests[completedReq->pDest];
                    if (iter == NULL) {
                        printInterconnForRingState();
                        printf("Req that is causing problems:\n addr: 0x%016lx,\n pSrc: %d,\n pDest: %d,\n msgNum: %d,\n procNum: %d,\n link between %d and %d\n",
                               completedReq->addr, completedReq->pSrc, completedReq->pDest,
                               completedReq->msgNum, completedReq->procNum, lnk->proc1, lnk->proc2);
                    }
                    assert(iter != NULL);
                    if (iter->msgNum != completedReq->msgNum) {
                        //find in chain
                        prev = iter;
                        iter = iter->next;
                        assert(iter != NULL);
                        while (iter->msgNum != completedReq->msgNum) {
                            prev = iter;
                            iter = iter->next;
                            if (iter == NULL) {
                                printInterconnForRingState();
                            }
                            assert(iter != NULL);
                        }
                    }
                    if (iter->broadcast) {
                        //if broadcast, need to wait for acks from all processors
                        iter->numAcks++;
                        assert(iter->numAcks <= processorCount - 1);
                        if (completedReq->brt == SHARED_DATA || completedReq->brt == DATA) {
                            //data/shared ack, so mark data as available
                            iter->dataAvail = 1;
                        }
                        if (completedReq->brt == SHARED_DATA || completedReq->brt == SHARED) {
                            //shared data ack, so mark shared as true
                            iter->shared = 1;
                        }
                        if (iter->numAcks == processorCount - 1) {
                            //all acks received, remove from active requests
                            if (CADSS_VERBOSE) {
                                printf("All ACKs for broadcast msg %d received at proc %d\n",
                                       completedReq->msgNum, completedReq->pDest);
                            }
                            if (prev != NULL) {
                                assert(prev->next == iter);
                                prev->next = iter->next;
                            }
                            else {
                                activeRequests[completedReq->pDest] = iter->next;
                            }
                            if (!iter->dataAvail) {
                                memReqs++;
                                req(MEMORY, iter->addr, iter->pSrc, processorCount, false, -2);
                            }
                            else {
                                if (iter->shared) {
                                    //data is shared
                                    coherComp->busReq(SHARED, iter->addr,
                                                      iter->pSrc, -1, -2);
                                }
                                else {
                                    //data is exclusive
                                    coherComp->busReq(DATA, iter->addr,
                                                      iter->pSrc, -1, -2);
                                }
                            }
                            free(iter);
                        }
                    }
                    else {
                        perror("should not be here with no directory\n");
                        exit(1);
                        //not broadcast, so just remove from active requests
                        if (prev != NULL) {
                            prev->next = iter->next;
                        }
                        else {
                            activeRequests[completedReq->pDest] = iter->next;
                        }
                        free(iter);
                    }
                }
                assert(checkActiveRequests());
            }
        }
        else if (lnk->countDown == 0)
        {
            if (linkRequestQueueSize(lnk) == 0) {
                continue;
            }
            lastProgressTick = tickCount;
            bus_req* nextReq = deqLinkRequest(lnk);
            lnk->pendingReq = nextReq;
            if (lnk->pendingReq->procNum == lnk->pendingReq->pSrc &&
                lnk->pendingReq->ack == false &&
                (lnk->pendingReq->brt == BUSRD || lnk->pendingReq->brt == BUSWR)) {
                //print we are waiting for acks for msg with msgnum:
                if (CADSS_VERBOSE) {
                    printf("Tracking active request from proc %d of type %s for address %lx with msgNum %d\n",
                           lnk->pendingReq->pSrc,
                           req_type_map[lnk->pendingReq->brt],
                           lnk->pendingReq->addr,
                           lnk->pendingReq->msgNum);
                }
                assert(lnk->pendingReq->broadcast == true);
                bus_req* copy = malloc(sizeof(bus_req));
                memcpy(copy, lnk->pendingReq, sizeof(bus_req));
                copy->numAcks = 0;
                if (activeRequests[lnk->pendingReq->pSrc] == NULL) {
                    activeRequests[lnk->pendingReq->pSrc] = copy;
                }
                else {
                    //there is already an active request from this processor, so chain it
                    bus_req* iter = activeRequests[lnk->pendingReq->pSrc];
                    bus_req* prev = NULL;
                    bool shouldAdd = true;
                    while (iter!= NULL) {
                        if (iter->msgNum == copy->msgNum) {
                            //already tracking this request
                            shouldAdd = false;
                            free(copy);
                            break;
                        }
                        prev = iter;
                        iter = iter->next;
                    }
                    if (shouldAdd) {
                        assert(prev != NULL);
                        prev->next = copy;
                    }
                }
                assert(checkActiveRequests());
            }
            if (CADSS_VERBOSE) {
                printf("Link between proc %d and proc %d sending req from proc %d of type %s to proc %d\n",
                       lnk->proc1, lnk->proc2, lnk->pendingReq->pSrc,
                       req_type_map[lnk->pendingReq->brt],
                       (lnk->pendingReq->broadcast) ? -1 : lnk->pendingReq->pDest);
            }
            lnk->countDown = CACHE_DELAY;
            lnk->pendingReq->currentState = WAITING_CACHE;
        } 
    }
    if (memoryCountdown > 0) {
        memoryCountdown--;
    }
    if (memoryCountdown == 0 && memoryRequests != NULL) {
        memReqsMade++;
        bus_req* thisRequest = memoryRequests;
        int memCountDown = memComp->busReq(thisRequest->addr,
                        thisRequest->pSrc, memReqCallback);
        memoryCountdown = memCountDown;
        memoryRequests = memoryRequests->next;
        free(thisRequest);
    }
    // if (tickCount - lastProgressTick > 10000) {
    //     printf("No progress made in 10000 ticks, possible deadlock in interconnect\n");
    //     printInterconnForRingState();
    //     printf("%d requests to memory, %d requests reached memory, %d requests made to memory, and %d responses from memory, %d of which were received\n", memReqs, memReqsReachedMemRing, memReqsMade, memResponses, memRecvs);
    //     raise(SIGTRAP);
    // }
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
        lineTick();
    }
    
    if (t == 2 && processorCount > 1) {
        ringTick();
    }

    if (t == 3 && processorCount > 1) {
        meshTick();
    }

    return 0;
}

void printInterconnForLineState(void)
{
    printf("--- Interconnect Debug State for Line Topology (Processors: %d) ---\n",
           processorCount);
    for (int i = 0; i < processorCount; i++) {
        link* lnk = links[i];
        printf("Link between proc %d and proc %d:\n", lnk->proc1, lnk->proc2);
        if (lnk->pendingReq != NULL) {
            printf("  Pending Request:\n"
                   "    From Proc: %d\n"
                   "    Type: %s\n"
                   "    Address: 0x%016lx\n"
                   "    State: %s\n"
                   "    Broadcast: %s\n"
                   "    Ack: %s\n"
                   "msgNum: %d\n",
                   lnk->pendingReq->procNum,
                   req_type_map[lnk->pendingReq->brt],
                   lnk->pendingReq->addr,
                   req_state_map[lnk->pendingReq->currentState],
                   (lnk->pendingReq->broadcast) ? "true" : "false",
                   (lnk->pendingReq->ack) ? "true" : "false",
                   lnk->pendingReq->msgNum);
        } else {
            printf("  No Pending Request\n");
        }
        printf("  Link Queue Size: %d\n", linkRequestQueueSize(lnk));
    }
    // Print active requests per processor
    for (int p = 0; p < processorCount; p++)
    {
        printf("  Active Requests for Processor[%02d]:\n", p);
        bus_req* iter = activeRequests[p];
        if (iter == NULL) {
            printf("    None\n");
        }
        while (iter)
        {
            printf("    Request:\n"
                   "      Type: %s\n"
                   "      Address: 0x%016lx\n"
                   "      Broadcast: %s\n"
                   "      Ack: %s\n"
                   "      Num Acks Received: %d\n"
                   "      MsgNum: %d\n"
                   "      source Proc: %d\n",
                   req_type_map[iter->brt],
                   iter->addr,
                   (iter->broadcast) ? "true" : "false",
                   (iter->ack) ? "true" : "false",
                   iter->numAcks,
                   iter->msgNum,
                   iter->pSrc);
            iter = iter->next;
        }
    }
}

void printInterconnForRingState(void) {
    printf("--- Interconnect Debug State for Ring Topology (Processors: %d) ---\n",
           processorCount);
    for (int i = 0; i < processorCount + 1; i++) {
        link* lnk = links[i];
        printf("Link between proc %d and proc %d:\n", lnk->proc1, lnk->proc2);
        if (lnk->pendingReq != NULL) {
            printf("  Pending Request:\n"
                   "    From Proc: %d\n"
                   "    Type: %s\n"
                   "    Address: 0x%016lx\n"
                   "    State: %s\n"
                   "    Broadcast: %s\n"
                   "    Ack: %s\n"
                   "msgNum: %d\n",
                   lnk->pendingReq->procNum,
                   req_type_map[lnk->pendingReq->brt],
                   lnk->pendingReq->addr,
                   req_state_map[lnk->pendingReq->currentState],
                   (lnk->pendingReq->broadcast) ? "true" : "false",
                   (lnk->pendingReq->ack) ? "true" : "false",
                   lnk->pendingReq->msgNum);
        } else {
            printf("  No Pending Request\n");
        }
        printf("  Link Queue Size: %d\n", linkRequestQueueSize(lnk));
    }
    // Print active requests per processor
    for (int p = 0; p < processorCount; p++)
    {
        printf("  Active Requests for Processor[%02d]:\n", p);
        bus_req* iter = activeRequests[p];
        if (iter == NULL) {
            printf("    None\n");
        }
        while (iter)
        {
            printf("    Request:\n"
                   "      Type: %s\n"
                   "      Address: 0x%016lx\n"
                   "      Broadcast: %s\n"
                   "      Ack: %s\n"
                   "      Num Acks Received: %d\n"
                   "      MsgNum: %d\n"
                   "      source Proc: %d\n",
                   req_type_map[iter->brt],
                   iter->addr,
                   (iter->broadcast) ? "true" : "false",
                   (iter->ack) ? "true" : "false",
                   iter->numAcks,
                   iter->msgNum,
                   iter->pSrc);
            iter = iter->next;
        }
    }
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
    //check every link's pending request and queue to see if any node that is not memory is transferring data for this addr and procNum
    for (int i = 0; i < processorCount; i++) {
        link* lnk = links[i];
        if (lnk->pendingReq != NULL) {
            if (lnk->pendingReq->addr == addr &&
                lnk->pendingReq->pDest == procNum &&
                lnk->pendingReq->data == 1) {
                return 1;
            }
        }
        bus_req* iter = lnk->linkQueue1;
        while (iter != NULL) {
            if (iter->addr == addr &&
                iter->pDest == procNum &&
                iter->data == 1) {
                return 1;
            }
            iter = iter->next;
        }
        iter = lnk->linkQueue2;
        while (iter != NULL) {
            if (iter->addr == addr &&
                iter->pDest == procNum &&
                iter->data == 1) {
                return 1;
            }
            iter = iter->next;
        }
    }

    if (!pendingRequest)
        return 0;

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
