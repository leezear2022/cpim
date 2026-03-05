#pragma once

#include <cstdint>
#include <vector>

namespace cpim {

/**
 * Trail 条目 (16 字节)
 * 记录单个域修改操作,用于回溯
 */
struct TrailEntry {
  enum Type : uint8_t {
    DOMAIN_CHANGE = 0,  // 域修改
  };

  Type type;
  uint8_t padding[7];      // 对齐到 8 字节
  int32_t var_id;          // 变量 ID
  int32_t word_index;      // bitDom 中的 word 索引
  uint64_t old_bits;       // 修改前的 bit 值 (BITSIZE=64，必须用 64 位)
};

/**
 * 统一 Trail 回溯系统
 *
 * 核心设计:
 * - 单层域 + 增量 Trail (vs 多级域全量拷贝)
 * - O(1) 回溯 (仅指针移动)
 * - 前向覆盖 (不恢复数据,由后续搜索覆盖)
 * - CPU/GPU 统一 (Phase 1.1 先实现 CPU 版本)
 *
 * 性能:
 * - 内存: O(num_changes) vs O(num_vars * max_depth)
 * - 回溯: O(1) vs O(num_vars * domain_size)
 *
 * 使用方式:
 *   trail.NewLevel();                                // 进入新层
 *   trail.RecordDomainChange(var_id, word, old);     // 记录修改
 *   trail.BacktrackTo(target_level);                 // 回溯
 */
// 前向声明
class IntVar;

class UnifiedTrail {
 public:
  /**
   * 构造函数
   * @param max_trail_size Trail 容量 (最大条目数)
   * @param enable_gpu 是否启用 GPU (Phase 1.1 暂时忽略,传 false)
   */
  explicit UnifiedTrail(int max_trail_size, bool enable_gpu = false);

  /**
   * 设置变量数组 (用于回溯时恢复域)
   */
  void SetVariables(std::vector<IntVar*>* vars) { vars_ = vars; }

  /**
   * 析构函数
   */
  ~UnifiedTrail();

  // 禁止拷贝,允许移动
  UnifiedTrail(const UnifiedTrail&) = delete;
  UnifiedTrail& operator=(const UnifiedTrail&) = delete;
  UnifiedTrail(UnifiedTrail&&) noexcept = default;
  UnifiedTrail& operator=(UnifiedTrail&&) noexcept = default;

  /**
   * 进入新的搜索层级 (O(1))
   * 记录当前 Trail 大小作为层级标记
   */
  void NewLevel();

  /**
   * 回溯到指定层级 (O(1))
   * 仅移动 Trail 指针,不恢复数据
   * 数据由后续搜索前向覆盖
   *
   * @param target_level 目标层级
   */
  void BacktrackTo(int target_level);

  /**
   * 记录域修改 (O(1))
   * 在修改域之前调用,记录旧值用于可能的回溯
   *
   * @param var_id 变量 ID
   * @param word_idx bitDom 中的 word 索引
   * @param old_bits 修改前的 bit 值
   */
  void RecordDomainChange(int var_id, int word_idx, uint64_t old_bits);

  /**
   * 获取当前层级
   */
  int CurrentLevel() const { return current_level_; }

  /**
   * 获取 Trail 当前大小 (已记录的条目数)
   */
  int Size() const { return trail_size_; }

  /**
   * 获取 Trail 容量
   */
  int Capacity() const { return trail_capacity_; }

  /**
   * 获取指定索引的 Trail 条目 (用于增量队列初始化等场景)
   * @param index 条目索引 (0 到 Size()-1)
   * @return Trail 条目的只读引用
   */
  const TrailEntry& GetEntry(int index) const { return trail_entries_[index]; }

  /**
   * GPU 辅助接口 (Phase 1.2 使用)
   * 获取 GPU 可访问的 Trail 指针
   */
  TrailEntry* GetGPUTrailPointer() { return trail_entries_; }
  int* GetGPULevelMarkersPointer() { return level_markers_.data(); }
  int* GetGPUCurrentSizePointer() { return &trail_size_; }

  /**
   * 调试: 打印 Trail 内容
   */
  void DebugPrint() const;

 private:
  // Trail 数据 (Phase 1.1 使用普通 C++ 内存,Phase 1.2 升级为 cudaMallocManaged)
  TrailEntry* trail_entries_;  // Trail 条目数组
  std::vector<int> level_markers_;  // 每层起始位置 level_markers_[level]

  int trail_size_;      // 当前 Trail 大小
  int trail_capacity_;  // Trail 容量
  int current_level_;   // 当前搜索层级

  bool gpu_enabled_;    // 是否启用 GPU (Phase 1.2)
  std::vector<IntVar*>* vars_;  // 变量数组指针 (用于回溯恢复)
};

}  // namespace cpim
