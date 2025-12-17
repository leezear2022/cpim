#!/bin/bash
#
# CPIM 完整测试脚本 (TIER1 + TIER2)
# 用途：合并前或发版前的完整验证（30-60 分钟）
#
# 包含：
# - TIER1: 标准回归（39 个实例，60s 超时）
# - TIER2: 完整验证（79 个实例，300s 超时）
#

set -e  # 出错时退出

# 切换到项目根目录
cd "$(dirname "$0")/.."

# 配置
BUILD_DIR="${BUILD_DIR:-build}"
TIER1_TIMEOUT=60
TIER2_TIMEOUT=300

echo "==================================================================="
echo "CPIM 完整测试 (TIER1 + TIER2)"
echo "==================================================================="

# 检查编译
echo -e "\n检查编译状态..."
if [ ! -f "$BUILD_DIR/cpim_test_parser" ]; then
    echo "Error: cpim_test_parser 未找到，请先编译："
    echo "  cd build && cmake .. && make -j4"
    exit 1
fi
echo "✓ 编译状态检查通过"

# TIER1: 标准回归
echo -e "\n==================================================================="
echo "[1/2] TIER1 - 标准回归测试 (39 个实例，超时=${TIER1_TIMEOUT}s)"
echo "==================================================================="
python3 samples/batch_test_v2.py --tier=1 --timeout=$TIER1_TIMEOUT --stats

# TIER2: 完整验证
echo -e "\n==================================================================="
echo "[2/2] TIER2 - 完整验证测试 (79 个实例，超时=${TIER2_TIMEOUT}s)"
echo "==================================================================="
python3 samples/batch_test_v2.py --tier=2 --timeout=$TIER2_TIMEOUT --stats --export-csv=tier2_results.csv

# 完成
echo -e "\n==================================================================="
echo "✅ 完整测试完成！"
echo "==================================================================="
echo "结果已导出到: tier2_results.csv"
echo ""
