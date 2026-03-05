#!/usr/bin/env python3
"""
对比 FULL_ACTIVATION 和 NEIGHBOR_ACTIVATION 的结果一致性
使用 TIER0 测试集（12 个实例）

验证：
1. 两种策略的状态一致性（域恢复正确）
2. 两种策略产生相同的失败探测集合（结果一致性）
"""

import subprocess
import sys
from pathlib import Path
from tier_definitions import TIER0

PROJECT_ROOT = Path(__file__).parent.parent.parent
BUILD_DIR = PROJECT_ROOT / "build"


def run_consistency_test(instance_path: str) -> dict:
    """
    运行 test_batch_probe_state 对比两种策略

    返回：
    {
        'full_ok': bool,      # FULL_ACTIVATION 状态一致性
        'neighbor_ok': bool,  # NEIGHBOR_ACTIVATION 状态一致性
        'results_match': bool, # 结果一致性
        'status': 'PASS' | 'FAIL' | 'ERROR'
    }
    """
    cmd = [
        str(BUILD_DIR / "test_batch_probe_state"),
        f"--input={instance_path}"
    ]

    try:
        # 使用 PIPE 并立即读取以避免缓冲区问题
        result = subprocess.run(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=60
        )

        output = result.stdout if result.stdout else ""

        # 简化：只检查返回码和关键输出
        all_pass = "✓ ALL TESTS PASSED" in output
        is_unsat = "Test skipped (UNSAT instance)" in output

        # 成功条件：返回码 0 且（全部通过 或 UNSAT跳过）
        if result.returncode == 0 and (all_pass or is_unsat):
            status_suffix = " (UNSAT skipped)" if is_unsat else ""
            return {
                'full_ok': True,
                'neighbor_ok': True,
                'results_match': True,
                'status': f'PASS{status_suffix}',
                'output': output
            }

        # 失败条件
        return {
            'full_ok': False,
            'neighbor_ok': False,
            'results_match': False,
            'status': 'FAIL' if result.returncode == 0 else 'ERROR',
            'output': output
        }
    except subprocess.TimeoutExpired:
        return {
            'full_ok': False,
            'neighbor_ok': False,
            'results_match': False,
            'status': 'TIMEOUT',
            'output': ''
        }
    except Exception as e:
        return {
            'full_ok': False,
            'neighbor_ok': False,
            'results_match': False,
            'status': 'ERROR',
            'output': str(e)
        }


def main():
    print("=" * 80)
    print("FULL vs NEIGHBOR Activation 一致性测试")
    print("=" * 80)
    print()

    passed = 0
    failed = 0

    for i, instance in enumerate(TIER0, 1):
        name = Path(instance).name
        print(f"[{i:2}/{len(TIER0)}] {name:50}", end=" ", flush=True)

        result = run_consistency_test(instance)

        if result['status'] == 'PASS' or result['status'] == 'PASS (UNSAT skipped)':
            status_suffix = " (UNSAT)" if "UNSAT" in result['status'] else ""
            print(f"✅ PASS{status_suffix}")
            passed += 1
        elif result['status'] == 'TIMEOUT':
            print("⏱️  TIMEOUT")
            failed += 1
        elif result['status'] == 'ERROR':
            print("❌ ERROR")
            print(f"    Error: {result['output'][:200]}")
            failed += 1
        else:
            print("❌ FAIL")
            print(f"    FULL_ok={result['full_ok']}, "
                  f"NEIGHBOR_ok={result['neighbor_ok']}, "
                  f"Match={result['results_match']}")

            # 打印失败输出的关键部分
            if not result['results_match']:
                for line in result['output'].split('\n'):
                    if 'Mismatch' in line or 'Different number' in line or 'FAIL' in line:
                        print(f"    {line}")
            failed += 1

    # 总结
    print()
    print("=" * 80)
    print(f"总结: {passed} 通过, {failed} 失败 / 总共 {len(TIER0)} 个实例")
    print("=" * 80)

    if failed > 0:
        print()
        print("❌ 存在失败实例，需要调查！")
        sys.exit(1)
    else:
        print()
        print("✅ 所有测试通过！FULL 和 NEIGHBOR 策略产生完全一致的结果。")
        sys.exit(0)


if __name__ == "__main__":
    main()
