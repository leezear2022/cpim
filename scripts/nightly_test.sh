#!/bin/bash
#
# CPIM 夜间测试脚本 (可选)
# 用途：压力测试，发现潜在问题（可根据需要运行）
#
# 注意：由于 TIER3 有 1000+ 实例，建议按需使用，或仅运行 TIER2
#

set -e  # 出错时退出

# 切换到项目根目录
cd "$(dirname "$0")/.."

# 配置
BUILD_DIR="${BUILD_DIR:-build}"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
LOG_FILE="nightly_test_${TIMESTAMP}.log"
CSV_FILE="nightly_${TIMESTAMP}.csv"

# 默认只运行 TIER2（可通过环境变量 NIGHTLY_TIER 修改）
NIGHTLY_TIER="${NIGHTLY_TIER:-2}"
TIMEOUT=300

echo "==================================================================="
echo "CPIM 夜间测试 (TIER${NIGHTLY_TIER})"
echo "==================================================================="
echo "时间戳: $TIMESTAMP"
echo "日志文件: $LOG_FILE"
echo "结果文件: $CSV_FILE"
echo ""

# 检查编译
if [ ! -f "$BUILD_DIR/cpim_test_parser" ]; then
    echo "Error: cpim_test_parser 未找到，请先编译："
    echo "  cd build && cmake .. && make -j4"
    exit 1
fi

# 运行测试
echo "开始测试..." | tee $LOG_FILE

python3 samples/batch_test_v2.py \
    --tier=$NIGHTLY_TIER \
    --timeout=$TIMEOUT \
    --stats \
    --export-csv=$CSV_FILE \
    | tee -a $LOG_FILE

echo -e "\n==================================================================="
echo "✅ 夜间测试完成！"
echo "==================================================================="
echo "日志: $LOG_FILE"
echo "结果: $CSV_FILE"
echo ""

# 提示：如何运行 TIER3（如果需要）
if [ "$NIGHTLY_TIER" != "3" ]; then
    echo "提示：如需运行 TIER3 全量测试（1000+ 实例），使用："
    echo "  NIGHTLY_TIER=3 ./scripts/nightly_test.sh"
    echo ""
fi
