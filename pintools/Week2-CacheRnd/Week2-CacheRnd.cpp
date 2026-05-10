#include <stdio.h>
#include <vector>
#include <cstdlib>
#include "pin.H"

KNOB<int> KnobCacheSize(KNOB_MODE_WRITEONCE, "pintool", "c", "8192", "Cache size"); // 2^13 by default (8 kb)
KNOB<int> KnobAssoc(KNOB_MODE_WRITEONCE, "pintool", "a", "2", "Associativity"); // 2-way set associative
KNOB<int> KnobBlockSize(KNOB_MODE_WRITEONCE, "pintool", "b", "64", "Block size"); // 2^6 by default (64 bytes)

int CACHE_SIZE, ASSOCIATIVITY, BLOCK_SIZE, NUM_SETS;

struct CacheLine {
    ADDRINT tag;
    BOOL valid;
};

std::vector<std::vector<CacheLine>> cache;

UINT64 hits = 0, misses = 0;

VOID AccessMemory(VOID* addr)
{
    ADDRINT address = (ADDRINT)addr;

    ADDRINT blockAddr = address / BLOCK_SIZE;
    ADDRINT index = blockAddr % NUM_SETS;
    ADDRINT tag = blockAddr / NUM_SETS;

    auto &set = cache[index];

    for (int i = 0; i < ASSOCIATIVITY; i++)
    {
        if (set[i].valid && set[i].tag == tag)
        {
            hits++;
            return;
        }
    }

    misses++;

    int victim = -1;

    for (int i = 0; i < ASSOCIATIVITY; i++)
    {
        if (!set[i].valid)
        {
            victim = i;
            break;
        }
    }

    if (victim == -1)
    {
        victim = rand() % ASSOCIATIVITY;
    }

    set[victim].valid = TRUE;
    set[victim].tag = tag;
}

VOID Instruction(INS ins, VOID* v)
{
    UINT32 memOperands = INS_MemoryOperandCount(ins);
    for (UINT32 memOp = 0; memOp < memOperands; memOp++)
    {
        if (INS_MemoryOperandIsRead(ins, memOp) ||
            INS_MemoryOperandIsWritten(ins, memOp))
        {
            INS_InsertPredicatedCall(
                ins, IPOINT_BEFORE,
                (AFUNPTR)AccessMemory,
                IARG_MEMORYOP_EA, memOp,
                IARG_END);
        }
    }
}

VOID Fini(INT32 code, VOID* v)
{
    UINT64 total = hits + misses;
    printf("RANDOM\nHits: %lu\nMisses: %lu\n", hits, misses);
    if (total) printf("HitRate: %.2f%%\n", (100.0 * hits) / total);
}

int main(int argc, char* argv[])
{
    if (PIN_Init(argc, argv)) return -1;

    CACHE_SIZE = KnobCacheSize.Value();
    ASSOCIATIVITY = KnobAssoc.Value();
    BLOCK_SIZE = KnobBlockSize.Value();

    NUM_SETS = CACHE_SIZE / (BLOCK_SIZE * ASSOCIATIVITY);

    cache.resize(NUM_SETS, std::vector<CacheLine>(ASSOCIATIVITY));

    INS_AddInstrumentFunction(Instruction, 0);
    PIN_AddFiniFunction(Fini, 0);

    PIN_StartProgram();
}