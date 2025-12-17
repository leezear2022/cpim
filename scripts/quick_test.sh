#!/bin/bash
#
# CPIM 快速验证脚本 (TIER0)
# 用途：每次代码修改后快速验证基本功能（2-3 分钟）
#
# 包含：
# 1. GAC 正确性验证（verify_gac）
# 2. 搜索正确性验证（verify_search）
# 3. TIER0 批量对比测试（12 个实例）
#

set -e  # 出错时退出

# 切换到项目根目录
cd "$(dirname "$0")/.."

# 配置
BUILD_DIR="${BUILD_DIR:-build}"
TIMEOUT=10

echo "==================================================================="
echo "CPIM 快速验证测试 (TIER0)"
echo "==================================================================="

# 检查编译
echo -e "\n[0/3] 检查编译状态..."
if [ ! -f "$BUILD_DIR/cpim_test_parser" ]; then
    echo "Error: cpim_test_parser 未找到，请先编译："
    echo "  cd build && cmake .. && make -j4"
    exit 1
fi

if [ ! -f "$BUILD_DIR/verify_gac" ]; then
    echo "Warning: verify_gac 未找到，跳过 GAC 验证"
    SKIP_GAC=1
fi

if [ ! -f "$BUILD_DIR/verify_search" ]; then
    echo "Warning: verify_search 未找到，跳过搜索验证"
    SKIP_SEARCH=1
fi

echo "✓ 编译状态检查通过"

# 1. GAC 正确性验证
if [ -z "$SKIP_GAC" ]; then
    echo -e "\n[1/3] GAC 正确性验证 (verify_gac)..."
    echo "  测试 1: samples/bench/test.xml"
    $BUILD_DIR/verify_gac --input=samples/bench/test.xml > /dev/null 2>&1 && echo "    ✓ 通过" || echo "    ✗ 失败"

    echo "  测试 2: benchmarks/langford/langford-2-4-ext.xml"
    $BUILD_DIR/verify_gac --input=benchmarks/langford/langford-2-4-ext.xml > /dev/null 2>&1 && echo "    ✓ 通过" || echo "    ✗ 失败"
else
    echo -e "\n[1/3] GAC 正确性验证 - 跳过"
fi

# 2. 搜索正确性验证
if [ -z "$SKIP_SEARCH" ]; then
    echo -e "\n[2/3] 搜索正确性验证 (verify_search)..."
    echo "  测试 1: samples/bench/test.xml"
    timeout 5s $BUILD_DIR/verify_search --input=samples/bench/test.xml --timeout=5 > /dev/null 2>&1 && echo "    ✓ 通过" || echo "    ✗ 失败"

    echo "  测试 2: samples/bench/queens-4_ext.xml"
    timeout 5s $BUILD_DIR/verify_search --input=samples/bench/queens-4_ext.xml --timeout=5 > /dev/null 2>&1 && echo "    ✓ 通过" || echo "    ✗ 失败"
else
    echo -e "\n[2/3] 搜索正确性验证 - 跳过"
fi

# 3. TIER0 批量对比测试
echo -e "\n[3/3] TIER0 批量对比测试 (12 个实例，超时=${TIMEOUT}s)..."
python3 samples/batch_test_v2.py --tier=0 --timeout=$TIMEOUT

# 完成
echo -e "\n==================================================================="
echo "✅ 快速验证测试完成！"
echo "==================================================================="
