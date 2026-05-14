#include <stdio.h>
#include <vector>
#include <deque>
#include <algorithm>
#include <string>
#include "pin.H"

KNOB<int> KnobCacheSize (KNOB_MODE_WRITEONCE, "pintool", "c", "8192", "Cache size in bytes");
KNOB<int> KnobAssoc     (KNOB_MODE_WRITEONCE, "pintool", "a", "2",    "Associativity");
KNOB<int> KnobBlockSize (KNOB_MODE_WRITEONCE, "pintool", "b", "64",   "Block size in bytes");
KNOB<int> KnobA1Percent (KNOB_MODE_WRITEONCE, "pintool", "q", "25",   "A1 probation quota as %% of ways (1-99)");
KNOB<std::string> KnobOutputPath(
    KNOB_MODE_WRITEONCE,
    "pintool",
    "o",
    "traces/week4-2q.log",
    "Output log path");

int CACHE_SIZE, ASSOCIATIVITY, BLOCK_SIZE, NUM_SETS;
int A1_MAX;
FILE* output = nullptr;

enum LineState { INVALID_S, A1_S, AM_S };

struct CacheLine {
    ADDRINT   tag;
    LineState state;
};

std::vector<std::vector<CacheLine>> cache;

struct SetQueues {
    std::deque<ADDRINT> a1; // FIFO
    std::deque<ADDRINT> am; // LRU
};
std::vector<SetQueues> queues;

UINT64 hits         = 0;
UINT64 misses       = 0;
UINT64 promotions   = 0;
UINT64 a1_evictions = 0;
UINT64 am_evictions = 0;


// Remove first occurrence of tag from a deque
static void dq_erase(std::deque<ADDRINT> &dq, ADDRINT tag)
{
    auto it = std::find(dq.begin(), dq.end(), tag);
    if (it != dq.end()) dq.erase(it);
}

// Find way index holding tag
static int find_valid_way(int si, ADDRINT tag)
{
    for (int w = 0; w < ASSOCIATIVITY; w++)
        if (cache[si][w].state != INVALID_S && cache[si][w].tag == tag)
            return w;
    return -1;
}

// Find way index by tag
static int find_way_by_tag(int si, ADDRINT tag)
{
    for (int w = 0; w < ASSOCIATIVITY; w++)
        if (cache[si][w].tag == tag) return w;
    return 0;   // unreachable when queues are consistent
}

// Find an invalid way
static int find_free_way(int si)
{
    for (int w = 0; w < ASSOCIATIVITY; w++)
        if (cache[si][w].state == INVALID_S) return w;
    return -1;
}

VOID AccessMemory(VOID *addr)
{
    ADDRINT block = (ADDRINT)addr / (ADDRINT)BLOCK_SIZE;
    int     si    = (int)(block % (ADDRINT)NUM_SETS);
    ADDRINT tag   = block / (ADDRINT)NUM_SETS;

    auto &s = cache[si];
    auto &q = queues[si];

    int way = find_valid_way(si, tag);

    if (way != -1) {
        hits++;
        if (s[way].state == A1_S) {
            // promote to hot queue (MRU).
            dq_erase(q.a1, tag);
            q.am.push_back(tag);
            s[way].state = AM_S;
            promotions++;
        } else {
            // refresh to MRU end.
            dq_erase(q.am, tag);
            q.am.push_back(tag);
        }
        return;
    }

    misses++;

    int victim = find_free_way(si);

    if (victim == -1) {
        // Eviction Policy:
        //   A1 over quota → evict from A1 (keep A1 at target size)
        //   A1 at/under quota, Am non-empty → evict LRU from Am
        //   Am empty (all ways in A1) → must evict from A1 anyway
        ADDRINT vtag;
        if ((int)q.a1.size() >= A1_MAX) {
            vtag = q.a1.front(); q.a1.pop_front();
            a1_evictions++;
        } else if (!q.am.empty()) {
            vtag = q.am.front(); q.am.pop_front();
            am_evictions++;
        } else {
            vtag = q.a1.front(); q.a1.pop_front();
            a1_evictions++;
        }
        victim = find_way_by_tag(si, vtag);
    }

    // All new lines enter A1
    s[victim].tag   = tag;
    s[victim].state = A1_S;
    q.a1.push_back(tag);
}

VOID Instruction(INS ins, VOID *v)
{
    UINT32 memOperands = INS_MemoryOperandCount(ins);
    for (UINT32 memOp = 0; memOp < memOperands; memOp++) {
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

VOID Fini(INT32 code, VOID *v)
{
    UINT64 total    = hits + misses;
    UINT64 evictions = a1_evictions + am_evictions;

    fprintf(output, "2Q (A1 quota=%d%%,  A1_MAX=%d ways)\n",
         KnobA1Percent.Value(), A1_MAX);
    fprintf(output, "Hits:                %lu\n", hits);
    fprintf(output, "Misses:              %lu\n", misses);
    if (total)
     fprintf(output, "HitRate:             %.2f%%\n", (100.0 * hits) / total);
    fprintf(output, "---\n");
    fprintf(output, "Promotions (A1→Am):  %lu  (%.1f%% of misses)\n",
         promotions,
         misses ? (100.0 * promotions) / misses : 0.0);
    fprintf(output, "A1 evictions:        %lu  (%.1f%% of evictions)\n",
         a1_evictions,
         evictions ? (100.0 * a1_evictions) / evictions : 0.0);
    fprintf(output, "Am evictions:        %lu  (%.1f%% of evictions)\n",
         am_evictions,
         evictions ? (100.0 * am_evictions) / evictions : 0.0);
    fflush(output);
    fclose(output);

    printf("2Q (A1 quota=%d%%,  A1_MAX=%d ways)\n",
        KnobA1Percent.Value(), A1_MAX);
    printf("Hits:                %lu\n", hits);
    printf("Misses:              %lu\n", misses);
    if (total)
     printf("HitRate:             %.2f%%\n", (100.0 * hits) / total);
    printf("---\n");
    printf("Promotions (A1→Am):  %lu  (%.1f%% of misses)\n",
        promotions,
        misses ? (100.0 * promotions) / misses : 0.0);
    printf("A1 evictions:        %lu  (%.1f%% of evictions)\n",
        a1_evictions,
        evictions ? (100.0 * a1_evictions) / evictions : 0.0);
    printf("Am evictions:        %lu  (%.1f%% of evictions)\n",
        am_evictions,
        evictions ? (100.0 * am_evictions) / evictions : 0.0);
}

int main(int argc, char *argv[])
{
    if (PIN_Init(argc, argv)) return -1;

    std::string output_path = KnobOutputPath.Value();
    output = fopen(output_path.c_str(), "w");
    if (!output) {
        perror(output_path.c_str());
        return -1;
    }

    CACHE_SIZE    = KnobCacheSize.Value();
    ASSOCIATIVITY = KnobAssoc.Value();
    BLOCK_SIZE    = KnobBlockSize.Value();
    NUM_SETS      = CACHE_SIZE / (BLOCK_SIZE * ASSOCIATIVITY);

    int q = KnobA1Percent.Value();
    if (q < 1 || q > 99) {
        fprintf(stderr, "Error: -q must be between 1 and 99\n");
        return 1;
    }
    // at least 1 A1 slot even on low associativity.
    A1_MAX = std::max(1, (ASSOCIATIVITY * q) / 100);

    printf("[2Q init] sets=%d, assoc=%d, A1_MAX=%d\n",
           NUM_SETS, ASSOCIATIVITY, A1_MAX);

    CacheLine empty = {0, INVALID_S};
    cache.resize(NUM_SETS, std::vector<CacheLine>(ASSOCIATIVITY, empty));
    queues.resize(NUM_SETS);

    INS_AddInstrumentFunction(Instruction, 0);
    PIN_AddFiniFunction(Fini, 0);
    PIN_StartProgram();
}
