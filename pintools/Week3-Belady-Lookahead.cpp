#include <stdio.h>
#include <vector>
#include <deque>
#include "pin.H"

// ─── Knobs ────────────────────────────────────────────────────────────────────
KNOB<int> KnobCacheSize  (KNOB_MODE_WRITEONCE, "pintool", "c", "8192", "Cache size in bytes");
KNOB<int> KnobAssoc      (KNOB_MODE_WRITEONCE, "pintool", "a", "2",    "Associativity");
KNOB<int> KnobBlockSize  (KNOB_MODE_WRITEONCE, "pintool", "b", "64",   "Block size in bytes");
KNOB<int> KnobWindowSize (KNOB_MODE_WRITEONCE, "pintool", "w", "1000", "Lookahead window size W");

int CACHE_SIZE, ASSOCIATIVITY, BLOCK_SIZE, NUM_SETS, WINDOW_SIZE;
static FILE* output = nullptr;
static const char* kOutputPath = "traces/week3-belady-lookahead.log";

// ─── Cache ────────────────────────────────────────────────────────────────────
struct CacheLine {
    ADDRINT tag;
    BOOL    valid;
};
std::vector<std::vector<CacheLine>> cache;

// ─── Lookahead buffer ─────────────────────────────────────────────────────────
// Layout while processing:
//   lookahead is already pop_front'd, so lookahead[0..size-1] = the future window.
// During collection (before processing):
//   lookahead.front() = current access, rest = future.
std::deque<ADDRINT> lookahead;   // holds block addresses (not raw byte addresses)

// ─── Stats ────────────────────────────────────────────────────────────────────
UINT64 hits   = 0;
UINT64 misses = 0;

// ─── Helpers ──────────────────────────────────────────────────────────────────

// Reconstruct the block address stored in way w of set set_idx.
static inline ADDRINT block_of(int set_idx, int way)
{
    return cache[set_idx][way].tag * (ADDRINT)NUM_SETS + (ADDRINT)set_idx;
}

// Distance to the next access of block_addr in the current lookahead window.
// lookahead[0..size-1] is the future at the time this is called (current already popped).
// Returns WINDOW_SIZE+1 as "infinity" (not found = will never be used in window).
static int next_use_distance(ADDRINT block_addr)
{
    int sz = (int)lookahead.size();
    for (int i = 0; i < sz; i++)
        if (lookahead[i] == block_addr) return i + 1;   // +1: distance 1 = very next access
    return WINDOW_SIZE + 1;   // ∞ — not seen in window
}

// Belady victim selection for a set.
// Returns the way index whose cached line has the furthest next use.
// Tie among "never reused in window" lines → first found (all equally optimal).
static int belady_victim(int set_idx)
{
    // Prefer empty ways first (compulsory miss — no eviction cost).
    for (int w = 0; w < ASSOCIATIVITY; w++)
        if (!cache[set_idx][w].valid) return w;

    int victim   = 0;
    int max_dist = -1;

    for (int w = 0; w < ASSOCIATIVITY; w++) {
        int dist = next_use_distance(block_of(set_idx, w));
        if (dist > max_dist) {
            max_dist = dist;
            victim   = w;
            // Short-circuit: true infinity — no need to check others.
            // (Any line with no reuse in window is equally optimal to evict.)
            if (dist == WINDOW_SIZE + 1) break;
        }
    }
    return victim;
}

// ─── Core simulation step ─────────────────────────────────────────────────────
// Called with block_addr = the access to process.
// At call time, lookahead already has this entry popped, so it holds the future.
static void process_access(ADDRINT block_addr)
{
    int     set_idx = (int)(block_addr % (ADDRINT)NUM_SETS);
    ADDRINT tag     = block_addr / (ADDRINT)NUM_SETS;

    auto &s = cache[set_idx];

    // ── Hit check ────────────────────────────────────────────────────────────
    for (int w = 0; w < ASSOCIATIVITY; w++) {
        if (s[w].valid && s[w].tag == tag) {
            hits++;
            return;
        }
    }

    // ── Miss: Belady eviction ─────────────────────────────────────────────────
    misses++;
    int victim = belady_victim(set_idx);
    s[victim].tag   = tag;
    s[victim].valid = TRUE;
}

// ─── PIN callback: buffer every access ───────────────────────────────────────
VOID AccessMemory(VOID* addr)
{
    ADDRINT block_addr = (ADDRINT)addr / (ADDRINT)BLOCK_SIZE;
    lookahead.push_back(block_addr);

    // Once we have W+1 entries the front has a full W-entry future window.
    if ((int)lookahead.size() > WINDOW_SIZE) {
        ADDRINT current = lookahead.front();
        lookahead.pop_front();
        // lookahead now holds exactly the W future accesses after `current`.
        process_access(current);
    }
}

// ─── PIN instrumentation ──────────────────────────────────────────────────────
VOID Instruction(INS ins, VOID* v)
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

// ─── Drain tail + print results ───────────────────────────────────────────────
VOID Fini(INT32 code, VOID* v)
{
    // Drain the remaining buffered accesses.
    // Each iteration the future window shrinks by 1 — still valid Belady
    // reasoning (we simply know less about the future near the end of the trace).
    while (!lookahead.empty()) {
        ADDRINT current = lookahead.front();
        lookahead.pop_front();
        process_access(current);
    }

    UINT64 total = hits + misses;
    fprintf(output, "Belady (lookahead W=%d)\n", WINDOW_SIZE);
    fprintf(output, "Hits:    %lu\n", hits);
    fprintf(output, "Misses:  %lu\n", misses);
    if (total)
        fprintf(output, "HitRate: %.2f%%\n", (100.0 * hits) / total);
    fflush(output);
    fclose(output);

    printf("Belady (lookahead W=%d)\n", WINDOW_SIZE);
    printf("Hits:    %lu\n", hits);
    printf("Misses:  %lu\n", misses);
    if (total)
        printf("HitRate: %.2f%%\n", (100.0 * hits) / total);
}

// ─── Entry point ─────────────────────────────────────────────────────────────
int main(int argc, char* argv[])
{
    if (PIN_Init(argc, argv)) return -1;

    output = fopen(kOutputPath, "w");
    if (!output) {
        perror(kOutputPath);
        return -1;
    }

    CACHE_SIZE    = KnobCacheSize.Value();
    ASSOCIATIVITY = KnobAssoc.Value();
    BLOCK_SIZE    = KnobBlockSize.Value();
    NUM_SETS      = CACHE_SIZE / (BLOCK_SIZE * ASSOCIATIVITY);
    WINDOW_SIZE   = KnobWindowSize.Value();

    CacheLine empty = {0, FALSE};
    cache.resize(NUM_SETS, std::vector<CacheLine>(ASSOCIATIVITY, empty));

    INS_AddInstrumentFunction(Instruction, 0);
    PIN_AddFiniFunction(Fini, 0);
    PIN_StartProgram();
}