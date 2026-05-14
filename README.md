# Cache Simulator with Memory Trace Analysis

## Overview

This project implements a configurable cache simulator with memory trace analysis using Intel PIN instrumentation. It collects memory access patterns, analyzes temporal and spatial locality, and evaluates cache replacement policies against an optimal benchmark.

## Features

- **Memory Trace Collection**: Uses Intel PIN dynamic instrumentation to collect memory access traces
- **Pattern Analysis**: 
	- Reuse distance computation for temporal locality
	- Stride pattern detection for spatial locality
- **Configurable Cache Simulator**:
	- Adjustable cache size, block size, and associativity
	- Multiple replacement policies: LRU, FIFO, Random
	- Belady's optimal replacement algorithm as benchmark
- **Performance Comparison**: Hit/miss rate analysis against optimal policy
- **Custom Policy**: Implements a replacement policy based on observed access patterns
- **Reporting**: Detailed cache performance metrics and analysis

## Build and Run

### Prerequisites
- GCC/G++ compiler
- GNU Make

### Build

From the repository root:

```bash
make Week1                    # Build Week 1 pintool
make Week2-CacheFIFO         # Build Week 2 FIFO cache
make Week2-CacheLRU          # Build Week 2 LRU cache
make Week2-CacheRnd          # Build Week 2 random cache
make pintool                 # Build all pintools
```

Output files (`.so` binaries) are generated in the `pintools/` directory.

### Run

Instrument a program with a PIN tool:

```bash
./pin_kit/pin -t ./pintools/Week1.so -- <program> [args]
```

Generated artifacts are written under `traces/` with consistent names:

- `week1-trace.csv` for the Week 1 memory trace
- `week2-lru.log`, `week2-fifo.log`, and `week2-random.log` for the Week 2 policies
- `week3-belady-optimal.log` (trace replay) and `week3-belady-lookahead.log` (PIN lookahead) for Week 3
- `week4-rrip-sp.log` for the Week 4 prefetching policy

## Roadmap

The project is planned in four stages:

1. **Trace analysis** - collect memory traces with Intel PIN and compute reuse distance and stride patterns.
2. **Cache simulator** - build a configurable simulator with cache size, block size, associativity, and standard policies such as LRU, FIFO, and Random.
3. **Optimal baseline** - implement Belady's optimal replacement policy and compare it against the practical policies.
4. **Custom policy and evaluation** - design a policy based on observed access patterns, then measure and report its performance.

## Project Structure

```
.
├── Makefile                 # Root build configuration
├── makefile.rules           # Build rules for pintools
├── makefile.nopincc         # Alternative build configuration
├── pintools/                # PIN tool sources
│   ├── Week1.cpp
│   ├── Week2-CacheFIFO.cpp
│   ├── Week2-CacheLRU.cpp
│   └── Week2-CacheRnd.cpp
│   ├── Week3-Belady-With-Trace.cpp
│   ├── Week3-Belady-Without-Trace.cpp
│   └── Week4.cpp
├── tests/                   # Test programs and binaries
│   ├── asm/                 # Assembly test sources
│   └── bin/                 # Compiled test binaries
├── traces/                  # Generated trace files and logs
└── pin_kit/                 # Intel PIN toolkit
```

## Notes

- All PIN tools compile to `.so` files in the `pintools/` directory
- Object files are placed in `obj-$(TARGET)/` directory
- Test binaries are located in `tests/bin/`
- Generated traces and logs are saved to `traces/`