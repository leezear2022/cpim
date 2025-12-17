# A Novel Multi‑Thread Parallel Constraint Propagation Scheme — Detailed Summary

> Prepared for LLM consumption — structured, concise, and implementation‑oriented.

## 1) Problem & Motivation
**Constraint Programming (CP)** solves combinatorial problems by enforcing *consistency* via propagators over variable domains. For table (extensional) constraints, state‑of‑the‑art GAC (Generalized Arc Consistency) filters like **STRbit** and **Compact‑Table (CT)** are highly optimized but remain **serial**. This paper proposes **parallel propagation schemes**—designed to parallelize the *scheduling and execution* of propagators—*without changing the underlying hardware*, aiming to accelerate CP on multi‑core CPUs.

## 2) Core Idea (One‑Liner)
> Use **snapshots** + **atomic bitset domains** + a **work‑stealing thread pool** to run table‑constraint propagators in parallel, with formal guarantees that the parallel fixed point equals the serial GAC fixed point.

## 3) Scope
- **Target constraints**: Extensional constraints (tables), esp. STR‑style propagators (e.g., STRbit) and CT.
- **Not changing**: Search tree semantics (backtracking), solver architecture (beyond scheduling), or table filtering logic.
- **Changing**: Propagation *scheduling* and *data exchange* between propagators and global domains.

---

## 4) Key Contributions
1. **Formalization** of parallel propagation via:
   - **Snapshots** (local, thread‑side domains) and **Global Domains**.
   - **Temporary GAC (TGAC)** definition and **equivalence proof** to GAC at the parallel fixed point.
2. **Two parallel propagation schemes**:
   - **Static Submission (PSTRss/PCTss)**: batch submit tasks each wave, then synchronize.
   - **Dynamic Submission (PSTRds/PCTds)**: continuous execution without per‑wave barriers; propagators resubmit affected neighbors on the fly.
3. **Practical engineering**:
   - **AtomicBitSet** domains for lock‑free updates.
   - Work‑stealing pool to balance load.
   - Revised timestamps and per‑propagator *last snapshot* tracking.
4. **Extensive experiments** showing speedups on many non‑binary table benchmarks; analysis of when parallelism helps or hurts.

---

## 5) Background Recap (CP & STR/CT)
- **CP model**: variables X with domains D, constraints C; search (BS/BAB/etc.) interleaves with **propagation**.
- **GAC**: For constraint `c`, every value `a ∈ D(x)` of each `x ∈ scope(c)` must have a **supporting tuple** in `rel(c)`.
- **STR family / CT**: Maintain a live table of valid tuples and filter domains accordingly. Serial `propagate()` typically does: `initial() → updateTable() → filterDomains()`.

---

## 6) New Formalism for Parallelization

### 6.1 Snapshots & Global Domain
- Each propagator `c` reads **snapshots** `𝛿_c(x)` of its scope variables at start; updates only these local copies during filtering.
- At end, `c` **submits** its snapshot back to the **global domain**: `D(x) ← D(x) ∩ 𝛿_c(x)` (atomic AND).

### 6.2 Temporary GAC (TGAC)
- A tuple is **temporarily valid** iff it matches all **snapshots** (not necessarily the global domain).
- A network is **TGAC** iff every `a ∈ 𝛿_c(x)` has a temporary support in each `c`.
- **Proposition**: When parallel propagation reaches a **fixed point**,
  - If success: all snapshots equal their globals ⇒ TGAC ≡ GAC.
  - If failure: DWO/TWO is detected ⇒ both serial and parallel are inconsistent at that node.

### 6.3 Timestamping & Last Snapshots
- Serial *lastSize* heuristics don’t work in parallel. Instead maintain per‑propagator **last snapshot** `𝛿̄(x)` to detect changes precisely across waves.
- **Stamps** mark variables whose global domains just changed, driving which propagators to schedule next.

---

## 7) Thread‑Safe Data Structures

### 7.1 AtomicBitSet (lock‑free)
Essential operations (atomic variants starred): `And&Get*`, `Set*`, `Try&Set*`, `Get&Inc*`.  
Benefits: efficient bit‑parallel set algebra and cheap snapshot copy/AND; ideal for domain masks.

### 7.2 SafeVar / AtomicBitVar API
- `getSnapshot*()`: fetch current global domain as a BitSet view/snapshot.
- `submit*(snapshot)`: atomic `global &= snapshot`, returns the post‑AND mask for change detection.

---

## 8) Two Parallel Propagation Schemes

### 8.1 Static Submission (**PSTRss** / **PCTss**)
**Loop**: (1) collect propagators whose scope touched in last wave (by `stamp==time`), (2) batch submit to pool, (3) **barrier** until all finish, (4) repeat until quiescence or inconsistency.

**Pros**: simple, deterministic waves.  
**Cons**: frequent **barriers** reduce throughput.

**Per‑propagator workflow**:  
`initial()` → take snapshots; update `Sval` (changed vars) & `Ssup` (vars needing supports) via `𝛿̄`.  
`updateTable()` → filter tuples using snapshots.  
`filterDomains()` → filter snapshots using tuples; **submit** snapshots; set `stamp[x]` if global changed.

### 8.2 Dynamic Submission (**PSTRds** / **PCTds**)
- Submit **each affected propagator once at start**. No per‑wave barrier.
- During `filterDomains()`, record modified vars into `Y_evt` and **resubmit neighbors** on the fly (`for c in srb(x)`).
- Guard with **numReq (atomic counter)** to avoid multiple concurrent executions of the same `c`:
  - On demand: `if c.numReq.Get&Inc*()==0` then submit.
  - Finish loop: `repeat … until numReq.Try&Set*(1,0)` to compress bursts of requests.

**Pros**: removes the main bottleneck; higher utilization of workers.  
**Cons**: tends to execute **more propagations** overall (see “hyperactivity”).

---

## 9) “Parallel Propagation Hyperactivity”
With more workers, propagators start sooner and see **fewer accumulated changes**; they may run **more often** (e.g., re‑run after additional value deletions). This increases `#prop` (propagation calls), which can offset gains if tasks are tiny.

---

## 10) Complexity (Intuition)
- Serial worst‑case time: `O(e·r·d·T)` where `T` is STR/CT filtering, `e` constraints, max arity `r`, max domain `d`.
- Parallel worst‑case: `O(e·r·d·T·S/p)` with `p` threads and scheduling overhead `S`.
- In practice: time ≈ `(n · T · S) / p` where `n` is **actual** number of `propagate()` calls (often **higher** for parallel due to hyperactivity).

---

## 11) Implementation Notes
- Language: **Scala 2.12 / Java 11**; pool: **ForkJoinPool** (work‑stealing).
- Parallelism explored: `2, 3, 5, 7, 9, 13, 16` workers.
- Heuristic for experiments: **dom/ddeg** (to avoid failure‑driven search‑tree shape differences).
- Parallelized both **CT → PCTss/PCTds** and **STRbit → PSTRbitss/PSTRbitds**.

---

## 12) Experiments — What to Expect

### 12.1 Non‑Binary Table Constraints
- Benchmarks from **XCSP** repositories, e.g., MODEL‑RB, RENAULT, CROSSWORD, TSP, LARGE‑TABLES, DIMACS, MDD, etc.
- **Headline**: *Parallel schemes win on ~2/3 of groups; dynamic submission wins most often.*
- **PCTds** frequently achieves **>3× speedup** on sizable tables and enough constraints; parallelism around **7** threads often sweet spot. Very large models (MODEL‑RB / LARGE‑TABLES) benefit from **higher** parallelism (up to 16).

**When serial wins**: tiny tables, tiny domains (e.g., SAT‑style {0,1}), or very few constraints—task granularity too small vs. scheduling overhead.

### 12.2 Binary Constraints
- Compared **CT**, **PCTds@5**, and **pfall** (CT + lMaxRPCbit parallelized).
- **pfall** often best due to **stronger consistency** (MaxRPC) guiding main search and interrupt mechanics preventing runaway inference.
- **CT vs PCT**: similar node counts; PCT slightly better but not dramatic on binaries since tables/domains are small.

---

## 13) Practical Guidance (When to Use Which)
- **Prefer Dynamic (PSTRds/PCTds)** for **large tables** and **many constraints**; set pool size to **~#physical cores** (7–16 in paper’s tests).
- **Prefer Serial** or **Static** when:
  - constraints are few,
  - tables are tiny (tuples < 10–20) or domains are Boolean,
  - scheduler overhead dominates.
- For **mixed models**: gate parallelization by **table size / scope size** thresholds; keep small constraints serial.

---

## 14) Integration Pattern (Pseudocode)

```pseudo
// Global solver state
pool = WorkStealingPool(p)
stamp[x] = 0 for all x
time = 1

procedure propagate_parallel_dynamic(X_evt):
  consistent = true
  // seed impacted propagators once
  for x in X_evt:
    for c in srb(x):
      if c.numReq.getAndInc() == 0:
         pool.submit(() -> PSTRds_propagate(c))

  pool.awaitQuiescence()
  return consistent

procedure PSTRds_propagate(c):
  repeat
    c.numReq.set(1)                // compress pending requests
    initial(c)                     // build snapshots 𝛿 from globals
    updateTable(c, 𝛿)              // tuples ← tuples ∩ supports(𝛿)
    filterDomains(c, 𝛿):           // 𝛿[x] ← 𝛿[x] ∩ proj_x(tuples)
      tmp = submit*(x, 𝛿[x])       // global &= 𝛿[x]   (atomic)
      if tmp != 𝛿[x]:              // global changed
         stamp[x] = time
         record x in Y_evt
    submitOthers(c, Y_evt):        // reschedule neighbors
      for x in Y_evt:
        for c2 in srb(x), c2 ≠ c:
          if c2.numReq.getAndInc() == 0:
             pool.submit(() -> PSTRds_propagate(c2))
  until c.numReq.trySet(1, 0)      // done if exactly one request
```

---

## 15) Limitations & Gotchas
- **Hyperactivity**: more calls to `propagate()`—tune pool size; coarsen tasks by grouping micro‑constraints or thresholding.
- **Heuristic interactions**: failure‑counting heuristics (e.g., wdeg/ABS) can be perturbed by different failure locations/timings across threads.
- **Determinism**: dynamic schedules can be non‑deterministic; use static waves for reproducibility when needed.
- **Fairness**: per‑propagator `numReq` prevents duplicate execution but doesn’t totally equalize work—still rely on work‑stealing.

---

## 16) Porting Checklist
- [ ] Replace domain representation with **AtomicBitSet** or equivalent lock‑free mask ops.
- [ ] Implement **snapshot get/submit** on variables.
- [ ] Add per‑propagator **last snapshot** cache for change detection.
- [ ] Implement **Static** and **Dynamic** driver loops (start with Static; graduate to Dynamic).
- [ ] Use a **work‑stealing** pool; expose `awaitQuiescence()`.
- [ ] Tune **parallelism**, **table‑size thresholds**, and **batch sizing**; add metrics (#prop, cpup, cpus).

---

## 17) TL;DR for Engineers
- **Do**: parallelize *propagation*, not just *search*; keep serial‐equivalence via **snapshots + TGAC**.
- **Use**: **dynamic submission** on big extensional constraints; bitset domains with **atomic AND**.
- **Expect**: 2–3× speedups on rich table benchmarks; little/no gain on tiny binary‑like constraints.

---

## 18) Key Terms (Glossary)
- **GAC**: Every remaining value has a supporting tuple in each incident constraint.
- **TGAC**: Same notion but with respect to **snapshots** (temporary views) during parallel propagation; equals GAC at fixed point.
- **DWO/TWO**: Domain Wipe Out / Tuple Wipe Out — immediate failure.
- **PSTRss / PSTRds**: Parallel STR‑style propagator with **static** / **dynamic** submission.
- **PCTss / PCTds**: Parallel Compact‑Table with **static** / **dynamic** submission.
- **Work‑stealing**: idle worker steals tasks from busy ones; good load balance.

---

## 19) Suggested Citations in Your Code/Docs
- Original STR and CT references (Lecoutre et al., Demeulenaere et al.).
- This parallel scheme paper (define TGAC, snapshots, PSTRss/ds, PCTss/ds).
- Optional: pfall + lMaxRPCbit if comparing on binaries.

