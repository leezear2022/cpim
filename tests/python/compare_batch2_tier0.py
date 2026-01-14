#!/usr/bin/env python3
"""
TIER0 批量回归：Batch-2 (Micro-Batch) vs Batch-1 结果一致性

说明：
- 依赖 build/test_batch2_probe
- 对每个实例执行：
  1) 初始 GAC 传播（构造 AC snapshot）
  2) Batch-1（cooperative）与 Batch-2（micro-batch）在 FULL/NEIGHBOR 两种策略下对照
  3) 验证失败 probe 集合一致、且 Batch-2 不污染 GModel 状态
"""

import argparse
import subprocess
import sys
from pathlib import Path

from tier_definitions import TIER0, TIER1

PROJECT_ROOT = Path(__file__).parent.parent.parent
BUILD_DIR = PROJECT_ROOT / "build"


def _run_test(binary: Path, instance_path: str, timeout_s: int) -> dict:
    cmd = [str(binary), f"--input={instance_path}"]

    try:
        result = subprocess.run(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=timeout_s,
        )
        output = result.stdout if result.stdout else ""

        # test_batch2_probe：返回码 0 表示 PASS（或 UNSAT skip）
        if result.returncode == 0:
            is_unsat = "Test skipped (UNSAT instance)" in output
            return {"status": "PASS (UNSAT skipped)" if is_unsat else "PASS",
                    "output": output}

        return {"status": "FAIL", "output": output}
    except subprocess.TimeoutExpired:
        return {"status": "TIMEOUT", "output": ""}
    except Exception as e:
        return {"status": "ERROR", "output": str(e)}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tier", choices=["0", "1"], default="0",
                        help="选择测试集：0=TIER0, 1=TIER1")
    parser.add_argument("--timeout_s", type=int, default=180,
                        help="单实例超时（秒）")
    args = parser.parse_args()

    test_binary = BUILD_DIR / "test_batch2_probe"
    if not test_binary.exists():
        print(f"❌ 缺少可执行文件: {test_binary}")
        print("   先运行: cmake --build build -j$(nproc)")
        return 2

    tier = TIER0 if args.tier == "0" else TIER1

    print("=" * 80)
    print(f"Batch-2 (Micro-Batch) vs Batch-1 一致性测试 - TIER{args.tier}")
    print("=" * 80)
    print()

    passed = 0
    failed = 0

    for i, instance in enumerate(tier, 1):
        name = Path(instance).name
        print(f"[{i:2}/{len(tier)}] {name:50}", end=" ", flush=True)

        r = _run_test(test_binary, instance, args.timeout_s)
        status = r["status"]

        if status.startswith("PASS"):
            suffix = " (UNSAT)" if "UNSAT" in status else ""
            print(f"✅ PASS{suffix}")
            passed += 1
            continue

        if status == "TIMEOUT":
            print("⏱️  TIMEOUT")
        elif status == "ERROR":
            print("❌ ERROR")
        else:
            print("❌ FAIL")

        # 打印失败输出的关键行（避免刷屏）
        output = r["output"] or ""
        for line in output.splitlines():
            if ("Mismatch" in line or "FAIL" in line or
                    "Batch-2 Micro-Batch state consistency: FAIL" in line):
                print(f"    {line}")
        failed += 1

    print()
    print("=" * 80)
    print(f"总结: {passed} 通过, {failed} 失败 / 总共 {len(tier)} 个实例")
    print("=" * 80)

    if failed > 0:
        print()
        print("❌ 存在失败实例，需要调查！")
        return 1

    print()
    print("✅ 所有测试通过！Batch-2 Micro-Batch 与 Batch-1 结果完全一致。")
    return 0


if __name__ == "__main__":
    sys.exit(main())

