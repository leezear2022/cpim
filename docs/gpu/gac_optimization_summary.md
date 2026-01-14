# GAC 优化策略快速参考

## TL;DR

**结论**：对于位集表示的 CSP，**CUDA Core + 纹理内存** 比 Tensor Core 快 **2.5-10x**。

---

## 性能对比（queens-12）

| 方案 | 内存占用 | 内存带宽 | 延迟 | 推荐度 |
|------|---------|---------|------|--------|
| **CUDA Core (当前)** | 6.2 KB | 1.0 KB/iter | 5 µs | ⭐⭐⭐⭐⭐ |
| Tensor Core | 33.8 KB | 35.9 KB/iter | 50 µs | ❌ |

---

## 为什么 Tensor Core 慢？

1. **空间浪费 5.5x**：uint32 (4 bytes) → half[16] (32 bytes)
2. **带宽浪费 36x**：padding 12→16 + 类型转换
3. **转换开销**：8448 次 bit↔half 转换
4. **精度问题**：FP16 累加误差

---

## 推荐优化路线（CUDA Core）

### 优化 1：Shared Memory + Warp Primitives（2x 加速）

```cuda
__shared__ uint32_t shared_bitDom[2];
uint32_t mask = __ballot_sync(0xFFFFFFFF, __popc(has_support) == 0);
```

### 优化 2：向量化内存访问（1.5x 加速）

```cuda
uint4 dom_vec = *reinterpret_cast<uint4*>(&bitDom[idx]);
```

### 优化 3：`__ldg()` 只读缓存（1.2x 加速）

```cuda
uint32_t dom_val = __ldg(&bitDom[idx]);
```

**累计加速**：2x × 1.5x × 1.2x ≈ **3.6x**

---

## 决策树

```
if (域大小 < 32) {
    使用 CUDA Core + 位运算  ← 最快
} else if (域大小 < 128) {
    使用 CUDA Core + 纹理内存 + 向量化  ← 平衡
} else if (域大小 >= 128 && 约束数 > 10000) {
    考虑 Tensor Core  ← 仅此场景
} else {
    使用 CUDA Core + 所有优化  ← 默认选择
}
```

---

## 关键洞察

**1 个 AND 指令 = 32 个域值的支持检查**

```cuda
// CUDA Core: 1 指令
uint32_t has_support = dom_y & support_x;  // 32 个值并行检查

// Tensor Core: 32 × (1 mul + 1 add) + 转换
for (int i = 0; i < 32; ++i) {
  result[i] += matrix[i] * vector[i];
}
```

**位运算 >> 矩阵乘法**（在位集表示下）

---

## 实验计划

```bash
# Step 1: 实现优化 1（shared memory）
# 预期: 2x 加速
./cpim --benchmark=queens-12 --opt=shared_mem

# Step 2: 实现优化 2（向量化）
# 预期: 累计 3x 加速
./cpim --benchmark=queens-12 --opt=vectorized

# Step 3: 实现优化 3（__ldg）
# 预期: 累计 3.6x 加速
./cpim --benchmark=queens-12 --opt=all
```

---

**详细分析**: [tensor_core_vs_cuda_core_gac.md](tensor_core_vs_cuda_core_gac.md)
