# Batch-3 Re-Evaluation: Route B vs Route A

This document re-evaluates the Batch-3 architecture, specifically focusing on the limitations of the "Route B" (Bitwise Parallel) approach and proposing improvements based on the current "Route A" (Constraint Aggregation) implementation.

## 1. Original Batch-3 Design Intent

**Goal:** Maximize GPU throughput by exploiting **structural similarities** across multiple subproblems (worlds).

*   **Hypothesis:** If 32 worlds are all solving the same CSP (just with different initial domains), they likely need to propagate similar constraints at similar times.
*   **Mechanism:**
    *   **Constraint Aggregation (Route A):** Group worlds that need to check constraint $C$ into a single warp.
    *   **Bitwise Parallelism (Route B):** Execute the check for constraint $C$ across 32 worlds simultaneously using bitwise logic (`&`, `|`).

## 2. The Problem with Route B (Bitwise Parallel)

**Core Issue:** **Divergence & Sparsity penalty outweighs the bitwise speedup.**

As correctly identified, the `[Var][World]` (SoA) layout forces a **Lockstep Execution Model**. All 32 worlds packed into a single word must execute the exact same instruction stream.

### Why it Fails in Practice:
1.  **Rare Coincidence:** It is statistically unlikely that 32 independent worlds (especially in the long tail) will simultaneously need to propagate **the same constraint on the same variable**.
2.  **Masking Overhead:** To handle divergence, we must employ heavy masking. If only World 0 needs to check `C1`, the kernel still executes `C1` for all 32 worlds, but masks out results for 1-31.
    *   **Effective Throughput:** $1/32 \approx 3\%$.
    *   **Wasted Bandwidth:** Loading packed data for 31 idle worlds.

**Conclusion:** Route B works only for "dense, uniform" phases (e.g., initial propagation) but catastrophic for the "sparse, irregular" phases that dominate solving time.

## 3. Current Implementation Status (Batch-3A / Route A)

**Current Code (`Batch3AManager`):**
*   **Data Layout:** `[World][Var]` (AoS). Independent domains for each world.
*   **Parallelism:** `Warp-per-World` (or `Block-per-Group-of-Worlds`).
*   **Mechanism:**
    *   Maintains a **Global Worklist** of constraints.
    *   Uses `atomicOr` to aggregate which worlds need to check which constraint.
    *   **Dynamic Dispatch:** A warp grabs a constraint $C$. It then iterates over the worlds that need $C$ (using `__ffs` on the mask).

**Pros:**
*   **Flexibility:** Worlds are not locked. World 0 can check $C_A$ while World 1 checks $C_B$ (if using different warps/streams).
*   **No Transpose Cost:** Uses standard snapshot format.

**Cons:**
*   **Memory Divergence:** Threads in a warp access disjoint memory regions (`World[0].Var[X]` vs `World[1].Var[X]`).
*   **Queue Overhead:** Managing the global worklist and masks adds significant latency.

## 4. Proposed Improvements

### Idea 1: Hybrid Dynamic Dispatch (Smart Batch-3)
Instead of forcing 32 worlds to lockstep (Route B) or fully decoupling them (Batch-2), use **Opportunistic Aggregation**.

*   **Queue Structure:** `[ConstraintID] -> BitMask<32>`
*   **Kernel Logic:**
    1.  Block pulls a task: `(CID, Mask)`.
    2.  **If `popcount(Mask)` is high (e.g., > 16):** Use a **Route B-style** kernel (if data layout allows, or load-and-transpose in shared mem).
    3.  **If `popcount(Mask)` is low (e.g., < 16):** Use a **Route A-style** kernel. Assign each active bit to a single thread/lane. The warp collaborates to process the *active* worlds only.

### Idea 2: Warp-Specialized Kernels (The "Available" List)
Instead of a single mega-kernel:
*   Have specialized warps for specific *types* of constraints (e.g., `AllDiff` warps, `Table` warps).
*   Worlds "subscribe" to these warps.
*   When a warp sees enough subscribers (high density), it runs.

### Idea 3: Data Layout Optimization (Blocked-AoS)
Compromise between AoS and SoA.
*   **Layout:** `[Var][BlockOf32Worlds]`
*   Stored as: `Var0_World0...31`, `Var1_World0...31`...
*   This allows **Coalesced Access** even for a single thread processing World $K$ (it reads `Var[K]`, and the next thread reads `Var[K+1]`).

## Next Steps
1.  **Stick to Route A Logic** (Constraint Aggregation) as the baseline.
2.  **Optimize Memory Access** in Route A to minimize divergence penalty (e.g., using shared memory buffering).
3.  **Forget Route B for General Propagators**. It is statistically inviable for general CP. Retain it *only* for specific, dense global constraints (like `Bit-AllDifferent`) if needed.
