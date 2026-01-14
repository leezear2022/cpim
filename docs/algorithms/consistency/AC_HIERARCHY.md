# AC 算法层次结构

---
status: active
---

本文档描述 CPIM 中实现的弧一致性 (Arc Consistency) 算法层次。

## 算法概览

```
AC (弧一致性)
├── AC3bit      - 基础 AC，位图优化
├── RPC3        - 路径逆一致性
└── lMaxRPC     - 轻量级最大限制路径一致性
```

## AC3bit

**基础弧一致性算法**

- 文件：[src/solver/cpu/AC3bit.cpp](../../../src/solver/cpu/AC3bit.cpp)
- 类：`cpim::AC3bit`

### 核心思想

对于每个弧 (Xi, Xj)，确保 Xi 域中的每个值在 Xj 中都有支持。

### 关键优化

1. **位图支持**：使用 64 位整数表示域，位操作加速
2. **增量传播**：仅传播受影响的弧
3. **支持计数**：跳过已有足够支持的值

### 时间复杂度

- 最坏情况：O(ed³)，其中 e = 约束数，d = 最大域大小

## RPC3

**路径逆一致性 (Restricted Path Consistency)**

- 文件：[src/solver/cpu/RPC3.cpp](../../../src/solver/cpu/RPC3.cpp)
- 类：`cpim::RPC3`

### 核心思想

在 AC 基础上，检查路径一致性：如果值 a ∈ Di 在 Dj 中只有一个支持 b，则检查 b 在其他变量上是否有支持。

### 剪枝能力

RPC3 > AC3（更强的剪枝，但开销更大）

## lMaxRPC

**轻量级最大限制路径一致性**

- 文件：[src/solver/cpu/lMaxRPC.cpp](../../../src/solver/cpu/lMaxRPC.cpp)
- 类：`cpim::lMaxRPC`

### 核心思想

MaxRPC 的轻量级版本，在路径一致性检查中只考虑一跳邻居。

### 权衡

- 剪枝能力：lMaxRPC > RPC3 > AC3
- 计算开销：lMaxRPC > RPC3 > AC3

## 算法选择

### 使用场景

| 算法 | 适用场景 |
|------|----------|
| AC3bit | 通用问题，默认选择 |
| RPC3 | 约束紧密问题 |
| lMaxRPC | 需要强剪枝的困难问题 |

### 命令行选择

```bash
./cpim_test_parser --bench_path=<file> --ac_algo=AC3bit
./cpim_test_parser --bench_path=<file> --ac_algo=RPC3
./cpim_test_parser --bench_path=<file> --ac_algo=lMaxRPC
```

## 代码结构

所有 AC 算法继承自 `cpim::Solver` 基类：

```cpp
class AC3bit : public Solver {
public:
    bool EnforceAC();           // 执行 AC 传播
    int Propagate(int var_id);  // 单变量传播
};
```

## 相关文档

- [SAC 算法](SAC_ALGORITHMS.md) - 单例弧一致性
- [MAC 搜索](../search/MAC_SEARCH.md) - 基于 AC 的搜索
