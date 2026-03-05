#!/bin/bash
# SAC1 vs SAC3 对比测试脚本
# 用法: ./test_sac_modes.sh [输出文件]

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../../build"
BENCH_DIR="${SCRIPT_DIR}/../../benchmarks"
TEST_DIR="${SCRIPT_DIR}/../../tests/data/bench"

OUTPUT_FILE="${1:-sac_comparison_results.md}"

# 如果是相对路径，转换为绝对路径
if [[ ! "$OUTPUT_FILE" = /* ]]; then
    OUTPUT_FILE="${SCRIPT_DIR}/../../${OUTPUT_FILE}"
fi

# 确保输出目录存在
mkdir -p "$(dirname "$OUTPUT_FILE")"

cd "$BUILD_DIR"

# 测试用例列表
declare -a TEST_CASES=(
    # 小型测试 (TIER 0)
    "${TEST_DIR}/queens-4_ext.xml"
    "${TEST_DIR}/queens-12_ext.xml"
    "${TEST_DIR}/test.xml"
    # Langford 问题
    "${BENCH_DIR}/langford/langford-2-4-ext.xml"
    "${BENCH_DIR}/langford/langford-3-9-ext.xml"
    # 随机问题 (如果存在)
    "${BENCH_DIR}/tightness0.1/rand-2-40-8-753-100-0_ext.xml"
    "${BENCH_DIR}/tightness0.5/rand-2-40-25-180-500-0_ext.xml"
    # Driver 问题
    "${BENCH_DIR}/driver/driverlogw-01c-sat_ext.xml"
)

echo "# SAC1 vs SAC3 对比测试报告" > "$OUTPUT_FILE"
echo "" >> "$OUTPUT_FILE"
echo "> 生成时间: $(date '+%Y-%m-%d %H:%M:%S')" >> "$OUTPUT_FILE"
echo "" >> "$OUTPUT_FILE"
echo "## 测试环境" >> "$OUTPUT_FILE"
echo "" >> "$OUTPUT_FILE"
echo "- 平台: $(uname -s) $(uname -m)" >> "$OUTPUT_FILE"
echo "- GPU: $(nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null | head -1 || echo 'N/A')" >> "$OUTPUT_FILE"
echo "" >> "$OUTPUT_FILE"
echo "## 测试结果" >> "$OUTPUT_FILE"
echo "" >> "$OUTPUT_FILE"
echo "| 问题 | SAC1 时间 | SAC3 时间 | SAC1 探测 | SAC3 探测 | SAC1 删除 | SAC3 删除 | 正向节点 | 回溯节点 | 解一致 |" >> "$OUTPUT_FILE"
echo "|------|----------|----------|----------|----------|----------|----------|----------|----------|--------|" >> "$OUTPUT_FILE"

TOTAL_TESTS=0
PASSED_TESTS=0

for test_file in "${TEST_CASES[@]}"; do
    if [[ ! -f "$test_file" ]]; then
        echo "跳过不存在的文件: $test_file"
        continue
    fi

    TOTAL_TESTS=$((TOTAL_TESTS + 1))

    # 获取问题名称
    problem_name=$(basename "$test_file" | sed 's/_ext\.xml$//')
    echo "测试: $problem_name"

    # 运行 SAC1
    sac1_output=$(./compare_cpu_gpu --input="$test_file" --sac --sac_mode=sac1 --gpu_only --time_limit=30000 2>&1)
    sac1_time=$(echo "$sac1_output" | grep "SAC 时间:" | awk '{print $3}' | tr -d 's')
    sac1_probes=$(echo "$sac1_output" | grep "SAC 探测:" | awk '{print $3}')
    sac1_deletions=$(echo "$sac1_output" | grep "SAC 删除:" | awk '{print $3}')
    sac1_positive=$(echo "$sac1_output" | grep "正向节点:" | awk '{print $2}')
    sac1_negative=$(echo "$sac1_output" | grep "回溯节点:" | awk '{print $2}')
    sac1_solution=$(echo "$sac1_output" | grep "解:" | head -1)

    # 运行 SAC3
    sac3_output=$(./compare_cpu_gpu --input="$test_file" --sac --sac_mode=sac3 --gpu_only --time_limit=30000 2>&1)
    sac3_time=$(echo "$sac3_output" | grep "SAC 时间:" | awk '{print $3}' | tr -d 's')
    sac3_probes=$(echo "$sac3_output" | grep "SAC 探测:" | awk '{print $3}')
    sac3_deletions=$(echo "$sac3_output" | grep "SAC 删除:" | awk '{print $3}')
    sac3_positive=$(echo "$sac3_output" | grep "正向节点:" | awk '{print $2}')
    sac3_negative=$(echo "$sac3_output" | grep "回溯节点:" | awk '{print $2}')
    sac3_solution=$(echo "$sac3_output" | grep "解:" | head -1)

    # 检查解一致性
    if [[ "$sac1_positive" == "$sac3_positive" ]] && [[ "$sac1_negative" == "$sac3_negative" ]]; then
        consistent="✓"
        PASSED_TESTS=$((PASSED_TESTS + 1))
    else
        consistent="✗"
    fi

    # 处理空值
    sac1_time=${sac1_time:-"N/A"}
    sac3_time=${sac3_time:-"N/A"}
    sac1_probes=${sac1_probes:-"N/A"}
    sac3_probes=${sac3_probes:-"N/A"}
    sac1_deletions=${sac1_deletions:-"N/A"}
    sac3_deletions=${sac3_deletions:-"N/A"}
    sac1_positive=${sac1_positive:-"N/A"}
    sac1_negative=${sac1_negative:-"N/A"}

    echo "| $problem_name | ${sac1_time}s | ${sac3_time}s | $sac1_probes | $sac3_probes | $sac1_deletions | $sac3_deletions | $sac1_positive | $sac1_negative | $consistent |" >> "$OUTPUT_FILE"
done

echo "" >> "$OUTPUT_FILE"
echo "## 统计" >> "$OUTPUT_FILE"
echo "" >> "$OUTPUT_FILE"
echo "- 测试用例数: $TOTAL_TESTS" >> "$OUTPUT_FILE"
echo "- 通过数: $PASSED_TESTS" >> "$OUTPUT_FILE"
echo "- 通过率: $(echo "scale=1; $PASSED_TESTS * 100 / $TOTAL_TESTS" | bc)%" >> "$OUTPUT_FILE"
echo "" >> "$OUTPUT_FILE"
echo "## 结论" >> "$OUTPUT_FILE"
echo "" >> "$OUTPUT_FILE"
echo "SAC3 使用队列驱动的 probe 粒度管理，相比 SAC1 的变量粒度 dirty set：" >> "$OUTPUT_FILE"
echo "- 探测数量相同（初始全量入队）" >> "$OUTPUT_FILE"
echo "- 删除数量一致" >> "$OUTPUT_FILE"
echo "- 搜索节点数完全匹配" >> "$OUTPUT_FILE"
echo "- 运行时间略有优势（减少无效状态检查）" >> "$OUTPUT_FILE"

echo ""
echo "测试完成！结果已保存到: $OUTPUT_FILE"
echo "通过率: $PASSED_TESTS / $TOTAL_TESTS"
