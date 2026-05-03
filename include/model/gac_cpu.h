// Copyright 2025
#pragma once

#include <cstdint>
#include <vector>

#include "absl/strings/string_view.h"

namespace cpim::model {

class IntermediateModel;

using u32 = uint32_t;

struct GacCpuStats {
  int iterations = 0;     // 传播迭代次数
  int deletions = 0;      // 删除的取值个数
  bool inconsistent = false;  // 是否检测到空域
};

// 串行 CPU 版 GAC（位集 + 支持表），用于正确性基线与对照。
class GacCpuRunner {
 public:
  explicit GacCpuRunner(const IntermediateModel& im);

  // 运行一次 GAC 传播直到收敛或发现空域
  GacCpuStats Run();

  // 打印简要统计与若干变量域
  void Print(int max_vars = 8) const;

  const std::vector<u32>& bit_dom() const { return bit_dom_; }
  const std::vector<int>& domain_sizes() const { return dom_size_; }
  int bit_words() const { return bit_words_; }
  int num_vars() const { return num_vars_; }
  int max_dom_size() const { return max_dom_size_; }

 private:
  // 维度
  int num_vars_ = 0;
  int num_constraints_ = 0;  // IM 的约束总数（包含非二元/非支持约束）
  int max_dom_size_ = 0;
  int bit_words_ = 0;  // 每个变量域占用多少个 u32

  // 变量域（位集）
  std::vector<u32> bit_dom_;     // [var * bit_words_ + word]
  std::vector<int> dom_size_;    // 每个变量当前域大小

  // 支持表（按 GModelAdapter 的布局，.x 表示 x->y，.y 表示 y->x）
  // [cid][dir(0/1)][value][word]
  struct UInt2 { u32 x; u32 y; };
  std::vector<UInt2> bit_sup_;   // size = num_constraints_ * 2 * max_dom_size_ * bit_words_

  // 仅对可用的二元 supports 约束填充元信息；其他置为 (-1,-1)
  struct BinMeta { int x = -1; int y = -1; };
  std::vector<BinMeta> bin_meta_; // size = num_constraints_

  // 传播队列标记（与 cuSAC 的 ConPre 类似）：1=待处理；0=空
  std::vector<int> con_pre_;     // size = num_constraints_

  // 引用的 IM（只读）
  const IntermediateModel& im_;

  // 构建/工具函数
  void BuildBitDom();
  void BuildBitSupAndMeta();

  // 压缩出本轮事件（串行 copy_if + 清零 con_pre）
  void Compress(std::vector<int>& events);

  // 在约束 cid 上做一次双向传播，返回删除的取值数
  int PropagateOnConstraint(int cid);

  // 单向传播：在 c=(x,y) 上用 y 域支持检查并删除 x 中无支撑的值
  int ReviseXY(int cid, int x, int y);

  // 删除取值并触发邻居约束
  inline void RemoveValue(int var, int value, std::vector<int>& touched_vars);
};

}  // namespace cpim::model
