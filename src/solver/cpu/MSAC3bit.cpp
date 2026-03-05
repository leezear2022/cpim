#include "Solver.h"
#include <algorithm>
#include <chrono>
#include <glog/logging.h>

namespace cpim {

// Phase 0.2: MSACStats::Print() 实现
void MSACStats::Print() const {
  LOG(INFO) << "MSAC stats (global):";
  LOG(INFO) << "  Total enforce calls: " << total_enforce_calls;
  LOG(INFO) << "  Total probes: " << total_probes
            << " (" << total_probe_fail << " failed)";
  LOG(INFO) << "  Total values removed: " << total_removed;
  LOG(INFO) << "  Total probe time: " << total_probe_time_ms << " ms";
  if (total_enforce_calls > 0) {
    LOG(INFO) << "  Avg probes per enforce: "
              << static_cast<double>(total_probes) / total_enforce_calls;
  }
  // Phase 2.1: 缓存统计
  if (total_cache_hits > 0 || total_cache_misses > 0) {
    int64_t total_checks = total_cache_hits + total_cache_misses;
    double hit_rate = 100.0 * total_cache_hits / total_checks;
    LOG(INFO) << "  Cache hits: " << total_cache_hits
              << ", misses: " << total_cache_misses
              << " (hit rate: " << hit_rate << "%)";
  }
  // Phase 2.2: 快速检查统计
  if (total_quick_reject > 0 || total_quick_pass > 0) {
    int64_t total_quick = total_quick_reject + total_quick_pass;
    double reject_rate = 100.0 * total_quick_reject / total_quick;
    LOG(INFO) << "  Quick reject: " << total_quick_reject
              << ", pass: " << total_quick_pass
              << " (reject rate: " << reject_rate << "%)";
  }
}

MSAC3bit::MSAC3bit(Network* m, MSACConfig config)
    : AC(m), config_(config) {
  kernel_ = new AC3bit(m);

  // Phase 2.1: 初始化 probe 缓存
  probe_cache_.resize(m->vars.size());
  for (auto* x : m->vars) {
    probe_cache_[x->id()].resize(x->capacity() + 1);
  }

  // Phase 3: 预分配 SAC-SDS 支持计数器（仅在 SAC_SDS 模式下）
  if (config_.mode == MSACConfig::SAC_SDS) {
    int max_size = m->tabs.size() * m->max_arity() * m->max_domain_size();
    support_count_.reserve(max_size);
    VLOG(1) << "[SAC-SDS] Reserved support_count_ capacity: " << max_size;
  }
}

MSAC3bit::~MSAC3bit() {
  delete kernel_;
}

ConsistencyState MSAC3bit::enforce(vector<IntVar*>& x_evt, int level) {
  level_ = level;
  const int base_trail_level = m_->trail()->CurrentLevel();

  // FIX: 每次 enforce 重置统计（预算应该是每次 enforce 独立计算）
  stats_.Reset();
  ResetCache();  // Phase 2.1: 重置 probe 缓存

  VLOG(1) << "[MSAC3bit] enforce() called at level " << level
          << ", evt_size=" << x_evt.size()
          << ", mode=" << (config_.mode == MSACConfig::SAC3 ? "SAC3" : "SAC1")
          << ", trail_level=" << base_trail_level;

  // 增量初始化：记录 AC 阶段开始前的 Trail 位置
  ac_start_trail_pos_ = m_->trail()->Size();

  // Phase 1: 先运行 AC3bit kernel（正常传播，允许 weight 更新）
  ConsistencyState cs = kernel_->enforce(x_evt, level);
  VLOG(1) << "[MSAC3bit] AC3bit kernel " << (cs.state ? "succeeded" : "failed");
  if (!cs.state) {
    stats_.AccumulateToGlobal();  // 累加到全局统计
    return cs;  // AC 已经失败
  }

  // SAC3 模式：增量初始化队列（只入队 AC 阶段修改的变量的邻域）
  // 原因：队列状态不随 Trail 回溯，但可以利用 Trail 精确追踪 AC 修改
  if (config_.mode == MSACConfig::SAC3) {
    InitializeQueueIncremental();
  }

  // Phase 2: SAC 不动点循环
  VLOG(1) << "[MSAC3bit] Starting SAC fixed-point loop";

  if (config_.mode == MSACConfig::SAC3) {
    // SAC3: 深度优先（DFS）风格，符合原论文
    // 从队列取一个值，probe，失败则入队邻域，继续
    VLOG(1) << "[MSAC3bit] SAC3 DFS mode, initial queue size: "
            << pending_queue_.size();

    while (!pending_queue_.empty() && ShouldContinueProbe(level)) {
      // 取队首
      auto [x, a] = pending_queue_.back();
      pending_queue_.pop_back();

      // 清除 in_queue 标记
      if (x->id() < static_cast<int>(in_queue_.size()) &&
          a < static_cast<int>(in_queue_[x->id()].size())) {
        in_queue_[x->id()][a] = false;
      }

      // 值可能已被删除或变量已赋值
      if (!x->have(a) || x->assigned()) continue;

      // Phase 2.1: 缓存检查 - 避免重复 probe
      if (IsCacheValid(x, a)) {
        ++stats_.total_cache_hits;
        VLOG(2) << "[MSAC3bit] Cache hit: var=" << x->id() << ", val=" << a;
        continue;  // 跳过重复 probe
      }
      ++stats_.total_cache_misses;

      // Phase 2.2: 快速支持检测 - 在完整 probe 前快速拒绝无支持值
      if (!QuickCheckSupport(x, a)) {
        ++stats_.total_quick_reject;
        VLOG(1) << "[MSAC3bit] Quick reject: var=" << x->id() << ", val=" << a;

        // 快速拒绝：直接删除值，无需完整 probe
        x->RemoveValue(a);
        ++stats_.num_removed;

        // 入队邻域
        EnqueueNeighborhood(x, a);

        if (x->faild()) {
          cs.state = false;
          cs.var = x;
          stats_.AccumulateToGlobal();
          return cs;
        }

        // 重新传播
        vector<IntVar*> evt = {x};
        cs = kernel_->enforce(evt, level);
        if (!cs.state) {
          stats_.AccumulateToGlobal();
          return cs;
        }
        continue;  // 跳过完整 probe
      }
      ++stats_.total_quick_pass;

      // Probe (x=a)
      auto probe_start = std::chrono::high_resolution_clock::now();
      bool probe_result = ProbeValue(x, a, level);
      auto probe_end = std::chrono::high_resolution_clock::now();
      stats_.probe_time_ms += std::chrono::duration<double, std::milli>(
          probe_end - probe_start).count();

      if (probe_result) {
        // Probe 成功：更新缓存
        UpdateCache(x, a);
      } else {
        // Probe 失败，删除值
        VLOG(1) << "[MSAC3bit] Probe failed: var=" << x->id() << ", val=" << a
                << ", current_size=" << x->size();
        x->RemoveValue(a);
        ++stats_.num_removed;
        VLOG(1) << "[MSAC3bit] After removal: var=" << x->id()
                << ", size=" << x->size();

        // 立即将邻域值入队（DFS 核心：新入队的值可能下次就取到）
        EnqueueNeighborhood(x, a);

        if (x->faild()) {
          cs.state = false;
          cs.var = x;
          stats_.AccumulateToGlobal();
          return cs;
        }

        // 重新传播
        vector<IntVar*> evt = {x};
        cs = kernel_->enforce(evt, level);
        if (!cs.state) {
          stats_.AccumulateToGlobal();
          return cs;
        }
      }
    }

    if (!pending_queue_.empty()) {
      stats_.exited_by_budget = true;
      VLOG(1) << "[MSAC3bit] Budget exhausted, " << pending_queue_.size()
              << " values remaining in queue";
    }
  } else if (config_.mode == MSACConfig::SAC_SDS) {
    // SAC-SDS: 支持驱动的 SAC，仅检查零支持值
    VLOG(1) << "[SAC-SDS] Starting support-driven SAC";

    // 初始化支持计数器
    InitializeSupportCountsOptimized();

    // 初始化零支持队列
    InitializeZeroSupportQueue();

    VLOG(1) << "[SAC-SDS] Initial zero-support queue: "
            << pending_queue_.size() << " values";

    // SAC-SDS 主循环：仅 probe 零支持值
    while (!pending_queue_.empty() && ShouldContinueProbe(level)) {
      // 取队尾（LIFO）
      auto [x, a] = pending_queue_.back();
      pending_queue_.pop_back();

      // 清除 in_queue 标记
      if (x->id() < static_cast<int>(in_queue_.size()) &&
          a < static_cast<int>(in_queue_[x->id()].size())) {
        in_queue_[x->id()][a] = false;
      }

      // 值可能已被删除或变量已赋值
      if (!x->have(a) || x->assigned()) continue;

      // Probe 零支持值
      auto probe_start = std::chrono::high_resolution_clock::now();
      bool probe_result = ProbeValue(x, a, level);
      auto probe_end = std::chrono::high_resolution_clock::now();
      stats_.probe_time_ms += std::chrono::duration<double, std::milli>(
          probe_end - probe_start).count();

      if (!probe_result) {
        // Probe 失败，删除值
        VLOG(1) << "[SAC-SDS] Probe failed: var=" << x->id() << ", val=" << a;
        x->RemoveValue(a);
        ++stats_.num_removed;

        // 核心：更新依赖该值的支持计数
        UpdateSupportsAfterRemoval(x, a);

        if (x->faild()) {
          cs.state = false;
          cs.var = x;
          stats_.AccumulateToGlobal();
          return cs;
        }

        // 重新传播 AC
        vector<IntVar*> evt = {x};
        cs = kernel_->enforce(evt, level);
        if (!cs.state) {
          stats_.AccumulateToGlobal();
          return cs;
        }
      }
    }

    if (!pending_queue_.empty()) {
      stats_.exited_by_budget = true;
      VLOG(1) << "[SAC-SDS] Budget exhausted, " << pending_queue_.size()
              << " values remaining in queue";
    }

    VLOG(1) << "[SAC-SDS] Completed: " << stats_.num_probes << " probes, "
            << stats_.total_zero_detections << " zero-support values detected, "
            << stats_.total_support_updates << " support updates";
  } else {
    // SAC1: 广度优先（BFS）风格，每轮全扫描
    bool changed = true;
    int iteration = 0;

    while (changed && ShouldContinueProbe(level)) {
      ++iteration;
      changed = false;

      // 全扫描所有剩余值
      vector<pair<IntVar*, int>> candidates;
      SelectCandidates(candidates, level);
      VLOG(1) << "[MSAC3bit] SAC1 iteration " << iteration << ": "
              << candidates.size() << " candidates to probe";

      if (candidates.empty()) break;

      for (auto& [x, a] : candidates) {
        if (!ShouldContinueProbe(level)) {
          stats_.exited_by_budget = true;
          VLOG(1) << "[MSAC3bit] Budget exhausted";
          break;
        }

        if (!x->have(a)) continue;

        auto probe_start = std::chrono::high_resolution_clock::now();
        bool probe_result = ProbeValue(x, a, level);
        auto probe_end = std::chrono::high_resolution_clock::now();
        stats_.probe_time_ms += std::chrono::duration<double, std::milli>(
            probe_end - probe_start).count();

        if (!probe_result) {
          VLOG(1) << "[MSAC3bit] Probe failed: var=" << x->id() << ", val=" << a
                  << ", current_size=" << x->size();
          x->RemoveValue(a);
          ++stats_.num_removed;
          VLOG(1) << "[MSAC3bit] After removal: var=" << x->id()
                  << ", size=" << x->size();

          if (x->faild()) {
            cs.state = false;
            cs.var = x;
            stats_.AccumulateToGlobal();
            return cs;
          }

          vector<IntVar*> evt = {x};
          cs = kernel_->enforce(evt, level);
          if (!cs.state) {
            stats_.AccumulateToGlobal();
            return cs;
          }

          changed = true;
        }
      }
    }
  }

  VLOG(1) << "[MSAC3bit] SAC completed: "
          << stats_.num_probes << " probes, " << stats_.num_removed << " removals";
  cs.state = true;
  stats_.AccumulateToGlobal();  // 累加到全局统计
  return cs;
}

bool MSAC3bit::ProbeValue(IntVar* x, int a, int level) {
  ++stats_.num_probes;

  // Phase 0.1: 禁止 weight 更新（防止污染）
  ScopedWeightUpdates guard(kernel_, false);

  // 记录当前层级（应该回溯到这个层级）
  const int before_level = m_->trail()->CurrentLevel();

  // 优化：只保存 x 的 assigned 状态（只有 x 会被 ReduceTo 修改）
  // 原来是 O(n) 保存所有变量，现在是 O(1)
  const bool x_was_assigned = x->assigned();

  // 创建新层进行测试
  m_->trail()->NewLevel();
  VLOG(2) << "[MSAC3bit] ProbeValue: before_level=" << before_level
          << ", test_level=" << m_->trail()->CurrentLevel();

  // 临时赋值 x=a
  x->ReduceTo(a);
  x->assign(true);

  // 运行 AC3bit kernel（weight 更新已被禁止）
  vector<IntVar*> evt = {x};
  ConsistencyState cs = kernel_->enforce(evt, level);

  // 恢复到测试前状态（回溯到 before_level，而不是 test_level - 1）
  // 这很重要：确保回溯到 probe 前的状态，保留 MSAC 之前的删值
  m_->trail()->BacktrackTo(before_level);

  // 优化：只恢复 x 的 assigned 状态 O(1)
  x->assign(x_was_assigned);

  VLOG(2) << "[MSAC3bit] ProbeValue done: after_level=" << m_->trail()->CurrentLevel()
          << " (expected " << before_level << ")";

  // 记录 probe 结果
  if (!cs.state) {
    ++stats_.num_probe_fail;
  }

  return cs.state;
}

void MSAC3bit::SelectCandidates(vector<pair<IntVar*, int>>& candidates,
                                 int level) {
  (void)level;
  candidates.clear();

  if (config_.mode == MSACConfig::SAC1) {
    // SAC1 全扫：遍历所有未赋值变量的所有值
    int total_domain_size = 0;
    for (auto v : m_->vars) {
      if (!v->assigned()) {
        int var_size = 0;
        for (int a = v->head(); a != Limits::INDEX_OVERFLOW; a = v->next(a)) {
          candidates.push_back({v, a});
          ++var_size;
        }
        total_domain_size += var_size;
        VLOG(2) << "[MSAC3bit] SAC1 var " << v->id() << " has " << var_size << " values";
      }
    }
    VLOG(1) << "[MSAC3bit] SAC1 total domain size: " << total_domain_size;
  } else {
    // SAC3 队列：从待检查队列中取候选
    // 优化：使用 move + clear 替代逐个 erase，O(k·log k) → O(k)
    for (auto& [x, a] : pending_queue_) {
      if (x->have(a) && !x->assigned()) {
        candidates.push_back({x, a});
      }
      // 标记已移出队列
      if (x->id() < static_cast<int>(in_queue_.size()) &&
          a < static_cast<int>(in_queue_[x->id()].size())) {
        in_queue_[x->id()][a] = false;
      }
    }
    pending_queue_.clear();
  }

  // 启发式优化：按域大小排序（小域优先）
  // 理由：小域变量更容易失败，优先检查可以更快发现 SAC 不一致
  std::sort(candidates.begin(), candidates.end(),
    [](const auto& a, const auto& b) {
      return a.first->size() < b.first->size();
    });
}

void MSAC3bit::InitializeQueue() {
  VLOG(1) << "[MSAC3bit] Initializing SAC3 queue";
  pending_queue_.clear();

  // 初始化 in_queue_ 标记数组
  in_queue_.resize(m_->vars.size());
  for (auto* x : m_->vars) {
    in_queue_[x->id()].assign(x->capacity() + 1, false);
  }

  // 所有未赋值变量的所有值入队
  // 优化：使用 push_back 替代 insert，O(log k) → O(1)
  for (auto* x : m_->vars) {
    if (!x->assigned()) {
      for (int a = x->head(); a != Limits::INDEX_OVERFLOW; a = x->next(a)) {
        pending_queue_.push_back({x, a});
        in_queue_[x->id()][a] = true;
      }
    }
  }

  VLOG(1) << "[MSAC3bit] Queue initialized with " << pending_queue_.size() << " values";
}

void MSAC3bit::InitializeQueueIncremental() {
  VLOG(1) << "[MSAC3bit] Incremental queue initialization";
  pending_queue_.clear();

  // 初始化 in_queue_ 标记数组
  in_queue_.resize(m_->vars.size());
  for (auto* x : m_->vars) {
    in_queue_[x->id()].assign(x->capacity() + 1, false);
  }

  // 从 Trail 中找出 AC 阶段修改过的变量
  std::vector<bool> modified_vars(m_->vars.size(), false);
  int num_modified = 0;
  const int trail_end = m_->trail()->Size();

  for (int i = ac_start_trail_pos_; i < trail_end; ++i) {
    const auto& entry = m_->trail()->GetEntry(i);
    if (entry.type == TrailEntry::DOMAIN_CHANGE) {
      int var_id = entry.var_id;
      if (var_id >= 0 && var_id < static_cast<int>(m_->vars.size()) &&
          !modified_vars[var_id]) {
        modified_vars[var_id] = true;
        ++num_modified;
      }
    }
  }

  VLOG(1) << "[MSAC3bit] AC phase modified " << num_modified << " variables"
          << " (trail entries: " << (trail_end - ac_start_trail_pos_) << ")";

  // 如果 AC 阶段没有修改任何变量，则入队所有未赋值变量的值
  // （这种情况发生在第一次 enforce，或者 evt 为空的情况）
  if (num_modified == 0) {
    for (auto* x : m_->vars) {
      if (!x->assigned()) {
        for (int a = x->head(); a != Limits::INDEX_OVERFLOW; a = x->next(a)) {
          pending_queue_.push_back({x, a});
          in_queue_[x->id()][a] = true;
        }
      }
    }
    VLOG(1) << "[MSAC3bit] No AC changes, fallback to full init: "
            << pending_queue_.size() << " values";
    return;
  }

  // 将修改变量自身的值入队（自己的域缩小了，需要重新 probe）
  // 同时将其邻域变量的值入队
  for (int var_id = 0; var_id < static_cast<int>(m_->vars.size()); ++var_id) {
    if (!modified_vars[var_id]) continue;

    IntVar* x = m_->vars[var_id];

    // 入队 x 自己的值
    if (!x->assigned()) {
      for (int a = x->head(); a != Limits::INDEX_OVERFLOW; a = x->next(a)) {
        EnqueueValue(x, a);
      }
    }

    // 入队 x 的邻域变量的值
    EnqueueNeighborhood(x, -1);
  }

  VLOG(1) << "[MSAC3bit] Incremental init: " << pending_queue_.size() << " values"
          << " (vs full scan would be ~" << m_->vars.size() << "*d)";
}

void MSAC3bit::EnqueueNeighborhood(IntVar* x, int deleted_val) {
  (void)deleted_val;  // 可用于更精细的入队策略

  // 将 x 的所有邻域变量的所有值入队
  // 使用 Network 的 subscription 获取变量关联的约束
  auto it = m_->subscription.find(x);
  if (it == m_->subscription.end()) return;

  for (auto* c : it->second) {
    for (auto* y : c->scope) {
      if (y != x && !y->assigned()) {
        for (int b = y->head(); b != Limits::INDEX_OVERFLOW; b = y->next(b)) {
          EnqueueValue(y, b);
        }
      }
    }
  }
}

void MSAC3bit::EnqueueValue(IntVar* x, int a) {
  // 优化：使用 push_back 替代 insert，O(log k) → O(1)
  if (x->id() < static_cast<int>(in_queue_.size()) &&
      a < static_cast<int>(in_queue_[x->id()].size()) &&
      !in_queue_[x->id()][a]) {
    pending_queue_.push_back({x, a});
    in_queue_[x->id()][a] = true;
  }
}

bool MSAC3bit::ShouldContinueProbe(int current_level) const {
  // 检查深度限制
  if (config_.depth_limit >= 0 && current_level > config_.depth_limit) {
    return false;
  }

  // 检查 probe 次数限制
  if (config_.max_probes >= 0 && stats_.num_probes >= config_.max_probes) {
    return false;
  }

  // 检查时间限制
  if (config_.max_time_ms >= 0 &&
      stats_.probe_time_ms >= config_.max_time_ms) {
    return false;
  }

  return true;
}

// ============================================================================
// Phase 2.1: Probe 缓存方法实现
// ============================================================================

void MSAC3bit::ResetCache() {
  for (auto& var_cache : probe_cache_) {
    for (auto& cache : var_cache) {
      cache.trail_pos = -1;
    }
  }
}

bool MSAC3bit::IsCacheValid(IntVar* x, int a) {
  // 检查索引边界
  if (x->id() >= static_cast<int>(probe_cache_.size()) ||
      a >= static_cast<int>(probe_cache_[x->id()].size())) {
    return false;
  }

  auto& cache = probe_cache_[x->id()][a];
  if (cache.trail_pos < 0) return false;  // 未缓存

  // 检查从上次成功到现在，邻域变量是否有变化
  const int trail_end = m_->trail()->Size();
  for (int i = cache.trail_pos; i < trail_end; ++i) {
    const auto& entry = m_->trail()->GetEntry(i);
    int var_id = entry.var_id;
    if (var_id >= 0 && var_id < static_cast<int>(m_->vars.size())) {
      // 检查是否是 x 的邻域变量
      if (IsNeighbor(x, m_->vars[var_id])) {
        return false;  // 邻域变化，缓存失效
      }
    }
  }
  return true;  // 邻域未变，缓存有效
}

void MSAC3bit::UpdateCache(IntVar* x, int a) {
  if (x->id() < static_cast<int>(probe_cache_.size()) &&
      a < static_cast<int>(probe_cache_[x->id()].size())) {
    probe_cache_[x->id()][a].trail_pos = m_->trail()->Size();
  }
}

bool MSAC3bit::IsNeighbor(IntVar* x, IntVar* y) {
  if (x == y) return true;  // 自己也算邻居（自己的域变了需要重新 probe）

  // 使用 Network::subscription 检查是否共享约束
  auto it = m_->subscription.find(x);
  if (it == m_->subscription.end()) return false;

  for (auto* c : it->second) {
    for (auto* v : c->scope) {
      if (v == y) return true;
    }
  }
  return false;
}

// ============================================================================
// Phase 2.2: 快速支持检测
// ============================================================================

bool MSAC3bit::QuickCheckSupport(IntVar* x, int a) {
  // 检查值 (x, a) 在所有约束中是否有支持
  // 使用 AC3bit 的 bitSup_ 快速检查，无需完整 probe
  //
  // 返回值:
  //   true  - 所有约束都有支持，可能是 SAC-consistent（需要完整 probe 确认）
  //   false - 至少一个约束无支持，必定不是 SAC-consistent（可直接删除）

  auto it = m_->subscription.find(x);
  if (it == m_->subscription.end()) {
    // 变量不参与任何约束，trivially consistent
    return true;
  }

  for (auto* c : it->second) {
    // 创建 IntConVal 表示 (约束 c, 变量 x, 值 a)
    IntConVal cv(c, x, a);

    // 使用 AC3bit kernel 的 seek_support 检查是否有支持
    if (!kernel_->seek_support(cv, level_)) {
      VLOG(2) << "[MSAC3bit] QuickCheck: var=" << x->id() << ", val=" << a
              << " has no support in constraint " << c->id();
      return false;  // 无支持，快速拒绝
    }
  }

  return true;  // 所有约束都有支持
}

// ============================================================================
// Phase 3: SAC-SDS 方法实现
// ============================================================================

std::pair<int, int> MSAC3bit::GetBitIdx(int value) const {
  return {value / BITSIZE, value % BITSIZE};
}

int MSAC3bit::CountSupportsFromBitSup(const IntConVal& cv) {
  const int idx = m_->GetIntConValIndex(cv);
  int count = 0;

  const auto& bitSup = kernel_->GetBitSup();

  // 对约束中的其他变量
  for (IntVar* y : cv.c()->scope) {
    if (y != cv.v()) {
      // 统计 bitSup[idx] 与 y->bitDom() 重叠的位数
      for (int i = 0; i < y->bitDom().size(); ++i) {
        count += (bitSup[idx][i] & y->bitDom()[i]).count();
      }
    }
  }

  return count;
}

void MSAC3bit::InitializeSupportCountsOptimized() {
  // 计算所需大小
  int max_index = 0;
  for (auto* c : m_->tabs) {
    for (auto* x : c->scope) {
      int idx = m_->GetIntConValIndex(IntConVal(c, x, x->capacity()));
      max_index = std::max(max_index, idx);
    }
  }

  support_count_.assign(max_index + 1, 0);

  VLOG(1) << "[SAC-SDS] Initializing support counts (size="
          << support_count_.size() << ")";

  // 对每个约束中的每个值，使用 bitSup_ 快速计数支持
  for (auto* c : m_->tabs) {
    for (auto* x : c->scope) {
      for (int a = x->head(); a != Limits::INDEX_OVERFLOW; a = x->next(a)) {
        IntConVal cv(c, x, a);
        int idx = m_->GetIntConValIndex(cv);

        if (idx < static_cast<int>(support_count_.size())) {
          support_count_[idx] = CountSupportsFromBitSup(cv);
        }
      }
    }
  }

  VLOG(1) << "[SAC-SDS] Support counts initialized";
}

void MSAC3bit::InitializeZeroSupportQueue() {
  pending_queue_.clear();
  in_queue_.resize(m_->vars.size());
  for (auto* x : m_->vars) {
    in_queue_[x->id()].assign(x->capacity() + 1, false);
  }

  // 首次调用：入队所有剩余值（类似 SAC1）
  // 理由：AC 支持数不等于 SAC 支持数，首次需要全检查
  // 后续通过 UpdateSupportsAfterRemoval 维护真正的 SAC 支持计数
  for (auto* x : m_->vars) {
    if (x->assigned()) continue;
    for (int a = x->head(); a != Limits::INDEX_OVERFLOW; a = x->next(a)) {
      EnqueueValue(x, a);
      ++stats_.total_zero_detections;
    }
  }

  VLOG(1) << "[SAC-SDS] Initial queue (first pass): "
          << pending_queue_.size() << " values";
}

void MSAC3bit::UpdateSupportsAfterRemoval(IntVar* x, int a) {
  auto it = m_->subscription.find(x);
  if (it == m_->subscription.end()) return;

  const auto& bitSup = kernel_->GetBitSup();
  int num_updated = 0;
  int num_new_zeros = 0;

  for (auto* c : it->second) {
    // 对于约束 c 中的其他变量 y
    for (auto* y : c->scope) {
      if (y == x) continue;

      // 检查 y 的每个值是否依赖 (x, a)
      for (int b = y->head(); b != Limits::INDEX_OVERFLOW; b = y->next(b)) {
        IntConVal cv_y(c, y, b);
        int idx_y = m_->GetIntConValIndex(cv_y);

        if (idx_y >= static_cast<int>(support_count_.size())) continue;

        // 检查 (y, b) 的支持集中是否包含 (x, a)
        auto [word, bit] = GetBitIdx(a);

        if (word < static_cast<int>(bitSup[idx_y].size()) &&
            bitSup[idx_y][word].test(bit)) {
          // (y, b) 依赖 (x, a)，递减支持数
          --support_count_[idx_y];
          ++num_updated;
          ++stats_.total_support_updates;

          // 支持数降为0，入队待检测
          if (support_count_[idx_y] == 0) {
            EnqueueValue(y, b);
            ++num_new_zeros;
            ++stats_.total_zero_detections;
            VLOG(2) << "[SAC-SDS] New zero-support: var=" << y->id()
                    << ", val=" << b;
          }
        }
      }
    }
  }

  VLOG(2) << "[SAC-SDS] Updated " << num_updated << " supports, "
          << num_new_zeros << " new zeros";
}

}  // namespace cpim
