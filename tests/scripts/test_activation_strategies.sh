#!/bin/bash
# 测试 FULL vs NEIGHBOR activation 策略一致性
# TIER0 测试集

BUILD_DIR="/home/lee/Codes/cpim/build"
TEST_EXEC="$BUILD_DIR/test_batch_probe_state"

TIER0_INSTANCES=(
    "/home/lee/Codes/cpim/tests/data/bench/test.xml"
    "/home/lee/Codes/cpim/tests/data/bench/queens-4_ext.xml"
    "/home/lee/Codes/cpim/benchmarks/langford/langford-2-4-ext.xml"
    "/home/lee/Codes/cpim/benchmarks/langford/langford-3-9-ext.xml"
    "/home/lee/Codes/cpim/benchmarks/tightness0.1/rand-2-40-8-753-100-0_ext.xml"
    "/home/lee/Codes/cpim/benchmarks/tightness0.1/rand-2-40-8-753-100-5_ext.xml"
    "/home/lee/Codes/cpim/benchmarks/tightness0.5/rand-2-40-25-180-500-0_ext.xml"
    "/home/lee/Codes/cpim/benchmarks/tightness0.8/rand-2-40-80-103-800-0_ext.xml"
    "/home/lee/Codes/cpim/benchmarks/driver/driverlogw-01c-sat_ext.xml"
    "/home/lee/Codes/cpim/benchmarks/BH-4-4/BlackHole-4-4-e-0_ext.xml"
    "/home/lee/Codes/cpim/benchmarks/composed-25-1-2/composed-25-1-2-0_ext.xml"
    "/home/lee/Codes/cpim/benchmarks/graphs/graphw-05_ext.xml"
)

echo "========================================================================"
echo "FULL vs NEIGHBOR Activation 一致性测试 (TIER0)"
echo "========================================================================"
echo ""

PASSED=0
FAILED=0
TOTAL=${#TIER0_INSTANCES[@]}

for instance in "${TIER0_INSTANCES[@]}"; do
    name=$(basename "$instance")
    printf "[%2d/%2d] %-50s " $((PASSED + FAILED + 1)) "$TOTAL" "$name"

    # 运行测试，捕获返回码
    if "$TEST_EXEC" --input="$instance" > /tmp/test_output_$$.log 2>&1; then
        # 检查输出中是否包含成功标记
        if grep -q "✓ ALL TESTS PASSED" /tmp/test_output_$$.log || \
           grep -q "Test skipped (UNSAT instance)" /tmp/test_output_$$.log; then
            echo "✅ PASS"
            ((PASSED++))
        else
            echo "❌ FAIL (no success marker)"
            ((FAILED++))
            # 打印最后 10 行输出
            echo "  Last 10 lines of output:"
            tail -10 /tmp/test_output_$$.log | sed 's/^/    /'
        fi
    else
        echo "❌ ERROR (exit code: $?)"
        ((FAILED++))
        # 打印最后 10 行输出
        echo "  Last 10 lines of output:"
        tail -10 /tmp/test_output_$$.log | sed 's/^/    /'
    fi

    # 清理临时文件
    rm -f /tmp/test_output_$$.log
done

echo ""
echo "========================================================================"
echo "总结: $PASSED 通过, $FAILED 失败 / 总共 $TOTAL 个实例"
echo "========================================================================"

if [ $FAILED -gt 0 ]; then
    echo ""
    echo "❌ 存在失败实例！"
    exit 1
else
    echo ""
    echo "✅ 所有测试通过！"
    exit 0
fi
