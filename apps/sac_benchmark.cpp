// ============================================================================
// SAC-GPU Unified Benchmark
// ============================================================================
// This is the unified entry point for SAC-GPU performance benchmarking.
// Supports:
//   - Throughput test: batch1, stage2, batch3a, fqpt (measure probes/sec)
//   - Full SAC: full_sac (complete SAC convergence until fixed point)
//   - SAC preprocess: sac1_preprocess / sac3_preprocess (queue-based SAC/NSACQ)
//
// Usage:
//   ./sac_benchmark --input=<file.xml> [options]
//
// Options:
//   --mode=batch1|stage2|batch3a|fqpt|full_sac|sac1_preprocess|sac3_preprocess|compare
//                                 Select mode (default: stage2)
//   --num_probes=N                Number of probe tasks for throughput (default: 32)
//   --warmup=N                    Warmup iterations (default: 1)
//   --iterations=N                Benchmark iterations (default: 3)
//   --max_sac_rounds=N            Max SAC rounds for full_sac (default: 100)
//   --verbose                     Show detailed output
//   --compare                     Compare all throughput modes
//
// Examples:
//   # Full SAC convergence test
//   ./sac_benchmark --input=queens-4.xml --mode=full_sac --verbose
//
//   # Throughput benchmark comparing all modes
//   ./sac_benchmark --input=queens-4.xml --compare --num_probes=100
// ============================================================================

#include <gflags/gflags.h>
#include <glog/logging.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <set>
#include <vector>

#include "GModelSolver.h"
#include "GModel.cuh"
#include "model/gmodel_adapter.h"
#include "model/model_normalizer.h"
#include "model/xcsp_parser.h"
#include "solver/gpu/batch_probe_manager.h"
#include "solver/gpu/sac_gpu_config.h"

#ifndef CPIM_PROJECT_ROOT
#define CPIM_PROJECT_ROOT "."
#endif

DEFINE_string(input, "", "Input XCSP3 file path");
DEFINE_string(
    mode,
    "stage2",
    "Benchmark mode: batch1, stage2, batch3a, fqpt, compare, full_sac, sac1_preprocess, sac3_preprocess");
DEFINE_int32(num_probes, 32, "Number of probe tasks (for throughput test)");
DEFINE_int32(warmup, 1, "Warmup iterations");
DEFINE_int32(iterations, 3, "Benchmark iterations");
DEFINE_bool(verbose, false, "Show detailed output");
DEFINE_bool(compare, false, "Compare all modes");
DEFINE_int32(max_sac_rounds, 100, "Max SAC rounds for full_sac mode");
DEFINE_int32(assign_count, 0, "Number of variables to assign before SAC (simulates MSAC)");
DEFINE_string(msac_mode, "fast", "MSAC mode: 'fast' (GAC backtrack), 'full' (SAC backtrack, slow), 'parallel' (GPU parallel SAC probe)");
DEFINE_bool(nsac_mask, true, "Enable NSAC allowed-constraints mask (restrict propagation to neighborhood)");
DEFINE_bool(sac_queue_budget, false, "Enable SAC queue-level budget (P1-2)");
DEFINE_int32(sac_max_total_probes, 0, "Max total probes for SAC3 (0=unlimited)");
DEFINE_int32(sac_max_queue_size, 0, "Max probe queue size for SAC3 (0=unlimited)");
DEFINE_int32(sac_max_total_requeues, 0, "Max total requeues for SAC3 (0=unlimited)");
DEFINE_int32(sac_max_requeues_per_var, 0, "Max requeues per var for SAC3 (0=unlimited)");
DEFINE_int32(batch3a_check_mapping, 0,
             "Batch-3A check mapping: 0=warp-per-world, 1=subwarp-per-world (P2-1), 2=warp-per-word+lane-per-world (P2-2)");
DEFINE_int32(batch3a_subwarp_size, 8,
             "Batch-3A subwarp size: 4/8/16 (only for mapping=1)");
DEFINE_int32(batch3a_worlds_per_block, 0,
             "Batch-3A worlds per block (G): 0=auto, 1..32=override (P2-2 tuning)");
DEFINE_int32(batch3a_shmem_padding, 0,
             "Batch-3A shared packing stride padding (P2-2b): 0=off (default), 1=on");
DEFINE_int32(fqpt_num_blocks, -1,
             "FQ-PT: persistent blocks count (-1=auto)");
DEFINE_int32(fqpt_queue_capacity, 0,
             "FQ-PT: global ring capacity (power of two, 0=auto)");
DEFINE_int32(fqpt_pop_batch, 4,
             "FQ-PT: CTA batch pop size K");
DEFINE_int32(fqpt_local_buffer, 64,
             "FQ-PT: CTA local buffer capacity L");
DEFINE_int32(fqpt_lock_retry, 8,
             "FQ-PT: world lock retry count");
DEFINE_int32(fqpt_lock_backoff, 32,
             "FQ-PT: world lock backoff iterations");
DEFINE_bool(fqpt_enable_cid_grouping, false,
            "FQ-PT: enable CTA-local cid grouping");
DEFINE_bool(fqpt_enable_parallel_group_check, false,
            "FQ-PT: enable parallel group check (warp-per-world)");
DEFINE_int32(fqpt_group_warps, 4,
             "FQ-PT: group warps per CTA for grouped check");
DEFINE_int32(fqpt_group_degrade_threshold, 1,
             "FQ-PT: degrade to single-task when max bucket <= threshold");
DEFINE_bool(fqpt_enable_world_owner, false,
            "FQ-PT: enable Owner-World + two-level frontier path");
DEFINE_bool(fqpt_enable_world_stealing, false,
            "FQ-PT OW2: enable world_cursor dynamic world assignment control path");
DEFINE_bool(fqpt_enable_ow1_frontier_scatter, false,
            "FQ-PT OW1: enable warp-cooperative frontier neighbor scatter");
DEFINE_int32(fqpt_ow1_min_degree, 32,
             "FQ-PT OW1: min var degree to enable warp scatter");
DEFINE_int32(fqpt_ow1_scatter_mode, 1,
             "FQ-PT OW1: scatter mode (0=fallback, 1=legacy warp, 2=match_any)");
DEFINE_bool(fqpt_ow1_force_scatter, false,
            "FQ-PT OW1: force scatter regardless of min_degree");

namespace cpim {

using cpim::model::ModelNormalizer;
using cpim::model::ParserType;
using cpim::model::XcspParser;

void ConfigureBatch3AManager(Batch3AManager& manager) {
    const int mapping = FLAGS_batch3a_check_mapping;
    if (mapping == 1) {
        manager.SetCheckMapping(kSubwarpPerWorld);
    } else if (mapping == 2) {
        manager.SetCheckMapping(kWarpPerWordLaneWorld);
    } else {
        if (mapping != 0) {
            LOG(WARNING) << "Invalid --batch3a_check_mapping=" << mapping
                         << ", fallback to 0 (warp-per-world)";
        }
        manager.SetCheckMapping(kWarpPerWorld);
    }
    manager.SetSubwarpSize(FLAGS_batch3a_subwarp_size);
    manager.SetWorldsPerBlock(FLAGS_batch3a_worlds_per_block);
    manager.SetShmemPadding(FLAGS_batch3a_shmem_padding != 0);
}

void ConfigureFQPTManager(FQPTBaselineManager& manager) {
    manager.EnableStats(true);
    if (FLAGS_fqpt_queue_capacity > 0) {
        manager.SetQueueCapacity(FLAGS_fqpt_queue_capacity);
    }
    manager.SetCtaPopBatch(FLAGS_fqpt_pop_batch);
    manager.SetLocalBufferCapacity(FLAGS_fqpt_local_buffer);
    manager.SetLockRetryLimit(FLAGS_fqpt_lock_retry);
    manager.SetLockBackoff(FLAGS_fqpt_lock_backoff);
    manager.SetEnableCidGrouping(FLAGS_fqpt_enable_cid_grouping);
    manager.SetEnableParallelGroupCheck(FLAGS_fqpt_enable_parallel_group_check);
    manager.SetGroupWarpsPerCta(FLAGS_fqpt_group_warps);
    manager.SetGroupDegradeThreshold(FLAGS_fqpt_group_degrade_threshold);
    manager.SetEnableWorldOwner(FLAGS_fqpt_enable_world_owner);
    manager.SetEnableWorldStealing(FLAGS_fqpt_enable_world_stealing);
    manager.SetEnableOW1FrontierScatter(FLAGS_fqpt_enable_ow1_frontier_scatter);
    manager.SetOW1MinDegree(FLAGS_fqpt_ow1_min_degree);
    manager.SetOW1ScatterMode(FLAGS_fqpt_ow1_scatter_mode);
    manager.SetOW1ForceScatter(FLAGS_fqpt_ow1_force_scatter);
}

// ============================================================================
// Probe task generation
// ============================================================================

std::vector<ProbeTask> CollectProbeTasks(GModel* gmodel, int max_tasks) {
    std::vector<ProbeTask> tasks;
    const int num_vars = gmodel->GetNumVars();
    for (int var = 0; var < num_vars && static_cast<int>(tasks.size()) < max_tasks; ++var) {
        if (gmodel->IsAssigned(var)) continue;
        for (int val = gmodel->GetFirstValue(var);
             val != -1 && static_cast<int>(tasks.size()) < max_tasks;
             val = gmodel->GetNextValue(var, val)) {
            tasks.emplace_back(var, val, tasks.size());
        }
    }
    return tasks;
}

// Collect ALL (var, val) pairs in current domain (for full SAC)
std::vector<ProbeTask> CollectAllDomainValues(GModel* gmodel) {
    std::vector<ProbeTask> tasks;
    const int num_vars = gmodel->GetNumVars();
    for (int var = 0; var < num_vars; ++var) {
        if (gmodel->IsAssigned(var)) continue;
        for (int val = gmodel->GetFirstValue(var);
             val != -1;
             val = gmodel->GetNextValue(var, val)) {
            tasks.emplace_back(var, val, tasks.size());
        }
    }
    return tasks;
}

// Calculate total domain size (forward declaration for SimulateMSACAssignments)
int GetTotalDomainSize(GModel* gmodel) {
    int total = 0;
    const int num_vars = gmodel->GetNumVars();
    for (int var = 0; var < num_vars; ++var) {
        total += gmodel->GetDomainSize(var);
    }
    return total;
}

// Helper: Run a single round of SAC (check all domain values, return failed ones)
// Returns true if SAC found failures (values to delete)
bool RunSACRound(GModel* gmodel, std::vector<int>& failed_vars,
                 std::vector<int>& failed_values, bool verbose) {
    failed_vars.clear();
    failed_values.clear();

    Batch2PersistentManager manager(gmodel, -1);
    manager.SetActivationStrategy(1);  // NEIGHBOR_ACTIVATION

    // Collect all (var, val) pairs
    auto tasks = CollectAllDomainValues(gmodel);
    if (tasks.empty()) return false;

    for (const auto& t : tasks) {
        manager.AddTask(t.var_id, t.value);
    }

    manager.ExecutePersistentBlocks(failed_vars, failed_values);
    manager.Clear();

    return !failed_values.empty();
}

// Helper: Run Full SAC until convergence (returns true if converged without DWO)
bool RunSACToConvergence(GModel* gmodel, int max_rounds, bool verbose) {
    for (int round = 0; round < max_rounds; ++round) {
        std::vector<int> failed_vars, failed_values;
        bool has_failures = RunSACRound(gmodel, failed_vars, failed_values, verbose);

        if (!has_failures) {
            // SAC converged
            return true;
        }

        if (verbose) {
            LOG(INFO) << "  SAC round " << (round + 1) << ": removing "
                      << failed_values.size() << " values";
        }

        // Remove failed values
        for (size_t i = 0; i < failed_vars.size(); ++i) {
            gmodel->RemoveValue(failed_vars[i], failed_values[i]);
        }

        // Re-enforce GAC
        auto gac_stats = gmodel->EnforceGAC_Persistent(false, -1);
        if (gac_stats.inconsistent) {
            if (verbose) {
                LOG(INFO) << "  DWO after SAC deletions";
            }
            return false;  // DWO
        }
    }
    return true;  // Max rounds reached, assume converged
}

// Simulate partial search: assign some variables with backtracking
// mode: "fast" = GAC backtracking (standard MAC), "full" = SAC backtracking (true MSAC)
//       "parallel" = Parallel SAC probe all values at once (GPU-optimized)
// Returns number of variables successfully assigned
int SimulateMSACAssignments(GModel* gmodel, int num_assigns, bool verbose,
                            const std::string& mode) {
    int assigned = 0;
    const int num_vars = gmodel->GetNumVars();
    const bool use_sac_backtrack = (mode == "full");
    const bool use_parallel_sac = (mode == "parallel");

    if (use_sac_backtrack && verbose) {
        LOG(INFO) << "Using full SAC backtracking (slow but complete)";
    }
    if (use_parallel_sac && verbose) {
        LOG(INFO) << "Using parallel SAC probe (GPU-optimized)";
    }

    // Strategy: for each variable to assign, try values with backtracking
    for (int var = 0; var < num_vars && assigned < num_assigns; ++var) {
        if (gmodel->IsAssigned(var)) continue;

        bool found_valid_value = false;
        int selected_value = -1;

        // In "parallel" mode, probe ALL values of this variable at once using GPU
        if (use_parallel_sac) {
            // Collect all values for this variable
            std::vector<ProbeTask> var_tasks;
            for (int val = gmodel->GetFirstValue(var);
                 val != -1;
                 val = gmodel->GetNextValue(var, val)) {
                var_tasks.emplace_back(var, val, var_tasks.size());
            }

            if (var_tasks.empty()) {
                if (verbose) LOG(WARNING) << "var=" << var << " has empty domain";
                break;
            }

            if (verbose) {
                LOG(INFO) << "Parallel probing var=" << var << " with "
                          << var_tasks.size() << " values";
            }

            // Probe all values in parallel using GPU
            Batch2PersistentManager manager(gmodel, -1);
            manager.SetActivationStrategy(1);  // NEIGHBOR_ACTIVATION
            for (const auto& t : var_tasks) {
                manager.AddTask(t.var_id, t.value);
            }
            std::vector<int> failed_vars, failed_values;
            manager.ExecutePersistentBlocks(failed_vars, failed_values);
            manager.Clear();

            // Build set of failed values for quick lookup
            std::set<int> failed_value_set(failed_values.begin(), failed_values.end());

            // Find first SAC-consistent value
            for (const auto& t : var_tasks) {
                if (failed_value_set.find(t.value) == failed_value_set.end()) {
                    // This value passed the SAC probe (no DWO)
                    selected_value = t.value;
                    break;
                }
            }

            if (selected_value == -1) {
                if (verbose) {
                    LOG(WARNING) << "var=" << var << ": all " << var_tasks.size()
                                 << " values failed SAC probe";
                }
                break;  // No valid value found
            }

            if (verbose) {
                LOG(INFO) << "var=" << var << ": " << failed_values.size() << "/"
                          << var_tasks.size() << " values failed, selected val="
                          << selected_value;
            }

            // Now actually assign the selected value
            gmodel->NewLevel();
            gmodel->AssignValue(var, selected_value);
            auto gac_stats = gmodel->EnforceGAC_Persistent(false, -1);
            if (gac_stats.inconsistent) {
                // This shouldn't happen if SAC probe was correct
                LOG(ERROR) << "Unexpected: SAC-consistent value caused GAC DWO";
                gmodel->BacktrackTo(gmodel->GetCurrentLevel() - 1);
                break;
            }

            found_valid_value = true;
            assigned++;
            if (verbose) {
                LOG(INFO) << "Assigned var=" << var << " = " << selected_value
                          << ", domain_size=" << GetTotalDomainSize(gmodel);
            }
        } else {
            // Original serial approach: try each value one by one
            for (int val = gmodel->GetFirstValue(var);
                 val != -1;
                 val = gmodel->GetNextValue(var, val)) {

                // Save state before trying this value
                int trail_marker = gmodel->GetCurrentLevel();
                gmodel->NewLevel();

                // Assign this value
                gmodel->AssignValue(var, val);

                // Run GAC propagation
                auto gac_stats = gmodel->EnforceGAC_Persistent(false, -1);
                if (gac_stats.inconsistent) {
                    if (verbose) {
                        LOG(INFO) << "  var=" << var << ", val=" << val
                                  << " -> GAC DWO, trying next value";
                    }
                    gmodel->BacktrackTo(trail_marker);
                    continue;
                }

                // In "full" MSAC mode, also check SAC consistency
                if (use_sac_backtrack) {
                    bool sac_ok = RunSACToConvergence(gmodel, 10, false);
                    if (!sac_ok) {
                        if (verbose) {
                            LOG(INFO) << "  var=" << var << ", val=" << val
                                      << " -> SAC DWO, trying next value";
                        }
                        gmodel->BacktrackTo(trail_marker);
                        continue;
                    }
                }

                // This value is consistent
                found_valid_value = true;
                assigned++;
                if (verbose) {
                    LOG(INFO) << "Assigned var=" << var << " = " << val
                              << ", domain_size=" << GetTotalDomainSize(gmodel);
                }
                break;
            }
        }

        if (!found_valid_value) {
            if (verbose) {
                LOG(WARNING) << "No valid value found for var=" << var
                             << " (all values lead to DWO)";
            }
            break;
        }
    }
    return assigned;
}

// ============================================================================
// Benchmark result structure
// ============================================================================

struct BenchmarkResult {
    std::string mode_name;
    double avg_time_ms;
    double min_time_ms;
    double max_time_ms;
    int num_failures;
    int num_probes;
    double probes_per_sec;
    bool valid;
    int num_unknown = 0;
    unsigned long long overflow_count = 0;
    unsigned long long processed_tasks = 0;
    unsigned long long constraint_checks = 0;
    unsigned long long stale_drop_count = 0;
    unsigned long long lock_fail_count = 0;
    unsigned long long lock_retry_count = 0;
    double avg_bucket_size = 0.0;
    double avg_bucket_utilization = 0.0;
    unsigned long long frontier_pop_count = 0;
    unsigned long long frontier_scan_steps = 0;
    double avg_frontier_scan_steps = 0.0;
    unsigned long long ow1_scatter_calls = 0;
    unsigned long long ow1_fallback_calls = 0;
    unsigned long long ow1_word_leader_writes = 0;
};

void PrintResult(const BenchmarkResult& result) {
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "  " << std::setw(15) << std::left << result.mode_name;
    std::cout << "  " << std::setw(10) << result.avg_time_ms << " ms";
    std::cout << "  [" << result.min_time_ms << " - " << result.max_time_ms << "]";
    std::cout << "  " << std::setw(6) << result.num_failures << " failures";
    std::cout << "  " << std::setw(6) << result.num_unknown << " unknown";
    std::cout << "  ovf=" << result.overflow_count;
    std::cout << "  " << std::setw(10) << result.probes_per_sec << " probes/s";
    std::cout << "  checks=" << result.constraint_checks;
    std::cout << "  stale=" << result.stale_drop_count;
    std::cout << "  lock_fail=" << result.lock_fail_count;
    std::cout << "  lock_retry=" << result.lock_retry_count;
    std::cout << "  bsz=" << std::setprecision(2) << result.avg_bucket_size;
    std::cout << "  butil=" << std::setprecision(2)
              << result.avg_bucket_utilization;
    std::cout << "  fpop=" << result.frontier_pop_count;
    std::cout << "  fscan=" << std::setprecision(2)
              << result.avg_frontier_scan_steps;
    std::cout << "  ow1_sc=" << result.ow1_scatter_calls;
    std::cout << "  ow1_fb=" << result.ow1_fallback_calls;
    std::cout << "  ow1_w=" << result.ow1_word_leader_writes;
    std::cout << std::endl;
}

// ============================================================================
// Batch-1 benchmark
// ============================================================================

BenchmarkResult RunBatch1Benchmark(GModel* gmodel,
                                    const std::vector<ProbeTask>& tasks,
                                    int warmup, int iterations) {
    BenchmarkResult result;
    result.mode_name = "Batch-1";
    result.num_probes = tasks.size();
    result.valid = false;

    BatchProbeManager manager(gmodel, 256);

    // Warmup
    for (int w = 0; w < warmup; ++w) {
        for (const auto& t : tasks) {
            manager.AddTask(t.var_id, t.value);
        }
        std::vector<int> failed_vars, failed_values;
        manager.ExecuteBatch(failed_vars, failed_values);
        manager.Clear();
    }

    // Benchmark
    std::vector<double> times;
    int total_failures = 0;

    for (int iter = 0; iter < iterations; ++iter) {
        for (const auto& t : tasks) {
            manager.AddTask(t.var_id, t.value);
        }

        std::vector<int> failed_vars, failed_values;

        auto start = std::chrono::high_resolution_clock::now();
        manager.ExecuteBatch(failed_vars, failed_values);
        auto end = std::chrono::high_resolution_clock::now();

        double time_ms = std::chrono::duration<double, std::milli>(end - start).count();
        times.push_back(time_ms);
        total_failures = failed_vars.size();
        manager.Clear();
    }

    // Calculate statistics
    result.min_time_ms = *std::min_element(times.begin(), times.end());
    result.max_time_ms = *std::max_element(times.begin(), times.end());
    result.avg_time_ms = 0;
    for (double t : times) result.avg_time_ms += t;
    result.avg_time_ms /= times.size();
    result.num_failures = total_failures;
    result.probes_per_sec = result.num_probes * 1000.0 / result.avg_time_ms;
    result.valid = true;

    return result;
}

// ============================================================================
// Stage 2 benchmark
// ============================================================================

BenchmarkResult RunStage2Benchmark(GModel* gmodel,
                                    const std::vector<ProbeTask>& tasks,
                                    int warmup, int iterations) {
    BenchmarkResult result;
    result.mode_name = "Stage-2";
    result.num_probes = tasks.size();
    result.valid = false;

    Batch2PersistentManager manager(gmodel);

    // Warmup
    for (int w = 0; w < warmup; ++w) {
        for (const auto& t : tasks) {
            manager.AddTask(t.var_id, t.value);
        }
        std::vector<int> failed_vars, failed_values;
        manager.ExecutePersistentBlocks(failed_vars, failed_values);
        manager.Clear();
    }

    // Benchmark
    std::vector<double> times;
    int total_failures = 0;

    for (int iter = 0; iter < iterations; ++iter) {
        for (const auto& t : tasks) {
            manager.AddTask(t.var_id, t.value);
        }

        std::vector<int> failed_vars, failed_values;

        auto start = std::chrono::high_resolution_clock::now();
        manager.ExecutePersistentBlocks(failed_vars, failed_values);
        auto end = std::chrono::high_resolution_clock::now();

        double time_ms = std::chrono::duration<double, std::milli>(end - start).count();
        times.push_back(time_ms);
        total_failures = failed_vars.size();
        manager.Clear();
    }

    // Calculate statistics
    result.min_time_ms = *std::min_element(times.begin(), times.end());
    result.max_time_ms = *std::max_element(times.begin(), times.end());
    result.avg_time_ms = 0;
    for (double t : times) result.avg_time_ms += t;
    result.avg_time_ms /= times.size();
    result.num_failures = total_failures;
    result.probes_per_sec = result.num_probes * 1000.0 / result.avg_time_ms;
    result.valid = true;

    return result;
}

// ============================================================================
// Batch-3A benchmark
// ============================================================================

BenchmarkResult RunBatch3ABenchmark(GModel* gmodel,
                                     const std::vector<ProbeTask>& tasks,
                                     int warmup, int iterations) {
    BenchmarkResult result;
    result.mode_name = "Batch-3A";
    result.num_probes = tasks.size();
    result.valid = false;

    Batch3AManager manager(gmodel, -1, 32);
    ConfigureBatch3AManager(manager);

    // Check if suitable for Batch-3A
    if (!manager.IsSuitableForBatch3A()) {
        LOG(WARNING) << "Instance not suitable for Batch-3A (bitSup too large)";
        result.mode_name = "Batch-3A (N/A)";
        return result;
    }

    // Warmup
    for (int w = 0; w < warmup; ++w) {
        for (const auto& t : tasks) {
            manager.AddTask(t.var_id, t.value);
        }
        std::vector<int> failed_vars, failed_values;
        manager.Execute(failed_vars, failed_values);
        manager.Clear();
    }

    // Benchmark
    std::vector<double> times;
    int total_failures = 0;

    for (int iter = 0; iter < iterations; ++iter) {
        for (const auto& t : tasks) {
            manager.AddTask(t.var_id, t.value);
        }

        std::vector<int> failed_vars, failed_values;

        auto start = std::chrono::high_resolution_clock::now();
        manager.Execute(failed_vars, failed_values);
        auto end = std::chrono::high_resolution_clock::now();

        double time_ms = std::chrono::duration<double, std::milli>(end - start).count();
        times.push_back(time_ms);
        total_failures = failed_vars.size();
        manager.Clear();
    }

    // Calculate statistics
    result.min_time_ms = *std::min_element(times.begin(), times.end());
    result.max_time_ms = *std::max_element(times.begin(), times.end());
    result.avg_time_ms = 0;
    for (double t : times) result.avg_time_ms += t;
    result.avg_time_ms /= times.size();
    result.num_failures = total_failures;
    result.probes_per_sec = result.num_probes * 1000.0 / result.avg_time_ms;
    result.valid = true;

    return result;
}

// ============================================================================
// FQ-PT benchmark
// ============================================================================

BenchmarkResult RunFQPTBenchmark(GModel* gmodel,
                                 const std::vector<ProbeTask>& tasks,
                                 int warmup, int iterations) {
    BenchmarkResult result;
    if (FLAGS_fqpt_enable_world_owner) {
        if (FLAGS_fqpt_enable_ow1_frontier_scatter) {
            result.mode_name = FLAGS_fqpt_ow1_scatter_mode == 2 ? "FQ-PT(OWF+OW1m2)"
                                                                 : "FQ-PT(OWF+OW1)";
        } else {
            result.mode_name = "FQ-PT(OWF)";
        }
    } else {
        result.mode_name = "FQ-PT";
    }
    result.num_probes = tasks.size();
    result.valid = false;

    FQPTBaselineManager manager(gmodel, FLAGS_fqpt_num_blocks);
    ConfigureFQPTManager(manager);

    for (int w = 0; w < warmup; ++w) {
        for (const auto& t : tasks) {
            manager.AddTask(t.var_id, t.value);
        }
        std::vector<int> failed_vars, failed_values, unknown_vars, unknown_values;
        manager.Execute(failed_vars, failed_values, &unknown_vars, &unknown_values);
        manager.Clear();
    }

    std::vector<double> times;
    int total_failures = 0;
    int total_unknown = 0;
    unsigned long long total_overflow = 0;
    unsigned long long total_processed = 0;
    unsigned long long total_checks = 0;
    unsigned long long total_stale_drop = 0;
    unsigned long long total_lock_fail = 0;
    unsigned long long total_lock_retry = 0;
    double total_bucket_size = 0.0;
    double total_bucket_util = 0.0;
    unsigned long long total_frontier_pop = 0;
    unsigned long long total_frontier_scan = 0;
    double total_avg_frontier_scan = 0.0;
    unsigned long long total_ow1_scatter_calls = 0;
    unsigned long long total_ow1_fallback_calls = 0;
    unsigned long long total_ow1_word_leader_writes = 0;

    for (int iter = 0; iter < iterations; ++iter) {
        for (const auto& t : tasks) {
            manager.AddTask(t.var_id, t.value);
        }

        std::vector<int> failed_vars, failed_values, unknown_vars, unknown_values;
        auto start = std::chrono::high_resolution_clock::now();
        manager.Execute(failed_vars, failed_values, &unknown_vars, &unknown_values);
        auto end = std::chrono::high_resolution_clock::now();

        times.push_back(std::chrono::duration<double, std::milli>(end - start).count());
        total_failures = static_cast<int>(failed_vars.size());
        total_unknown = static_cast<int>(unknown_vars.size());
        const auto& st = manager.GetLastStatistics();
        total_overflow += st.overflow_count;
        total_processed += st.processed_tasks;
        total_checks += st.constraint_checks;
        total_stale_drop += st.stale_drop_count;
        total_lock_fail += st.lock_fail_count;
        total_lock_retry += st.lock_retry_count;
        total_bucket_size += st.avg_bucket_size;
        total_bucket_util += st.avg_bucket_utilization;
        total_frontier_pop += st.frontier_pop_count;
        total_frontier_scan += st.frontier_scan_steps;
        total_avg_frontier_scan += st.avg_frontier_scan_steps;
        total_ow1_scatter_calls += st.ow1_scatter_calls;
        total_ow1_fallback_calls += st.ow1_fallback_calls;
        total_ow1_word_leader_writes += st.ow1_word_leader_writes;
        manager.Clear();
    }

    result.min_time_ms = *std::min_element(times.begin(), times.end());
    result.max_time_ms = *std::max_element(times.begin(), times.end());
    result.avg_time_ms = 0;
    for (double t : times) result.avg_time_ms += t;
    result.avg_time_ms /= times.size();
    result.num_failures = total_failures;
    result.num_unknown = total_unknown;
    result.overflow_count = total_overflow / std::max(1, iterations);
    result.processed_tasks = total_processed / std::max(1, iterations);
    result.constraint_checks = total_checks / std::max(1, iterations);
    result.stale_drop_count = total_stale_drop / std::max(1, iterations);
    result.lock_fail_count = total_lock_fail / std::max(1, iterations);
    result.lock_retry_count = total_lock_retry / std::max(1, iterations);
    result.avg_bucket_size = total_bucket_size / std::max(1, iterations);
    result.avg_bucket_utilization = total_bucket_util / std::max(1, iterations);
    result.frontier_pop_count = total_frontier_pop / std::max(1, iterations);
    result.frontier_scan_steps = total_frontier_scan / std::max(1, iterations);
    result.avg_frontier_scan_steps =
        total_avg_frontier_scan / std::max(1, iterations);
    result.ow1_scatter_calls = total_ow1_scatter_calls / std::max(1, iterations);
    result.ow1_fallback_calls = total_ow1_fallback_calls / std::max(1, iterations);
    result.ow1_word_leader_writes =
        total_ow1_word_leader_writes / std::max(1, iterations);
    result.probes_per_sec = result.num_probes * 1000.0 / result.avg_time_ms;
    result.valid = true;
    return result;
}

// ============================================================================
// Full SAC convergence result structure
// ============================================================================

int PercentileInt(const std::vector<int>& values, double p) {
    if (values.empty()) return 0;
    const double pp = std::clamp(p, 0.0, 1.0);
    const size_t n = values.size();
    const size_t idx = std::min(n - 1, static_cast<size_t>(std::ceil(pp * n) - 1));
    std::vector<int> tmp(values.begin(), values.end());
    std::nth_element(tmp.begin(), tmp.begin() + idx, tmp.end());
    return tmp[idx];
}

struct FullSACResult {
    std::string mode_name;
    double total_time_ms;
    int num_rounds;              // SAC rounds until convergence
    int total_probes;            // Total probes across all rounds
    int total_deletions;         // Total values deleted
    int initial_domain_size;     // Sum of domain sizes before SAC
    int final_domain_size;       // Sum of domain sizes after SAC
    bool converged;              // true if reached fixed point
    bool inconsistent;           // true if detected DWO
    std::vector<int> round_probes;    // Probes per round
    std::vector<int> round_deletions; // Deletions per round
    std::vector<double> round_times;  // Time per round (ms)

    // P0-3: Per-probe iteration statistics
    int64_t total_iterations = 0;
    int p95_iterations = 0;
    int max_iterations = 0;
    int unknown_count = 0;
    double avg_iterations() const {
        return total_probes > 0 ? static_cast<double>(total_iterations) / total_probes : 0.0;
    }
};

void PrintFullSACResult(const FullSACResult& result) {
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "\n--- Full SAC Result (" << result.mode_name << ") ---\n";
    std::cout << "  Total time:      " << result.total_time_ms << " ms\n";
    std::cout << "  Rounds:          " << result.num_rounds << "\n";
    std::cout << "  Total probes:    " << result.total_probes << "\n";
    std::cout << "  Total deletions: " << result.total_deletions << "\n";
    std::cout << "  Domain size:     " << result.initial_domain_size
              << " -> " << result.final_domain_size
              << " (reduced " << (result.initial_domain_size - result.final_domain_size) << ")\n";
    // Note: In simulated MSAC, "inconsistent" means this search path leads to
    // SAC-DWO. Real MSAC would backtrack and try another value.
    std::cout << "  Status:          "
              << (result.inconsistent ? "SAC-DWO (path inconsistent, needs backtrack)" :
                  (result.converged ? "SAC-consistent (converged)" : "TIMEOUT"))
              << "\n";

    // P0-3: Per-probe iteration statistics
    std::cout << "  Avg iterations:  " << result.avg_iterations() << "\n";
    std::cout << "  P95 iterations:  " << result.p95_iterations << "\n";
    std::cout << "  Max iterations:  " << result.max_iterations << "\n";
    std::cout << "  Unknown probes:  " << result.unknown_count
              << " (" << (result.total_probes > 0 ? 100.0 * result.unknown_count / result.total_probes : 0.0) << "%)\n";

    if (FLAGS_verbose && !result.round_probes.empty()) {
        std::cout << "\n  Per-round details:\n";
        for (size_t i = 0; i < result.round_probes.size(); ++i) {
            std::cout << "    Round " << (i + 1) << ": "
                      << result.round_probes[i] << " probes, "
                      << result.round_deletions[i] << " deletions, "
                      << result.round_times[i] << " ms\n";
        }
    }
}

// ============================================================================
// Full SAC convergence test (using Stage-2 Persistent Blocks)
// ============================================================================

FullSACResult RunFullSAC_Stage2(GModel* gmodel, int max_rounds, bool verbose, bool nsac_mask = true) {
    FullSACResult result;
    result.mode_name = nsac_mask ? "Stage-2 (NSAC)" : "Stage-2 (no NSAC)";
    result.converged = false;
    result.inconsistent = false;
    result.num_rounds = 0;
    result.total_probes = 0;
    result.total_deletions = 0;
    result.initial_domain_size = GetTotalDomainSize(gmodel);

    auto total_start = std::chrono::high_resolution_clock::now();

    // P0-2: Initialize NSAC mask if enabled
    if (nsac_mask) {
        if (!gmodel->IsAllowedMasksBuilt()) {
            gmodel->BuildAllowedMasks();
        }
        gmodel->SetNSACMaskEnabled(true);
        std::cout << "  NSAC mask:       enabled (bitmap_words="
                  << gmodel->GetGModelDataView().constraint_bitmap_words << ")\n";
    } else {
        gmodel->SetNSACMaskEnabled(false);
        std::cout << "  NSAC mask:       disabled\n";
    }

    // Create manager with auto-tuned blocks
    Batch2PersistentManager manager(gmodel, -1);
    manager.SetActivationStrategy(1);  // NEIGHBOR_ACTIVATION
    manager.EnableStats(true);  // P0-3: Enable per-probe statistics

    std::vector<int> all_probe_iterations;
    all_probe_iterations.reserve(static_cast<size_t>(result.initial_domain_size));

    for (int round = 0; round < max_rounds; ++round) {
        // Collect all (var, val) pairs in current domain
        auto tasks = CollectAllDomainValues(gmodel);

        if (tasks.empty()) {
            // All variables are singletons - SAC converged
            result.converged = true;
            break;
        }

        if (verbose) {
            LOG(INFO) << "Round " << (round + 1) << ": " << tasks.size() << " probes";
        }

        // Add all tasks
        for (const auto& t : tasks) {
            manager.AddTask(t.var_id, t.value);
        }

        // Execute batch probes
        std::vector<int> failed_vars, failed_values;
        auto round_start = std::chrono::high_resolution_clock::now();
        manager.ExecutePersistentBlocks(failed_vars, failed_values);
        auto round_end = std::chrono::high_resolution_clock::now();
        double round_time = std::chrono::duration<double, std::milli>(round_end - round_start).count();

        // P0-3: Collect per-probe iteration statistics
        const auto& stats = manager.GetLastStatistics();
        result.total_iterations += stats.total_iterations;
        if (stats.max_iterations > result.max_iterations) {
            result.max_iterations = stats.max_iterations;
        }
        result.unknown_count += stats.unknown_count;
        const auto& probe_iters = manager.GetLastProbeIterations();
        if (!probe_iters.empty()) {
            all_probe_iterations.insert(all_probe_iterations.end(),
                                        probe_iters.begin(),
                                        probe_iters.end());
        }

        manager.Clear();

        // Record round stats
        result.round_probes.push_back(tasks.size());
        result.round_deletions.push_back(failed_values.size());
        result.round_times.push_back(round_time);
        result.total_probes += tasks.size();
        result.total_deletions += failed_values.size();
        result.num_rounds++;

        if (failed_values.empty()) {
            // No deletions - SAC converged
            result.converged = true;
            break;
        }

        // Remove failed values from domain
        if (verbose) {
            LOG(INFO) << "  Removing " << failed_values.size() << " values:";
            for (size_t i = 0; i < failed_vars.size() && i < 10; ++i) {
                LOG(INFO) << "    var=" << failed_vars[i] << ", val=" << failed_values[i];
            }
            if (failed_vars.size() > 10) {
                LOG(INFO) << "    ... and " << (failed_vars.size() - 10) << " more";
            }
        }

        for (size_t i = 0; i < failed_vars.size(); ++i) {
            gmodel->RemoveValue(failed_vars[i], failed_values[i]);
        }

        // Re-enforce GAC after deletions
        auto gac_stats = gmodel->EnforceGAC_Persistent(false, -1);
        if (gac_stats.inconsistent) {
            result.inconsistent = true;
            if (verbose) {
                LOG(INFO) << "  DWO detected after GAC propagation";
                // Debug: print which variable has empty domain
                for (int v = 0; v < gmodel->GetNumVars(); ++v) {
                    if (gmodel->GetDomainSize(v) == 0) {
                        LOG(INFO) << "  Variable " << v << " has empty domain!";
                    }
                }
            }
            break;
        }
    }

    auto total_end = std::chrono::high_resolution_clock::now();
    result.total_time_ms = std::chrono::duration<double, std::milli>(total_end - total_start).count();
    result.final_domain_size = GetTotalDomainSize(gmodel);
    result.p95_iterations = PercentileInt(all_probe_iterations, 0.95);

    return result;
}

// ============================================================================
// Full SAC convergence test (using Batch-3A)
// ============================================================================

FullSACResult RunFullSAC_Batch3A(GModel* gmodel, int max_rounds, bool verbose) {
    FullSACResult result;
    result.mode_name = "Batch-3A";
    result.converged = false;
    result.inconsistent = false;
    result.num_rounds = 0;
    result.total_probes = 0;
    result.total_deletions = 0;
    result.initial_domain_size = GetTotalDomainSize(gmodel);

    auto total_start = std::chrono::high_resolution_clock::now();

    // Create Batch-3A manager
    Batch3AManager manager(gmodel, -1, 32);
    ConfigureBatch3AManager(manager);

    if (!manager.IsSuitableForBatch3A()) {
        LOG(WARNING) << "Instance not suitable for Batch-3A (bitSup too large)";
        result.mode_name = "Batch-3A (N/A)";
        return result;
    }

    for (int round = 0; round < max_rounds; ++round) {
        auto tasks = CollectAllDomainValues(gmodel);

        if (tasks.empty()) {
            result.converged = true;
            break;
        }

        if (verbose) {
            LOG(INFO) << "Round " << (round + 1) << ": " << tasks.size() << " probes";
        }

        for (const auto& t : tasks) {
            manager.AddTask(t.var_id, t.value);
        }

        std::vector<int> failed_vars, failed_values;
        auto round_start = std::chrono::high_resolution_clock::now();
        manager.Execute(failed_vars, failed_values);
        auto round_end = std::chrono::high_resolution_clock::now();
        double round_time = std::chrono::duration<double, std::milli>(round_end - round_start).count();

        manager.Clear();

        result.round_probes.push_back(tasks.size());
        result.round_deletions.push_back(failed_values.size());
        result.round_times.push_back(round_time);
        result.total_probes += tasks.size();
        result.total_deletions += failed_values.size();
        result.num_rounds++;

        if (failed_values.empty()) {
            result.converged = true;
            break;
        }

        if (verbose) {
            LOG(INFO) << "  Removing " << failed_values.size() << " values";
        }

        for (size_t i = 0; i < failed_vars.size(); ++i) {
            gmodel->RemoveValue(failed_vars[i], failed_values[i]);
        }

        auto gac_stats = gmodel->EnforceGAC_Persistent(false, -1);
        if (gac_stats.inconsistent) {
            result.inconsistent = true;
            break;
        }
    }

    auto total_end = std::chrono::high_resolution_clock::now();
    result.total_time_ms = std::chrono::duration<double, std::milli>(total_end - total_start).count();
    result.final_domain_size = GetTotalDomainSize(gmodel);

    return result;
}

// ============================================================================
// SAC preprocess (SAC1 / SAC3) result structure
// ============================================================================

struct SacPreprocessResult {
    std::string mode_name;
    double total_time_ms = 0.0;
    int total_probes = 0;
    int total_deletions = 0;
    int initial_domain_size = 0;
    int final_domain_size = 0;
    bool inconsistent = false;
    bool early_stopped = false;

    int64_t total_iterations = 0;
    int p95_iterations = 0;
    int max_iterations = 0;
    int unknown_probes = 0;

    double avg_iterations() const {
        return total_probes > 0 ? static_cast<double>(total_iterations) / total_probes : 0.0;
    }
};

void PrintSacPreprocessResult(const SacPreprocessResult& result) {
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "\n--- SAC Preprocess Result (" << result.mode_name << ") ---\n";
    std::cout << "  Total time:      " << result.total_time_ms << " ms\n";
    std::cout << "  Total probes:    " << result.total_probes << "\n";
    std::cout << "  Total deletions: " << result.total_deletions << "\n";
    std::cout << "  Domain size:     " << result.initial_domain_size
              << " -> " << result.final_domain_size
              << " (reduced " << (result.initial_domain_size - result.final_domain_size) << ")\n";
    std::cout << "  Status:          "
              << (result.inconsistent ? "SAC-DWO" :
                  (result.early_stopped ? "TIMEOUT" : "SAC-consistent"))
              << "\n";
    std::cout << "  Avg iterations:  " << result.avg_iterations() << "\n";
    std::cout << "  P95 iterations:  " << result.p95_iterations << "\n";
    std::cout << "  Max iterations:  " << result.max_iterations << "\n";
    std::cout << "  Unknown probes:  " << result.unknown_probes
              << " (" << (result.total_probes > 0
                           ? 100.0 * result.unknown_probes / result.total_probes
                           : 0.0)
              << "%)\n";
}

SacPreprocessResult RunSacPreprocessWithSolver(GModel* gmodel,
                                               bool use_sac3,
                                               int max_rounds,
                                               bool verbose,
                                               bool nsac_mask) {
    SacPreprocessResult result;
    result.mode_name = use_sac3 ? (nsac_mask ? "SAC3 (NSACQ-GPU)" : "SAC3 (SACQ-GPU)")
                                : (nsac_mask ? "SAC1 (NSAC)" : "SAC1 (no NSAC)");
    result.initial_domain_size = GetTotalDomainSize(gmodel);

    GModelSolver solver(gmodel, verbose);

    GModelSolver::NSACMaskConfig mask_cfg;
    mask_cfg.enabled = nsac_mask;
    solver.SetNSACMaskConfig(mask_cfg);

    // Benchmark 默认关闭 Batch-3A，避免把“约束聚合”的开销混入 SAC1/SAC3 对照。
    GModelSolver::Batch3AConfig b3a_cfg;
    b3a_cfg.enabled = false;
    solver.SetBatch3AConfig(b3a_cfg);

    // P1-2: 外层队列预算（仅影响 SAC3）
    GModelSolver::SacQueueBudgetConfig budget_cfg;
    budget_cfg.enabled = FLAGS_sac_queue_budget;
    budget_cfg.max_total_probes = FLAGS_sac_max_total_probes;
    budget_cfg.max_queue_size = FLAGS_sac_max_queue_size;
    budget_cfg.max_total_requeues = FLAGS_sac_max_total_requeues;
    budget_cfg.max_requeues_per_var = FLAGS_sac_max_requeues_per_var;
    if (budget_cfg.max_total_probes > 0 || budget_cfg.max_queue_size > 0 ||
        budget_cfg.max_total_requeues > 0 || budget_cfg.max_requeues_per_var > 0) {
        budget_cfg.enabled = true;
    }
    solver.SetSacQueueBudgetConfig(budget_cfg);

    // P0-3: 观测入口需要 per-probe iterations 分布
    solver.EnableSacProbeStats(true);

    // 为了让 benchmark 的 “TIMEOUT” 语义与 full_sac 对齐：只用 max_rounds 截断；
    // wall-time 超时由 Python 脚本的 subprocess timeout 控制。
    auto sac_cfg = solver.GetSACConfig();
    sac_cfg.early_stop_enabled = true;
    sac_cfg.max_rounds = max_rounds;
    sac_cfg.time_budget_ms = 1e18;   // effectively disabled
    sac_cfg.min_deletion_rate = 0.0; // disable deletion-rate early stop
    sac_cfg.warmup_rounds = max_rounds;
    solver.SetSACConfig(sac_cfg);

    GpuSearchStatistics stats;
    int rc = use_sac3 ? solver.EnforceSAC3(stats) : solver.EnforceSAC1(stats);
    result.inconsistent = (rc < 0);
    result.early_stopped = solver.WasLastSacEarlyStopped();

    result.total_time_ms = stats.sac_time * 1000.0;
    result.total_probes = stats.sac_probes;
    result.total_deletions = stats.sac_deletions;
    result.total_iterations = solver.GetLastSacTotalIterations();
    result.max_iterations = solver.GetLastSacMaxIterations();
    result.p95_iterations = PercentileInt(solver.GetLastSacProbeIterations(), 0.95);
    result.unknown_probes = solver.GetLastSacUnknownProbes();
    result.final_domain_size = GetTotalDomainSize(gmodel);

    return result;
}

}  // namespace cpim

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    google::InitGoogleLogging(argv[0]);
    gflags::ParseCommandLineFlags(&argc, &argv, true);

    if (FLAGS_input.empty()) {
        LOG(ERROR) << "Please specify input file with --input=<file.xml>";
        return 1;
    }

    // Parse model
    LOG(INFO) << "Loading instance: " << FLAGS_input;
    auto parser = cpim::XcspParser::Create(cpim::ParserType::kLibXml2);
    auto model_or = parser->Parse(FLAGS_input);
    if (!model_or.ok()) {
        LOG(ERROR) << "Failed to parse model: " << model_or.status();
        return 1;
    }

    cpim::ModelNormalizer normalizer;
    auto normalized_or = normalizer.Normalize(*model_or);
    if (!normalized_or.ok()) {
        LOG(ERROR) << "Normalization failed: " << normalized_or.status();
        return 1;
    }
    const auto& im_model = *normalized_or;

    // Build GModel
    LOG(INFO) << "Building GModel...";
    auto gmodel_obj = cpim::model::GModelAdapter::Build(im_model);
    cpim::GModel* gmodel = &gmodel_obj;

    LOG(INFO) << "Model info: " << gmodel->GetNumVars() << " vars, "
              << gmodel->GetNumCons() << " constraints, "
              << "max_dom_size=" << gmodel->max_dom_size << ", "
              << "bit_dom_int_size=" << gmodel->GetBitDomIntSize();

    // Run initial GAC
    LOG(INFO) << "Running initial GAC propagation...";
    auto gac_stats = gmodel->EnforceGAC_Persistent(false, -1);
    if (gac_stats.inconsistent) {
        LOG(INFO) << "Initial GAC detected inconsistency (UNSAT instance)";
        return 0;
    }

    // ========================================
    // Full SAC convergence mode (or simulated MSAC)
    // ========================================
    if (FLAGS_mode == "full_sac") {
        std::cout << "\n========================================\n";
        if (FLAGS_assign_count > 0) {
            std::cout << "Simulated MSAC Test (assign " << FLAGS_assign_count << " vars first)\n";
        } else {
            std::cout << "Full SAC Convergence Test\n";
        }
        std::cout << "Instance: " << FLAGS_input << "\n";
        std::cout << "Max rounds: " << FLAGS_max_sac_rounds << "\n";
        std::cout << "========================================\n";

        int initial_domain_size = cpim::GetTotalDomainSize(gmodel);
        LOG(INFO) << "Initial domain size (after GAC): " << initial_domain_size;

        // Simulate MSAC: assign some variables first
        if (FLAGS_assign_count > 0) {
            LOG(INFO) << "Simulating MSAC: assigning " << FLAGS_assign_count
                      << " variables (mode=" << FLAGS_msac_mode << ")...";
            int actually_assigned = cpim::SimulateMSACAssignments(
                gmodel, FLAGS_assign_count, FLAGS_verbose, FLAGS_msac_mode);
            std::cout << "\nAssigned " << actually_assigned << " variables"
                      << " (msac_mode=" << FLAGS_msac_mode << ")\n";
            std::cout << "Domain size after assignments: "
                      << cpim::GetTotalDomainSize(gmodel) << "\n";

            if (actually_assigned == 0) {
                LOG(WARNING) << "Could not assign any variables";
            }
        }

        // Run Full SAC with Stage-2
        LOG(INFO) << "Running Full SAC with Stage-2 (nsac_mask=" << (FLAGS_nsac_mask ? "true" : "false") << ")...";
        auto stage2_result = cpim::RunFullSAC_Stage2(gmodel, FLAGS_max_sac_rounds, FLAGS_verbose, FLAGS_nsac_mask);
        cpim::PrintFullSACResult(stage2_result);

        // Explain the result
        if (FLAGS_assign_count > 0 && stage2_result.inconsistent) {
            if (FLAGS_msac_mode == "fast") {
                std::cout << "\nNote: In 'fast' mode, we use GAC backtracking for assignments.\n"
                          << "SAC-DWO means the first GAC-consistent path led to SAC failure.\n"
                          << "Use --msac_mode=full for true SAC backtracking (slower but complete).\n";
            } else {
                std::cout << "\nNote: SAC-DWO with 'full' mode suggests the problem may be UNSAT.\n";
            }
        }

        std::cout << "\n========================================\n";
        std::cout << "Full SAC complete!\n";
        std::cout << "========================================\n";
        return 0;
    }

    // ========================================
    // SAC preprocess modes (SAC1 / SAC3)
    // ========================================
    if (FLAGS_mode == "sac1_preprocess" || FLAGS_mode == "sac3_preprocess") {
        std::cout << "\n========================================\n";
        std::cout << "SAC Preprocess Benchmark (" << FLAGS_mode << ")\n";
        std::cout << "Instance: " << FLAGS_input << "\n";
        std::cout << "Max rounds: " << FLAGS_max_sac_rounds << "\n";
        std::cout << "NSAC mask: " << (FLAGS_nsac_mask ? "enabled" : "disabled") << "\n";
        if (FLAGS_sac_queue_budget || FLAGS_sac_max_total_probes > 0 ||
            FLAGS_sac_max_queue_size > 0 || FLAGS_sac_max_total_requeues > 0 ||
            FLAGS_sac_max_requeues_per_var > 0) {
            std::cout << "Queue budget: enabled"
                      << " (max_total_probes=" << FLAGS_sac_max_total_probes
                      << ", max_queue_size=" << FLAGS_sac_max_queue_size
                      << ", max_total_requeues=" << FLAGS_sac_max_total_requeues
                      << ", max_requeues_per_var=" << FLAGS_sac_max_requeues_per_var
                      << ")\n";
        }
        std::cout << "========================================\n";

        const bool use_sac3 = (FLAGS_mode == "sac3_preprocess");
        auto result = cpim::RunSacPreprocessWithSolver(
            gmodel,
            use_sac3,
            FLAGS_max_sac_rounds,
            FLAGS_verbose,
            FLAGS_nsac_mask);
        cpim::PrintSacPreprocessResult(result);
        return 0;
    }

    // ========================================
    // Throughput benchmark modes
    // ========================================

    // Generate probe tasks
    auto tasks = cpim::CollectProbeTasks(gmodel, FLAGS_num_probes);
    LOG(INFO) << "Generated " << tasks.size() << " probe tasks";

    if (tasks.empty()) {
        LOG(INFO) << "No probe tasks available (all variables are singletons)";
        return 0;
    }

    // Print header
    std::cout << "\n========================================\n";
    std::cout << "SAC-GPU Throughput Benchmark\n";
    std::cout << "Instance: " << FLAGS_input << "\n";
    std::cout << "Probes: " << tasks.size() << "\n";
    std::cout << "Warmup: " << FLAGS_warmup << ", Iterations: " << FLAGS_iterations << "\n";
    std::cout << "========================================\n\n";

    std::vector<cpim::BenchmarkResult> results;

    // Run benchmarks based on mode
    if (FLAGS_mode == "batch1" || FLAGS_compare) {
        LOG(INFO) << "Running Batch-1 benchmark...";
        auto result = cpim::RunBatch1Benchmark(gmodel, tasks, FLAGS_warmup, FLAGS_iterations);
        results.push_back(result);
        cpim::PrintResult(result);
    }

    if (FLAGS_mode == "stage2" || FLAGS_compare) {
        LOG(INFO) << "Running Stage-2 benchmark...";
        auto result = cpim::RunStage2Benchmark(gmodel, tasks, FLAGS_warmup, FLAGS_iterations);
        results.push_back(result);
        cpim::PrintResult(result);
    }

    if (FLAGS_mode == "batch3a" || FLAGS_compare) {
        LOG(INFO) << "Running Batch-3A benchmark...";
        auto result = cpim::RunBatch3ABenchmark(gmodel, tasks, FLAGS_warmup, FLAGS_iterations);
        results.push_back(result);
        cpim::PrintResult(result);
    }

    if (FLAGS_mode == "fqpt" || FLAGS_compare) {
        LOG(INFO) << "Running FQ-PT benchmark...";
        auto result = cpim::RunFQPTBenchmark(gmodel, tasks, FLAGS_warmup, FLAGS_iterations);
        results.push_back(result);
        cpim::PrintResult(result);
    }

    // Print comparison if multiple modes
    if (results.size() > 1) {
        std::cout << "\n--- Comparison ---\n";
        // Find baseline (Stage-2)
        double baseline_time = 0;
        for (const auto& r : results) {
            if (r.mode_name == "Stage-2" && r.valid) {
                baseline_time = r.avg_time_ms;
                break;
            }
        }
        if (baseline_time > 0) {
            for (const auto& r : results) {
                if (r.valid) {
                    double speedup = baseline_time / r.avg_time_ms;
                    std::cout << "  " << r.mode_name << " vs Stage-2: "
                              << std::fixed << std::setprecision(2) << speedup << "x\n";
                }
            }
        }
    }

    std::cout << "\n========================================\n";
    std::cout << "Benchmark complete!\n";
    std::cout << "========================================\n";

    return 0;
}
