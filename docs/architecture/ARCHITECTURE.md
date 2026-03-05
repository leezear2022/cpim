# CPIM 架构文档

## 核心组件

### 1. 模型层

| 组件 | 文件 | 说明 |
|------|------|------|
| HModel | `include/xcsp3model/HModel.h` | 传统高层模型（HVar, HTab） |
| IntermediateModel | `include/model/intermediate_model.h` | 现代规范化模型 |
| Network | `include/Network.h` | 运行时约束网络，支持多级域 |

### 2. 一致性算法

| 算法 | 文件 | 说明 |
|------|------|------|
| AC3/AC3bit/AC3rm | `src/AC3*.cpp` | 弧一致性 |
| FC/FCbit | `src/FC.cpp` | 前向检查 |
| SAC1/SAC3 | `src/SAC*.cpp` | 单例弧一致性 |
| lMaxRPC/RPC3 | `src/lMaxRPC.cpp`, `src/RPC3.cpp` | 路径一致性 |
| NSAC | `src/NSAC.cpp` | 邻域弧一致性 |

### 3. 搜索引擎

- **MAC** (`src/MAC.cpp`): 主搜索算法
  - 变量启发式: `VRH_DOM_MIN`, `VRH_DOM_WDEG_MIN` 等
  - 值启发式: `VLH_MIN` 等
  - 多级域管理: `NewLevel()`, `BackTo()`

### 4. GPU 求解器

| 组件 | 文件 | 说明 |
|------|------|------|
| CModel | `src/cuSAC.cu` | 传统 GPU 模型（纹理内存） |
| GModel | `src/GModel.cu` | 简化 GPU 模型（统一内存，Jetson 优化） |

## 数据流

### 传统管线 (HModel)
```
XCSP3 → XBuilder → HModel → Network → MAC + AC算法 → 解
```

### 现代管线 (IntermediateModel)
```
XCSP3 → XcspParser → ModelBuilder → ModelNormalizer → IntermediateModel
  ├→ GModel (GPU)
  └→ Network → MAC (CPU)
```

## Bitset 表示

- `BITSIZE = 64`: 64位字
- `DIV_BIT = 6`: 除以64 (右移6位)
- `MOD_MASK = 0x3f`: 位偏移 (与63取模)
- 域表示: `bit_doms_[level][word_idx]`

## 搜索级别管理

- Level 0: 初始状态
- 每次赋值创建新级别
- `Network::NewLevel(src)`: 复制级别
- `Network::BackTo(dest)`: 回溯
- `IntVar::bit_doms_[level]`: 各级域状态

## 文件组织

```
include/
  ├── xcsp3model/     # HModel, HVar, HTab, XBuilder
  ├── model/          # 现代模型栈
  ├── Network.h       # 运行时网络
  ├── Solver.h        # AC算法和MAC
  ├── GModel.cuh      # 简化GPU模型
  └── cuSAC.cuh       # 传统GPU模型

src/
  ├── AC*.cpp         # AC算法实现
  ├── MAC.cpp         # 搜索实现
  ├── Network.cpp     # 网络实现
  ├── GModel.cu       # GPU模型实现
  └── model/          # 现代模型实现

samples/
  ├── main_new_parser.cpp  # CPU求解器入口
  ├── verify_gac.cpp       # GAC验证工具
  ├── verify_search.cpp    # 搜索验证工具
  └── bench/               # 测试实例
```
