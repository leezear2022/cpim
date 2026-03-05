#pragma once

// ============================================================================
// SAC-GPU Configuration for Ablation Study
// ============================================================================
// This file provides compile-time and runtime configuration options for
// different SAC-GPU implementations. Use these options to enable/disable
// specific optimizations for ablation experiments.
// ============================================================================

namespace cpim {
namespace sac_gpu {

// ============================================================================
// Compile-time Switches (for ablation study)
// ============================================================================

// Batch level selection
#ifndef SAC_GPU_BATCH1_ENABLED
#define SAC_GPU_BATCH1_ENABLED 1  // Time-dimension batching
#endif

#ifndef SAC_GPU_BATCH2_ENABLED
#define SAC_GPU_BATCH2_ENABLED 1  // Space-dimension batching
#endif

#ifndef SAC_GPU_BATCH3A_ENABLED
#define SAC_GPU_BATCH3A_ENABLED 1  // Constraint aggregation
#endif

// Stage 2 optimizations
#ifndef STAGE2_AUTO_NUM_BLOCKS
#define STAGE2_AUTO_NUM_BLOCKS 1  // P1-1: Auto-tune num_blocks
#endif

#ifndef STAGE2_CHUNK_PULL
#define STAGE2_CHUNK_PULL 1  // P1-2: Batch task pulling
#endif

#ifndef STAGE2_PER_TASK_STATS
#define STAGE2_PER_TASK_STATS 1  // P2: Per-task statistics
#endif

// Batch-3A optimizations
#ifndef BATCH3A_SPARSE_QUEUE
#define BATCH3A_SPARSE_QUEUE 1  // P3-3: Sparse task queue
#endif

#ifndef BATCH3A_MULTI_BLOCK
#define BATCH3A_MULTI_BLOCK 1  // P4: Multi-block world sharding
#endif

#ifndef BATCH3A_WARP_PER_WORD
#define BATCH3A_WARP_PER_WORD 0  // P5: Warp-per-Word (not implemented)
#endif

// Common optimizations
#ifndef FRONTIER_ACTIVATION_STRATEGY
#define FRONTIER_ACTIVATION_STRATEGY 1  // 0=FULL, 1=NEIGHBOR
#endif

#ifndef CHEAP_PRECHECK_ENABLED
#define CHEAP_PRECHECK_ENABLED 1  // P0-2: Fast fail detection
#endif

// ============================================================================
// Runtime Configuration
// ============================================================================

enum class BatchMode {
    BATCH1,         // Time-dimension batching (serial probes)
    BATCH2_STAGE1,  // Space-dimension: Micro-Batch
    BATCH2_STAGE2,  // Space-dimension: Persistent Blocks (default)
    BATCH3A,        // Constraint aggregation
    AUTO            // Automatic selection based on problem characteristics
};

enum class FrontierStrategy {
    FULL_ACTIVATION,     // Activate all constraints
    NEIGHBOR_ACTIVATION  // Activate only neighbor constraints
};

struct SACGPUConfig {
    // Batch mode selection
    BatchMode batch_mode = BatchMode::BATCH2_STAGE2;

    // Stage 2 configuration
    bool auto_num_blocks = true;
    int fixed_num_blocks = 32;  // Used when auto_num_blocks=false
    int chunk_size = 1;
    bool per_task_stats = true;
    int max_iterations_per_probe = 1000;

    // Batch-3A configuration
    bool sparse_queue = true;
    bool multi_block = true;
    int worlds_per_block = 4;  // G: worlds per block
    int max_worlds = 32;       // Maximum concurrent worlds

    // Common configuration
    FrontierStrategy frontier_strategy = FrontierStrategy::NEIGHBOR_ACTIVATION;
    bool precheck_enabled = true;

    // Adaptive fallback thresholds
    double min_aggregation_threshold = 3.0;  // avg_popcount threshold
    int max_bit_dom_int_size_for_batch3a = 4;  // fallback to Stage 2 if larger

    // Debug options
    bool verbose = false;
    bool verify_results = false;  // Compare with CPU baseline
};

// Global config instance (can be modified at runtime)
inline SACGPUConfig& GetGlobalConfig() {
    static SACGPUConfig config;
    return config;
}

// ============================================================================
// Helper functions for ablation study
// ============================================================================

inline const char* BatchModeToString(BatchMode mode) {
    switch (mode) {
        case BatchMode::BATCH1: return "Batch-1 (Serial)";
        case BatchMode::BATCH2_STAGE1: return "Batch-2 Stage 1 (Micro-Batch)";
        case BatchMode::BATCH2_STAGE2: return "Batch-2 Stage 2 (Persistent)";
        case BatchMode::BATCH3A: return "Batch-3A (Aggregation)";
        case BatchMode::AUTO: return "Auto";
        default: return "Unknown";
    }
}

inline const char* FrontierStrategyToString(FrontierStrategy strategy) {
    switch (strategy) {
        case FrontierStrategy::FULL_ACTIVATION: return "Full";
        case FrontierStrategy::NEIGHBOR_ACTIVATION: return "Neighbor";
        default: return "Unknown";
    }
}

// Print current configuration
inline void PrintConfig(const SACGPUConfig& config) {
    printf("=== SAC-GPU Configuration ===\n");
    printf("Batch Mode: %s\n", BatchModeToString(config.batch_mode));
    printf("Frontier Strategy: %s\n", FrontierStrategyToString(config.frontier_strategy));
    printf("Precheck: %s\n", config.precheck_enabled ? "ON" : "OFF");
    printf("\n--- Stage 2 Options ---\n");
    printf("Auto num_blocks: %s\n", config.auto_num_blocks ? "ON" : "OFF");
    printf("Chunk size: %d\n", config.chunk_size);
    printf("Per-task stats: %s\n", config.per_task_stats ? "ON" : "OFF");
    printf("\n--- Batch-3A Options ---\n");
    printf("Sparse queue: %s\n", config.sparse_queue ? "ON" : "OFF");
    printf("Multi-block: %s\n", config.multi_block ? "ON" : "OFF");
    printf("Worlds per block: %d\n", config.worlds_per_block);
    printf("============================\n");
}

}  // namespace sac_gpu
}  // namespace cpim
