# SAC 快速测试指南

## 编译

```bash
cd /home/lee/Codes/cpim/build
cmake .. && make -j4
```

## 快速验证

### 1. 单实例测试

```bash
# 基础 AC3bit (默认)
./cpim_test_parser --bench_path=../tests/data/bench/queens-4_ext.xml

# 使用 MSAC (需要修改代码中的 ACAlgorithm 参数)
```

### 2. 批量测试

```bash
cd /home/lee/Codes/cpim

# TIER 0: 快速验证 (约 1 分钟)
python3 tests/python/batch_test_v2.py --tier=0

# TIER 1: 完整测试 (约 10 分钟)
python3 tests/python/batch_test_v2.py --tier=1
```

### 3. 验证工具

```bash
# 验证 GAC 传播正确性
./verify_gac --input=../tests/data/bench/queens-4_ext.xml

# 验证搜索过程正确性
./verify_search --input=../tests/data/bench/queens-4_ext.xml
```

## 调试输出

```bash
# 启用详细日志
GLOG_v=1 ./cpim_test_parser --bench_path=../tests/data/bench/test.xml

# 更详细
GLOG_v=2 ./cpim_test_parser --bench_path=../tests/data/bench/test.xml
```

## 测试实例位置

```
tests/data/bench/          # 小型测试实例
  ├── queens-4_ext.xml     # 4皇后
  ├── queens-12_ext.xml    # 12皇后
  ├── test.xml             # 最小测试
  └── ...

benchmarks/                # 大型实例 (需单独下载)
  ├── langford/
  ├── tightness0.1/
  ├── tightness0.5/
  └── ...
```

## 预期结果

| 实例 | 结果 | Positives | Negatives |
|------|------|-----------|-----------|
| queens-4 | SAT | 5 | 1 |
| queens-12 | SAT | 41 | 29 |
| test.xml | SAT | 3 | 0 |
| langford-3-9 | SAT | 468 | 441 |

## 常见问题

### CMake 报错 "generator mismatch"
```bash
cd build
rm -rf CMakeCache.txt CMakeFiles _deps
cmake ..
```

### glog 未找到
```bash
sudo apt install libgoogle-glog-dev
```

### 测试超时
```bash
# 增加超时时间
python3 tests/python/batch_test_v2.py --tier=0 --timeout=120
```
