#include "model/gac_cpu.h"

#include <algorithm>
#include <iostream>
#include <stdexcept>

#include "glog/logging.h"
#include "model/intermediate_model.h"
#include "model/types.h"

namespace cpim::model {

namespace {

constexpr int kBitsPerWord = 32;

inline int IntSize(int nbits) { return (nbits + kBitsPerWord - 1) / kBitsPerWord; }

inline uint32_t TailMask(int bits) {
  if (bits <= 0) return 0u;
  if (bits >= kBitsPerWord) return 0xFFFFFFFFu;
  return (1u << bits) - 1u;
}

inline bool BitTest(const std::vector<u32>& bitset, int base, int value) {
  const int w = value / kBitsPerWord;
  const int b = value % kBitsPerWord;
  return (bitset[base + w] >> b) & 1u;
}

inline void BitClear(std::vector<u32>& bitset, int base, int value) {
  const int w = value / kBitsPerWord;
  const int b = value % kBitsPerWord;
  bitset[base + w] &= ~(1u << b);
}

}  // namespace

GacCpuRunner::GacCpuRunner(const IntermediateModel& im) : im_(im) {
  num_vars_ = im_.num_variables();
  num_constraints_ = im_.num_constraints();
  max_dom_size_ = 0;
  for (const auto& var : im_.variables()) {
    max_dom_size_ = std::max(max_dom_size_, im_.GetDomain(var.domain).Size());
  }
  bit_words_ = IntSize(max_dom_size_);

  bit_dom_.assign(num_vars_ * bit_words_, 0u);
  dom_size_.assign(num_vars_, 0);
  bin_meta_.assign(num_constraints_, BinMeta{});
  con_pre_.assign(num_constraints_, 0);

  // 预分配 bitSup（全部约束），非二元/非 supports 将保持 0
  const size_t bitsup_per_constraint = 2 * max_dom_size_ * bit_words_;
  bit_sup_.assign(num_constraints_ * bitsup_per_constraint, GacCpuRunner::UInt2{0u, 0u});

  BuildBitDom();
  BuildBitSupAndMeta();
}

void GacCpuRunner::BuildBitDom() {
  // 初始化 bit_dom 为各变量完整域（尾部不足 32 位的用 TailMask）
  for (const auto& var : im_.variables()) {
    const int vid = var.id.value;
    const int size = im_.GetDomain(var.domain).Size();
    dom_size_[vid] = size;
    const int base = vid * bit_words_;
    for (int w = 0; w < bit_words_; ++w) {
      const int remain = size - w * kBitsPerWord;
      bit_dom_[base + w] = TailMask(remain);
    }
  }
}

void GacCpuRunner::BuildBitSupAndMeta() {
  const size_t bitsup_per_constraint = 2 * max_dom_size_ * bit_words_;

  for (int cid = 0; cid < num_constraints_; ++cid) {
    const Constraint& c = im_.constraints()[cid];
    const auto* ext = std::get_if<ExtensionConstraint>(&c.data);
    if (!ext || ext->Arity() != 2) {
      continue;  // 非二元暂不支持
    }
    if (ext->semantics != ExtensionConstraint::Semantics::kSupports) {
      continue;  // 仅处理 supports
    }

    const int x = ext->scope[0].value;
    const int y = ext->scope[1].value;
    bin_meta_[cid] = {x, y};

    // 标记该约束初始需要处理
    con_pre_[cid] = 1;

    // 填充支持表位集
    for (const auto& tup : ext->tuples) {
      if (tup.size() != 2) continue;
      const int xv = tup[0];
      const int yv = tup[1];
      if (xv < 0 || yv < 0 || xv >= max_dom_size_ || yv >= max_dom_size_) continue;

      const int y_word = yv / kBitsPerWord;
      const int y_bit = yv % kBitsPerWord;
      const int x_word = xv / kBitsPerWord;
      const int x_bit = xv % kBitsPerWord;

      const int base = cid * bitsup_per_constraint;
      const int idx_x = base + (0 * max_dom_size_ + xv) * bit_words_ + y_word;
      const int idx_y = base + (1 * max_dom_size_ + yv) * bit_words_ + x_word;

      bit_sup_[idx_x].x |= (1u << y_bit);
      bit_sup_[idx_y].y |= (1u << x_bit);
    }
  }
}

void GacCpuRunner::Compress(std::vector<int>& events) {
  events.clear();
  events.reserve(256);
  for (int cid = 0; cid < num_constraints_; ++cid) {
    if (con_pre_[cid] != 0 && bin_meta_[cid].x >= 0) {
      events.push_back(cid);
      con_pre_[cid] = 0;
    }
  }
}

int GacCpuRunner::ReviseXY(int cid, int x, int y) {
  int deletions = 0;
  const int x_base = x * bit_words_;
  const int y_base = y * bit_words_;
  const int x_size = dom_size_[x];
  const int bitsup_per_constraint = 2 * max_dom_size_ * bit_words_;
  const int sup_base = cid * bitsup_per_constraint;

  // 扫描 x 的每个取值，检查与 y 的交支持是否为空
  for (int xv = 0; xv < x_size; ++xv) {
    if (!BitTest(bit_dom_, x_base, xv)) continue;  // 已删除

    // 取出 x->y 的支持位集（跨 bit_words_）
    const int idx_x = sup_base + (0 * max_dom_size_ + xv) * bit_words_;

    bool supported = false;
    for (int w = 0; w < bit_words_; ++w) {
      const u32 sup_word = bit_sup_[idx_x + w].x;
      const u32 y_word = bit_dom_[y_base + w];
      if ((sup_word & y_word) != 0u) { supported = true; break; }
    }

    if (!supported) {
      BitClear(bit_dom_, x_base, xv);
      --dom_size_[x];
      ++deletions;
      if (dom_size_[x] == 0) return deletions;  // 空域可提前返回
    }
  }
  return deletions;
}

int GacCpuRunner::PropagateOnConstraint(int cid) {
  const BinMeta m = bin_meta_[cid];
  if (m.x < 0) return 0;  // 非二元，忽略

  int del = 0;
  // x ← y
  del += ReviseXY(cid, m.x, m.y);
  if (dom_size_[m.x] == 0) return del;

  // y ← x（同一个 cid 上另一方向）
  // 注意：同一个 cid 的 y->x 支持位集在 bit_sup_.y 字段
  // 为了复用 ReviseXY 的逻辑，交换角色即可（内部始终访问 .x）
  // 因此这里进行一次“视图转换”：通过交换 m.x/m.y 和在 Build 时填充的对称关系实现
  // 具体做法：将 bit_sup_ 视为 y->x 的布局相同（在 Build 时已填充 .y 对称位）
  // 为不修改 ReviseXY，这里临时交换两变量把第二次调用映射为 y ← x

  // 小技巧：把 cid 的 x/y 逻辑互换后，借助 y->x 的填充效果实现对称传播
  // 我们可以在 ReviseXY 中始终读 .x，因此这里需要一个代用路径：
  // 复制一段临时的 sup 视图并把 .y 映射到 .x 上代用，避免额外分支（开销小，正确优先）

  // 简化：直接写一个内联版本遍历使用 .y
  const int y = m.y, x = m.x;
  const int y_base = y * bit_words_;
  const int x_base = x * bit_words_;
  const int y_size = dom_size_[y];
  const int bitsup_per_constraint = 2 * max_dom_size_ * bit_words_;
  const int sup_base = cid * bitsup_per_constraint;

  for (int yv = 0; yv < y_size; ++yv) {
    if (!BitTest(bit_dom_, y_base, yv)) continue;
    const int idx_y = sup_base + (1 * max_dom_size_ + yv) * bit_words_;
    bool supported = false;
    for (int w = 0; w < bit_words_; ++w) {
      const u32 sup_word = bit_sup_[idx_y + w].y;  // 注意使用 .y
      const u32 x_word = bit_dom_[x_base + w];
      if ((sup_word & x_word) != 0u) { supported = true; break; }
    }
    if (!supported) {
      BitClear(bit_dom_, y_base, yv);
      --dom_size_[y];
      ++del;
      if (dom_size_[y] == 0) break;
    }
  }

  return del;
}

GacCpuStats GacCpuRunner::Run() {
  GacCpuStats stats;

  std::vector<int> events;
  Compress(events);

  while (!events.empty()) {
    ++stats.iterations;
    for (int cid : events) {
      const BinMeta m = bin_meta_[cid];
      if (m.x < 0) continue;

      const int before_x = dom_size_[m.x];
      const int before_y = dom_size_[m.y];
      stats.deletions += PropagateOnConstraint(cid);

      if (dom_size_[m.x] == 0 || dom_size_[m.y] == 0) {
        stats.inconsistent = true;
        return stats;
      }

      // 若有删除，触发相关邻居约束入队（下轮处理）
      if (dom_size_[m.x] != before_x) {
        for (const auto c2 : im_.GetConstraintsForVariable(VariableId{m.x})) {
          const int nid = c2.value;
          if (bin_meta_[nid].x >= 0) con_pre_[nid] = 1;
        }
      }
      if (dom_size_[m.y] != before_y) {
        for (const auto c2 : im_.GetConstraintsForVariable(VariableId{m.y})) {
          const int nid = c2.value;
          if (bin_meta_[nid].x >= 0) con_pre_[nid] = 1;
        }
      }
    }

    // 压缩下一轮事件
    Compress(events);
  }

  return stats;
}

void GacCpuRunner::Print(int max_vars) const {
  std::cout << "\n=== GacCpuRunner Summary ===" << std::endl;
  std::cout << "Variables: " << num_vars_ << std::endl;
  std::cout << "Constraints (IM total): " << num_constraints_ << std::endl;
  std::cout << "Max domain size: " << max_dom_size_ << std::endl;
  std::cout << "Bit words per domain: " << bit_words_ << std::endl;

  const int limit = std::min(max_vars, num_vars_);
  for (int v = 0; v < limit; ++v) {
    std::cout << "var[" << v << "] size=" << dom_size_[v] << ": ";
    const int base = v * bit_words_;
    for (int w = 0; w < bit_words_; ++w) {
      std::cout << "0x" << std::hex << bit_dom_[base + w] << std::dec;
      if (w + 1 < bit_words_) std::cout << " ";
    }
    std::cout << std::endl;
  }
}

}  // namespace cpim::model
