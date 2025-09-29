#include <branch.h>
#include <trace.h>

#include <getopt.h>
#include <stdlib.h>
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

branch* self = NULL;
uint64_t predictorSize = -1;
int8_t s = -1;
int8_t ss = -1; //adjusted value to shift if using GSelect
uint64_t BHRSize = 0;
int8_t b = 0;
int8_t g = 0;

typedef unsigned char counter;
typedef uint64_t BTBEntry;

counter* predictor;
BTBEntry* BTB;
uint64_t BHR = 0;

counter incrementCounter(counter c) {
    if (c == 3) {
        return c;
    }
    return c + 1;
}

counter decrementCounter(counter c) {
    if (c == 0) {
        return c;
    }
    return c - 1;
}

void addToBHR(bool taken) {
    uint8_t t = (uint8_t)taken;
    BHR = ((BHR << 1) | t) & ((1L << b) - 1);
}

void createPredictor(uint8_t type) {
    predictor = malloc(predictorSize * sizeof(counter));
    for (uint64_t i = 0; i < predictorSize; i++) {
        predictor[i] = 1;
    }
}

void createBTB() {
    BTB = calloc(predictorSize, sizeof(BTBEntry));
}

uint64_t getIndex(uint64_t addr) {
    if (g == 0) {
        return (addr >> 3) & ((1L << s) - 1);
    }
    else if (g == 2) {
        return (((addr >> 3) & ((1L << (s-b)) - 1)) << b) | BHR;
    }
}

counter getCounter(uint64_t addr) {
    uint64_t index = getIndex(addr);
    return predictor[index];
}

void setCounter(uint64_t addr, counter c) {
    uint64_t index = getIndex(addr);
    predictor[index] = c;
}

uint64_t getBTB(uint64_t addr) {
    uint64_t index = (addr >> 3) & ((1L << s) - 1);
    return BTB[index];
}

void setBTB(uint64_t addr, uint64_t NextAddr) {
    uint64_t index = (addr >> 3) & ((1L << s) - 1);
    BTB[index] = NextAddr;
}

bool predict(counter c) {
    if (c == 0 || c == 1) {
        return false;
    }
    else {
        return true;
    }
}

uint64_t branchRequest(trace_op* op, int processorNum);

branch* init(branch_sim_args* csa)
{
    int op;

    // TODO - get argument list from assignment
    while ((op = getopt(csa->arg_count, csa->arg_list, "p:s:b:g:")) != -1)
    {
        switch (op)
        {
            // Processor count
            case 'p':
                break;

                // predictor size
            case 's':
                s = atoi(optarg);
                predictorSize = 1L << s;
                break;

                // BHR size
            case 'b':
                b = atoi(optarg);
                if (b > 0) {
                    BHRSize = 1L << b;
                }
                break;
                // predictor model
            case 'g':
                g = atoi(optarg);
                break;
        }
    }

    self = malloc(sizeof(branch));
    self->branchRequest = branchRequest;
    self->si.tick = tick;
    self->si.finish = finish;
    self->si.destroy = destroy;
    createPredictor(g);
    createBTB();

    return self;
}

// Given a branch operation, return the predicted PC address
uint64_t branchRequest(trace_op* op, int processorNum)
{
    assert(op != NULL);

    uint64_t pcAddress = op->pcAddress;
    uint64_t nextAddress = op->nextPCAddress; 
    uint64_t predAddress = 0;

    //predict
    counter c = getCounter(pcAddress);
    bool taken = predict(c);
    if (taken) {
        predAddress = getBTB(pcAddress);
    }
    else {
        predAddress = pcAddress + 4;
    }

    //update predictor
    if (nextAddress != pcAddress + 4) {
        //branch taken
        setCounter(pcAddress, incrementCounter(c));
        setBTB(pcAddress, nextAddress);
        addToBHR(1);
    }
    else {
        //branch not taken
        setCounter(pcAddress, decrementCounter(c));
        addToBHR(0);
    }


    // In student's simulator, either return a predicted address from BTB
    //   or pcAddress + 4 as a simplified "not taken".
    // Predictor has the actual nextPCAddress, so it knows how to update
    //   its state after computing the prediction.

    return predAddress;
}

int tick()
{
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
