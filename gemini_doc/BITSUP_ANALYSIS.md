# BitSup 实现与用法分析

本文档分析了 `bitSup` (Bitwise Support) 在 CPIM 项目中的实现细节和 GPU 使用方式。

## 1. 代码位置

*   **构建逻辑 (Host)**: `src/model/gmodel_adapter.cu`
    *   函数: `GModelAdapter::BuildBitSup`
    *   作用: 解析约束及其扩展元组，将其转换为位图格式。
*   **内核使用 (Device)**: `src/solver/gpu/GModel.cu`
    *   内核: `CsCheckMainKernel`
    *   作用: 执行 GAC 检查，使用 `bitSup` 进行位掩码操作。
*   **结构定义**: `src/solver/gpu/GModel.cuh` (推测，或 `GModel` 类定义中)

## 2. 数据结构

`bitSup` 是一个扁平化的 `uint2` 数组，用于存储约束的支持位图。

*   **类型**: `uint2*` (在 GPU 上通常使用 Global Memory, 虽然也绑定了 Texture `texObj_BitSup`)
*   **大小**: `num_constraints * bitsup_per_constraint`
    *   `bitsup_per_constraint = 2 * max_dom_size * bit_dom_int_size`
*   **物理含义**:
    *   它是一个三维结构被压扁了：`[Constraint][Variable_Value][BitWord]`。
    *   对于每个二元约束 $C(X, Y)$，它存储了两组位图：
        1.  **X $\to$ Y**: 当 $X=a$ 时，允许的 $Y$ 的值的位图。
        2.  **Y $\to$ X**: 当 $Y=b$ 时，允许的 $X$ 的值的位图。

## 3. 构建逻辑 (`GModelAdapter::BuildBitSup`)

代码位于 `src/model/gmodel_adapter.cu` 第 359 行。

核心逻辑如下：
1.  遍历所有二元扩展约束 (Table Constraints)。
2.  遍历约束中的每个合法元组 `(x_val, y_val)`。
3.  **双向记录**:
    *   计算索引 `idx_x`: 对应 $X=x\_val$，在 $Y$ 的位图 (`bitDom` 格式) 中设置 `y_val` 对应的位。
        ```cpp
        idx_x = cid * size + (0 * max_dom + x_val) * int_size + y_word;
        bitSup[idx_x].x |= (1u << y_bit); // .x 存储 X->Y 的支持
        ```
    *   计算索引 `idx_y`: 对应 $Y=y\_val$，在 $X$ 的位图 (`bitDom` 格式) 中设置 `x_val` 对应的位。
        ```cpp
        idx_y = cid * size + (1 * max_dom + y_val) * int_size + x_word;
        bitSup[idx_y].y |= (1u << x_bit); // .y 存储 Y->X 的支持
        ```

**注意**: 这里使用了 `uint2` 的 `.x` 和 `.y` 字段来分别存储两个方向，或者它们在数组中是交错的？
*   代码中 `idx_x` 和 `idx_y` 的计算略有不同：
    *   `idx_x` 使用 `0 * max_dom_size` (方向 0)
    *   `idx_y` 使用 `1 * max_dom_size` (方向 1)
*   **实际存储**: 实际上 `bitSup` 应该被视为 `uint` 数组，但代码使用了 `uint2` 类型，可能利用了向量加载指令。
    *   看代码: `bitSup[idx_x].x |= ...` 和 `bitSup[idx_y].y |= ...`。
    *   这意味着 `bitSup` 数组的一个元素 `uint2` 同时服务于两个方向？
    *   **仔细看代码**:
        *   `idx_x` 依赖 `y_word`。
        *   `idx_y` 依赖 `x_word`。
        *   如果 `x_word != y_word`，它们会访问不同的 `uint2` 元素。
        *   **潜在混淆**: 代码中写的是 `bitSup[idx_x].x` 和 `bitSup[idx_y].y`。这表明数据结构设计是：
            对于每个 `word` 索引，`.x` 存的是方向 0 (X->Y) 的部分位图，`.y` 存的是方向 1 (Y->X) 的部分位图？
            **需要核实**: 如果 `max_dom_size` 很大，`idx_x` 和 `idx_y` 会相差很大，它们不可能指向同一个 `uint2`。
            **推测**: 代码可能是一个 `reinterpret_cast<uint2*>` 的数组，或者 `idx` 计算本身就包含了方向偏移。
            *   让我们看 `idx` 计算：
                `idx = cid * ... + (DIR * max_dom + val) * ... + word`
            *   这里 `idx` 是以 `uint2` 为单位的索引吗？如果是，那么 `bitSup[idx].x` 修改的是该单位的低 32 位。

## 4. GPU 使用 (`GModel.cu`)

在 Kernel `CsCheckMainKernel` 中：

```cpp
// 伪代码
for (int i = 0; i < n_words; ++i) {
    // 读取支持位图 (当 X=val 时，Y 允许的值)
    // 注意：这里访问使用了 .x 或 .y 取决于当前是检查哪个变量
    u32 supported_mask = bitSup[base_idx + i].x; 
    
    // 与当前 Y 的域 (d_dom_y) 做交集
    if (supported_mask & d_dom_y[i]) {
        has_support = true;
        break;
    }
}
```

*   **位并行加速**: 通过 `&` 操作一次性检查 32 个候选值。
*   **Texture 优化**: 虽然分配了 `texObj_BitSup`，但目前的 `CsCheckMainKernel` 似乎直接使用了全局内存指针 `bitSup` (为了利用 L1/L2 Cache 且避免纹理限制)。

## 5. 关键总结

*   **Bit-Parallel AC3**: 这是实现位并行 GAC 的核心数据结构。
*   **预计算**: 所有的支持关系都在 CPU 端预先计算好，GPU 只需要查表。
*   **内存消耗**: $O(Constraints \times DomainSize^2 / 32)$。对于大域问题，这可能非常占用显存。
