#include "base/unified_trail.h"

#include <glog/logging.h>

#include <iomanip>
#include <iostream>

#include "Network.h"  // Phase 1.1: 需要 IntVar 定义

namespace cpim {

UnifiedTrail::UnifiedTrail(int max_trail_size, bool enable_gpu)
    : trail_entries_(nullptr),
      trail_size_(0),
      trail_capacity_(max_trail_size),
      current_level_(-1),  // -1 表示初始状态,第一次 NewLevel() 将变为 0
      gpu_enabled_(enable_gpu),
      vars_(nullptr) {  // Phase 1.1: 初始化变量数组指针
  // Phase 1.1: 使用普通 C++ 内存
  // Phase 1.2: 改为 cudaMallocManaged
  trail_entries_ = new TrailEntry[max_trail_size];

  // 预留层级标记空间 (假设最大深度 1000)
  level_markers_.reserve(1000);

  LOG(INFO) << "UnifiedTrail created: capacity=" << trail_capacity_
            << ", gpu_enabled=" << gpu_enabled_;
}

UnifiedTrail::~UnifiedTrail() {
  if (trail_entries_) {
    delete[] trail_entries_;
    trail_entries_ = nullptr;
  }
  LOG(INFO) << "UnifiedTrail destroyed";
}

void UnifiedTrail::NewLevel() {
  current_level_++;

  // 记录当前 Trail 大小作为新层的起始位置
  if (static_cast<size_t>(current_level_) >= level_markers_.size()) {
    level_markers_.push_back(trail_size_);
  } else {
    level_markers_[current_level_] = trail_size_;
  }

  VLOG(2) << "Trail::NewLevel() -> level=" << current_level_
          << ", trail_size=" << trail_size_;
}

void UnifiedTrail::BacktrackTo(int target_level) {
  // Phase 1.1: 允许回溯到 -1 (初始状态,表示完全清空)
  CHECK_GE(target_level, -1) << "Invalid backtrack level: " << target_level;
  CHECK_LE(target_level, current_level_)
      << "Cannot backtrack to future level: " << target_level
      << " (current=" << current_level_ << ")";

  if (target_level >= current_level_) {
    return;  // 无需回溯
  }

  // Bug Fix: 计算目标 trail 大小 - 保留 level 0..target_level 的修改
  // level_markers_[N] 是 level N 的起始位置
  // 回溯到 level N 应该撤销 level N+1 及之后的修改
  // 因此 target_trail_size = level_markers_[N+1]（下一层的起始位置）
  int target_trail_size;
  if (target_level == -1) {
    target_trail_size = 0;  // 回到初始状态
  } else if (target_level + 1 < static_cast<int>(level_markers_.size())) {
    target_trail_size = level_markers_[target_level + 1];  // 下一层的起始位置
  } else {
    // target_level 是最后一层，无需回溯
    return;
  }

  // Phase 1.1: 恢复域数据 (从后向前遍历 trail entries)
  CHECK(vars_ != nullptr) << "Variables not set! Call SetVariables() first.";
  int old_trail_size = trail_size_;
  for (int i = trail_size_ - 1; i >= target_trail_size; --i) {
    const TrailEntry& entry = trail_entries_[i];
    if (entry.type == TrailEntry::DOMAIN_CHANGE) {
      (*vars_)[entry.var_id]->RestoreBitWord(entry.word_index, entry.old_bits);
    }
  }

  // 更新 trail 指针
  trail_size_ = target_trail_size;
  current_level_ = target_level;

  VLOG(2) << "Trail::BacktrackTo(" << target_level << ") -> restored "
          << (old_trail_size - target_trail_size) << " changes, trail_size="
          << trail_size_;
}

void UnifiedTrail::RecordDomainChange(int var_id, int word_idx,
                                       uint32_t old_bits) {
  CHECK_LT(trail_size_, trail_capacity_)
      << "Trail overflow! Increase max_trail_size. Current capacity="
      << trail_capacity_;

  // 记录修改
  trail_entries_[trail_size_] = {
      TrailEntry::DOMAIN_CHANGE,
      {0, 0, 0},  // padding
      var_id,
      word_idx,
      old_bits,
  };

  trail_size_++;

  VLOG(3) << "Trail::RecordDomainChange(var=" << var_id << ", word=" << word_idx
          << ", old_bits=" << std::hex << old_bits << std::dec
          << ") -> trail_size=" << trail_size_;
}

void UnifiedTrail::DebugPrint() const {
  std::cout << "===== UnifiedTrail Debug Info =====" << std::endl;
  std::cout << "Current Level: " << current_level_ << std::endl;
  std::cout << "Trail Size: " << trail_size_ << "/" << trail_capacity_
            << std::endl;
  std::cout << "Level Markers: ";
  for (size_t i = 0; i < level_markers_.size(); ++i) {
    std::cout << "[" << i << "]=" << level_markers_[i] << " ";
  }
  std::cout << std::endl;

  std::cout << "\nTrail Entries:" << std::endl;
  for (int i = 0; i < trail_size_; ++i) {
    const auto& e = trail_entries_[i];
    std::cout << "  [" << std::setw(4) << i << "] "
              << "type=" << static_cast<int>(e.type) << " var=" << std::setw(4)
              << e.var_id << " word=" << std::setw(2) << e.word_index
              << " old_bits=" << std::hex << std::setw(8) << e.old_bits
              << std::dec << std::endl;
  }
  std::cout << "===================================" << std::endl;
}

}  // namespace cpim
