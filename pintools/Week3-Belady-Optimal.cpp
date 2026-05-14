/*
 * belady_trace.cpp — True Belady's Optimal Replacement Simulator
 *
 * Reads the CSV trace produced by the Week 1 PIN tool:
 *   format per line:  <hex_addr>,<reuse_dist>,<stride>
 *   (only column 1 is used)
 *
 * Algorithm:
 *   Pass 1 — read every address, build per-block sorted access-position lists.
 *   Pass 2 — replay the trace; on every eviction decision, binary-search each
 *             cached line's position list to find its exact next use.
 *             Evict the line with the furthest (or no) next use. True optimal.
 *
 * Build (no PIN needed):
 *   g++ -O2 -std=c++17 -o belady_trace belady_trace.cpp
 *
 * Usage:
 *   ./belady_trace [-c <cache_bytes>] [-a <ways>] [-b <block_bytes>] [-t <trace>]
 *   Defaults: -c 8192  -a 2  -b 64  -t traces/week1-trace.csv
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <climits>

// ─── Types ────────────────────────────────────────────────────────────────────
typedef uint64_t addr_t;   // block address (byte_addr / BLOCK_SIZE)
typedef uint32_t pos_t;    // position in trace (limits trace to ~4B accesses)
static const pos_t INF_POS = UINT32_MAX;  // "never used again"

// ─── Cache ────────────────────────────────────────────────────────────────────
struct CacheLine {
    addr_t tag;
    bool   valid;
};
static std::vector<std::vector<CacheLine>> cache;

// ─── Globals ──────────────────────────────────────────────────────────────────
static int CACHE_SIZE    = 8192;
static int ASSOCIATIVITY = 2;
static int BLOCK_SIZE    = 64;
static int NUM_SETS;
static const char* TRACE_PATH = "traces/week1-trace.csv";
static FILE* output = nullptr;
static const char* kOutputPath = "traces/week3-belady-optimal.log";

// ─── Pass 1 data ──────────────────────────────────────────────────────────────
// For each block address: the sorted list of trace positions where it's accessed.
// Using pos_t (uint32_t) keeps memory reasonable (~4 bytes per access entry).
static std::unordered_map<addr_t, std::vector<pos_t>> access_positions;
static std::vector<addr_t> trace_blocks;   // the full trace as block addresses

// ─── next_use_after ───────────────────────────────────────────────────────────
// Returns the next position > current_pos where block_addr is accessed,
// or INF_POS if it is never accessed again.
// Binary search in the pre-sorted position list → O(log N).
static pos_t next_use_after(addr_t block_addr, pos_t current_pos)
{
    auto it = access_positions.find(block_addr);
    if (it == access_positions.end()) return INF_POS;

    const auto &positions = it->second;
    // upper_bound gives first element strictly greater than current_pos.
    auto ub = std::upper_bound(positions.begin(), positions.end(), current_pos);
    if (ub == positions.end()) return INF_POS;
    return *ub;
}

// ─── Belady victim selection ──────────────────────────────────────────────────
// Returns the way index whose line has the furthest next use from current_pos.
// Tie among INF_POS lines → first found (all equally optimal per true Belady).
static int belady_victim(int set_idx, pos_t current_pos)
{
    // Prefer empty ways (no eviction cost for compulsory misses).
    for (int w = 0; w < ASSOCIATIVITY; w++)
        if (!cache[set_idx][w].valid) return w;

    int    victim   = 0;
    pos_t  max_next = 0;

    for (int w = 0; w < ASSOCIATIVITY; w++) {
        // Reconstruct the block address stored in this way.
        addr_t block = (addr_t)cache[set_idx][w].tag * (addr_t)NUM_SETS
                       + (addr_t)set_idx;
        pos_t  nu    = next_use_after(block, current_pos);

        if (nu > max_next) {
            max_next = nu;
            victim   = w;
            // Short-circuit: can't do better than "never used again".
            if (nu == INF_POS) break;
        }
    }
    return victim;
}

// ─── Arg parsing ─────────────────────────────────────────────────────────────
static void parse_args(int argc, char* argv[])
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-c") && i + 1 < argc) CACHE_SIZE    = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-a") && i + 1 < argc) ASSOCIATIVITY = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-b") && i + 1 < argc) BLOCK_SIZE    = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) TRACE_PATH    = argv[++i];
        else {
            fprintf(stderr,
                "Usage: %s [-c cache_bytes] [-a ways] [-b block_bytes] [-t trace.csv]\n",
                argv[0]);
            exit(1);
        }
    }
}

// ─── Main ─────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[])
{
    parse_args(argc, argv);

    NUM_SETS = CACHE_SIZE / (BLOCK_SIZE * ASSOCIATIVITY);

    output = fopen(kOutputPath, "w");
    if (!output) {
        perror(kOutputPath);
        return 1;
    }

    // ── Pass 1: read trace, build access_positions map ──────────────────────
    printf("Pass 1: reading trace from %s ...\n", TRACE_PATH);

    FILE* f = fopen(TRACE_PATH, "r");
    if (!f) {
        perror(TRACE_PATH);
        fclose(output);
        return 1;
    }

    char line[128];
    pos_t pos = 0;

    while (fgets(line, sizeof(line), f)) {
        // Parse hex address (first CSV column; ignore rest).
        char* comma = strchr(line, ',');
        if (!comma) continue;
        *comma = '\0';

        addr_t byte_addr = (addr_t)strtoull(line, nullptr, 16);
        addr_t block     = byte_addr / (addr_t)BLOCK_SIZE;

        trace_blocks.push_back(block);
        access_positions[block].push_back(pos);

        pos++;
        if (pos == 0) {   // overflow guard (> 4B accesses)
            fprintf(stderr, "Error: trace exceeds 2^32 accesses. "
                            "Change pos_t to uint64_t and recompile.\n");
            fclose(f);
            return 1;
        }
    }
    fclose(f);

    uint64_t total_accesses = (uint64_t)trace_blocks.size();
    printf("  %llu accesses, %zu unique blocks.\n",
           (unsigned long long)total_accesses,
           access_positions.size());

    // The per-block position lists are built in insertion order which is
    // already sorted (positions are assigned monotonically). No sort needed.

    // ── Initialise cache ────────────────────────────────────────────────────
    CacheLine empty = {0, false};
    cache.resize(NUM_SETS, std::vector<CacheLine>(ASSOCIATIVITY, empty));

    // ── Pass 2: simulate Belady ──────────────────────────────────────────────
    printf("Pass 2: simulating Belady's optimal ...\n");

    uint64_t hits   = 0;
    uint64_t misses = 0;

    for (pos_t i = 0; i < (pos_t)total_accesses; i++) {
        addr_t block   = trace_blocks[i];
        int    set_idx = (int)(block % (addr_t)NUM_SETS);
        addr_t tag     = block / (addr_t)NUM_SETS;

        // Hit check.
        bool hit = false;
        for (int w = 0; w < ASSOCIATIVITY; w++) {
            if (cache[set_idx][w].valid && cache[set_idx][w].tag == tag) {
                hits++;
                hit = true;
                break;
            }
        }

        if (!hit) {
            misses++;
            int victim = belady_victim(set_idx, i);
            cache[set_idx][victim].tag   = tag;
            cache[set_idx][victim].valid = true;
        }
    }

    // ── Results ─────────────────────────────────────────────────────────────
        fprintf(output, "Belady Optimal (true)\n");
        fprintf(output, "Cache:   %d bytes, %d-way, %d-byte blocks, %d sets\n",
             CACHE_SIZE, ASSOCIATIVITY, BLOCK_SIZE, NUM_SETS);
        fprintf(output, "Hits:    %llu\n",   (unsigned long long)hits);
        fprintf(output, "Misses:  %llu\n",   (unsigned long long)misses);
        if (total_accesses)
         fprintf(output, "HitRate: %.2f%%\n",
              (100.0 * (double)hits) / (double)total_accesses);
        fflush(output);
        fclose(output);

        printf("\nBelady Optimal (true)\n");
        printf("Cache:   %d bytes, %d-way, %d-byte blocks, %d sets\n",
            CACHE_SIZE, ASSOCIATIVITY, BLOCK_SIZE, NUM_SETS);
        printf("Hits:    %llu\n",   (unsigned long long)hits);
        printf("Misses:  %llu\n",   (unsigned long long)misses);
        if (total_accesses)
         printf("HitRate: %.2f%%\n",
             (100.0 * (double)hits) / (double)total_accesses);

    return 0;
}