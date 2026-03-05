#!/bin/bash
# ============================================================================
# SAC-GPU Ablation Study Script
# ============================================================================
# This script runs performance comparisons across different SAC-GPU versions
# for ablation experiments.
#
# Usage:
#   ./ablation_study.sh [--tier=0|1|2] [--output=results.csv]
#
# Requirements:
#   - Build all test binaries first: cd build && make -j4
#   - Run from project root directory
# ============================================================================

set -e

# Default parameters
TIER=0
OUTPUT_FILE="ablation_results_$(date +%Y%m%d_%H%M%S).csv"
BUILD_DIR="build"
VERBOSE=0

# Parse arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --tier=*)
            TIER="${1#*=}"
            shift
            ;;
        --output=*)
            OUTPUT_FILE="${1#*=}"
            shift
            ;;
        --verbose)
            VERBOSE=1
            shift
            ;;
        --help)
            echo "Usage: $0 [--tier=0|1|2] [--output=results.csv] [--verbose]"
            echo ""
            echo "Options:"
            echo "  --tier=N     Test tier (0=quick, 1=medium, 2=full)"
            echo "  --output=F   Output CSV file"
            echo "  --verbose    Show detailed output"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

# Test instances by tier
TIER0_INSTANCES=(
    "tests/data/bench/queens-4_ext.xml"
    "tests/data/bench/queens-12_ext.xml"
    "tests/data/bench/test.xml"
)

TIER1_INSTANCES=(
    "${TIER0_INSTANCES[@]}"
    "tests/data/bench/haystacks-11_ext.xml"
    "benchmarks/composed-25-10-20/composed-25-10-20-0_ext.xml"
)

TIER2_INSTANCES=(
    "${TIER1_INSTANCES[@]}"
    "benchmarks/tightness0.9/rand-2-40-180-84-900-0_ext.xml"
    "benchmarks/tightness0.9/rand-2-40-180-84-900-1_ext.xml"
    "benchmarks/tightness0.9/rand-2-40-180-84-900-2_ext.xml"
)

# Probe counts by tier
TIER0_PROBES=(32)
TIER1_PROBES=(32 64 100)
TIER2_PROBES=(32 64 100 200)

# Select instances and probes based on tier
case $TIER in
    0)
        INSTANCES=("${TIER0_INSTANCES[@]}")
        PROBES=("${TIER0_PROBES[@]}")
        ;;
    1)
        INSTANCES=("${TIER1_INSTANCES[@]}")
        PROBES=("${TIER1_PROBES[@]}")
        ;;
    2)
        INSTANCES=("${TIER2_INSTANCES[@]}")
        PROBES=("${TIER2_PROBES[@]}")
        ;;
    *)
        echo "Invalid tier: $TIER (must be 0, 1, or 2)"
        exit 1
        ;;
esac

# Check build directory
if [ ! -d "$BUILD_DIR" ]; then
    echo "Error: Build directory not found. Run 'mkdir -p build && cd build && cmake .. && make -j4' first."
    exit 1
fi

# Check required binaries
BINARIES=(
    "test_batch_probe_state"   # Batch-1
    "test_stage2_persistent"   # Stage 2
    "test_batch3a"             # Batch-3A
)

for bin in "${BINARIES[@]}"; do
    if [ ! -f "$BUILD_DIR/$bin" ]; then
        echo "Warning: $bin not found, skipping..."
    fi
done

# Initialize output file
echo "# SAC-GPU Ablation Study Results" > "$OUTPUT_FILE"
echo "# Date: $(date)" >> "$OUTPUT_FILE"
echo "# Tier: $TIER" >> "$OUTPUT_FILE"
echo "" >> "$OUTPUT_FILE"
echo "instance,num_probes,version,time_ms,failures,probes_per_sec" >> "$OUTPUT_FILE"

# Helper function to extract time from output
extract_time() {
    local output="$1"
    # Try to extract time in ms from various output formats
    echo "$output" | grep -oP '(\d+\.?\d*)\s*ms' | head -1 | grep -oP '\d+\.?\d*' || echo "N/A"
}

extract_failures() {
    local output="$1"
    echo "$output" | grep -oP '(\d+)\s*/\s*\d+\s*probes?\s*failed' | head -1 | grep -oP '^\d+' || echo "0"
}

# Run tests
echo "=============================================="
echo "SAC-GPU Ablation Study"
echo "Tier: $TIER"
echo "Output: $OUTPUT_FILE"
echo "=============================================="
echo ""

total_tests=0
passed_tests=0

for inst in "${INSTANCES[@]}"; do
    # Skip if instance doesn't exist
    if [ ! -f "$inst" ]; then
        echo "Skipping (not found): $inst"
        continue
    fi

    inst_name=$(basename "$inst" .xml)
    echo "=== Instance: $inst_name ==="

    for np in "${PROBES[@]}"; do
        echo "  Probes: $np"

        # Batch-1 (if available)
        if [ -f "$BUILD_DIR/test_batch_probe_state" ]; then
            output=$("$BUILD_DIR/test_batch_probe_state" --input="$inst" --num_probes=$np 2>&1 || true)
            time_ms=$(extract_time "$output")
            failures=$(extract_failures "$output")
            if [ "$time_ms" != "N/A" ]; then
                probes_per_sec=$(echo "scale=2; $np * 1000 / $time_ms" | bc 2>/dev/null || echo "N/A")
            else
                probes_per_sec="N/A"
            fi
            echo "    Batch-1: ${time_ms}ms, ${failures} failures"
            echo "$inst_name,$np,Batch-1,$time_ms,$failures,$probes_per_sec" >> "$OUTPUT_FILE"
            ((total_tests++))
            if [ "$failures" = "0" ]; then ((passed_tests++)); fi
        fi

        # Stage 2
        if [ -f "$BUILD_DIR/test_stage2_persistent" ]; then
            output=$("$BUILD_DIR/test_stage2_persistent" --input="$inst" --num_probes=$np 2>&1 || true)
            time_ms=$(extract_time "$output")
            failures=$(extract_failures "$output")
            if [ "$time_ms" != "N/A" ]; then
                probes_per_sec=$(echo "scale=2; $np * 1000 / $time_ms" | bc 2>/dev/null || echo "N/A")
            else
                probes_per_sec="N/A"
            fi
            echo "    Stage-2: ${time_ms}ms, ${failures} failures"
            echo "$inst_name,$np,Stage-2,$time_ms,$failures,$probes_per_sec" >> "$OUTPUT_FILE"
            ((total_tests++))
            if [ "$failures" = "0" ]; then ((passed_tests++)); fi
        fi

        # Batch-3A
        if [ -f "$BUILD_DIR/test_batch3a" ]; then
            output=$("$BUILD_DIR/test_batch3a" --input="$inst" --num_probes=$np 2>&1 || true)
            time_ms=$(extract_time "$output")
            failures=$(extract_failures "$output")
            if [ "$time_ms" != "N/A" ]; then
                probes_per_sec=$(echo "scale=2; $np * 1000 / $time_ms" | bc 2>/dev/null || echo "N/A")
            else
                probes_per_sec="N/A"
            fi
            echo "    Batch-3A: ${time_ms}ms, ${failures} failures"
            echo "$inst_name,$np,Batch-3A,$time_ms,$failures,$probes_per_sec" >> "$OUTPUT_FILE"
            ((total_tests++))
            if [ "$failures" = "0" ]; then ((passed_tests++)); fi
        fi

        echo ""
    done
done

echo "=============================================="
echo "Ablation Study Complete"
echo "Total tests: $total_tests"
echo "Passed: $passed_tests"
echo "Results saved to: $OUTPUT_FILE"
echo "=============================================="

# Generate summary
echo "" >> "$OUTPUT_FILE"
echo "# Summary" >> "$OUTPUT_FILE"
echo "# Total tests: $total_tests" >> "$OUTPUT_FILE"
echo "# Passed: $passed_tests" >> "$OUTPUT_FILE"
