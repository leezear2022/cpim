# Batch-3 Data Layouts: Route A vs Route B

This document clarifies the critical distinction in data layout between the two proposed Batch-3 architectures for the GPU solver.

## Overview

The core difference lies in how the domains of multiple subproblems (worlds) are stored in memory to optimize for different parallel instructions.

| Feature | Route A (Constraint Aggregation) | Route B (Bitwise Parallel) |
| :--- | :--- | :--- |
| **Primary Goal** | Minimize kernel launch overhead & queue management cost. | Maximize ALU throughput via SWAR (SIMD within a Register). |
| **Parallelism** | Thread-per-World (or Warp-per-World). | Bit-per-World (32 Worlds per Thread). |
| **Memory Layout** | **Row-Major / AoS** (Array of Structures) | **Column-Major / SoA** (Structure of Arrays) |
| **User Notation** | `bitsubdom[subproblem][variable]` | `bitsubdom[variable][subproblem]` |
| **Hardware Mapping** | 1 Thread $\to$ 1 World | 1 ALU Bit $\to$ 1 World |

---

## 1. Route A: Constraint Aggregation (Current Batch-3A)

**Layout:** `[NumWorlds][NumVars][NumWords]`
Configuration: Standard "Array of Structures" where each world is a complete, contiguous CSP state.

```cpp
// Logic Notation: bitsubdom[subproblem][variable]
// Actual Memory:  Flat array based on World ID
u32* d_ws_bitdom; 

// Accessing Variable 'v' for World 'w':
u32* my_dom = &d_ws_bitdom[w * (num_vars * n_words) + v * n_words];
```

*   **Pros:**
    *   **Zero Conversion Cost:** Can copy directly from the standard snapshot format (`memcpy`).
    *   **Independent Indexing:** World 7 can process Constraint A while World 8 processes Constraint B (if using independent queues).
    *   **Code Reuse:** Can reuse existing propagators (just change the pointer).
*   **Cons:**
    *   **Memory Divergence:** If World 7 and World 8 are in the same warp but looking at different constraints/variables, memory accesses are uncoalesced.
    *   **ALU Underutilization:** `result = dom & mask` only processes 1 world's logic per instruction.

---

## 2. Route B: Bitwise Parallel (Bit-Sliced / Batch-3B)

**Layout:** `[NumVars][NumValues][NumBitMasks]`
Configuration: "Structure of Arrays" or "Bit-Sliced" where the statuses of 32 worlds for a specific `(Var, Val)` are packed into a single `u32`.

```cpp
// Logic Notation: bitsubdom[variable][subproblem]
// Actual Memory:  Transposed / Bit-Sliced
u32* d_start_bit_sup_dom; // or d_transposed_dom

// Accessing Variable 'v', Value 'val' across 32 Worlds:
// Note: We don't access a single world 'w'. We access ALL 32 at once.
u32 packed_status = d_transposed_dom[v * max_dom_size + val];

// packed_status bit 'k' corresponds to World 'k'.
```

*   **Pros:**
    *   **SIMD Efficiency:** `result = packed_dom & packed_mask` performs the logical AND for 32 worlds in a single cycle.
    *   **Coalesced Access:** All 32 generic "threads" (bits) are always accessing the exact same variable/value address.
*   **Cons:**
    *   **Transpose Cost:** Requires a dedicated kernel to transpose the snapshot from `[World][Var]` to `[Var][World]` at startup.
    *   **Lockstep Requirement:** All 32 worlds MUST be processing the exact same constraint on the exact same variable at the same time. If World 0 needs to check Constraint A and World 1 needs Constraint B, this model breaks efficiency (requires masking off).

## Summary Table

| Concept | Route A (Batch-3A) | Route B (Batch-3B) |
| :--- | :--- | :--- |
| **Logic** | `if (world[w].has(v, val))` | `mask = worlds.has(v, val)` |
| **Storage** | `Domain[World][Var]` | `Domain[Var][World]` |
| **Instruction** | `Scalar` (1 world) | `SWAR` (32 worlds) |
| **Best For** | Sparse/Divergent workloads | Dense/Uniform workloads |
