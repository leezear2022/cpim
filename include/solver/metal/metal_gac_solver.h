#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "model/device_layout.h"

namespace cpim::solver::metal {

enum class MetalReadonlyStorageMode {
  kShared,
  kPrivate,
};

enum class MetalFrontierMode {
  kFlags,
  kCompact,
  kWorklist,
  kAuto,
};

enum class MetalRunnerMode {
  kCold,
  kPrepared,
};

enum class MetalKernelVariant {
  kScalar,
  kWordParallel,
  kSimdgroup,
  kAuto,
};

enum class MetalBitSupLayout {
  kPair,
  kDirectional,
  kAuto,
};

enum class MetalResetMode {
  kCpu,
  kBlit,
  kAuto,
};

struct MetalGacOptions {
  std::string metallib_path;
  int max_iterations = 10000;
  bool verbose = false;
  MetalRunnerMode runner_mode = MetalRunnerMode::kCold;
  MetalReadonlyStorageMode readonly_storage = MetalReadonlyStorageMode::kShared;
  MetalFrontierMode frontier_mode = MetalFrontierMode::kFlags;
  MetalKernelVariant kernel_variant = MetalKernelVariant::kScalar;
  MetalBitSupLayout bitsup_layout = MetalBitSupLayout::kPair;
  MetalResetMode reset_mode = MetalResetMode::kCpu;
};

struct MetalGacStats {
  int iterations = 0;
  int deletions = 0;
  int dispatch_count = 0;
  bool inconsistent = false;
  bool budget_exceeded = false;
  double elapsed_ms = 0.0;
  double solve_ms = 0.0;
  double setup_ms = 0.0;
  double prepare_ms = 0.0;
  double reset_ms = 0.0;
  double reset_dispatch_ms = 0.0;
  double dispatch_ms = 0.0;
  double kernel_ms = 0.0;
  int active_constraints_total = 0;
  int worklist_push_count = 0;
  int worklist_rounds = 0;
  int worklist_epoch_resets = 0;
  double frontier_density_avg = 0.0;
  bool gpu_timing_available = false;
  std::string device_name;
  std::string runner_mode;
  std::string readonly_storage;
  std::string frontier_mode;
  std::string kernel_variant;
  std::string bitsup_layout;
  std::string reset_mode;
  std::string effective_frontier_mode;
  std::string effective_kernel_variant;
  std::string effective_bitsup_layout;
  std::string variant_name;
};

class MetalGacSolver {
 public:
  struct Impl;

  MetalGacSolver(const cpim::model::DeviceModelLayout& layout,
                 MetalGacOptions options);
  ~MetalGacSolver();

  MetalGacSolver(MetalGacSolver&&) noexcept;
  MetalGacSolver& operator=(MetalGacSolver&&) noexcept;

  MetalGacSolver(const MetalGacSolver&) = delete;
  MetalGacSolver& operator=(const MetalGacSolver&) = delete;

  absl::StatusOr<MetalGacStats> Run();

  const std::vector<uint32_t>& bit_dom() const;
  const std::vector<int32_t>& domain_sizes() const;

 private:
  std::unique_ptr<Impl> impl_;
};

class MetalPreparedGacRunner {
 public:
  MetalPreparedGacRunner(const cpim::model::DeviceModelLayout& layout,
                         MetalGacOptions options);
  ~MetalPreparedGacRunner();

  MetalPreparedGacRunner(MetalPreparedGacRunner&&) noexcept;
  MetalPreparedGacRunner& operator=(MetalPreparedGacRunner&&) noexcept;

  MetalPreparedGacRunner(const MetalPreparedGacRunner&) = delete;
  MetalPreparedGacRunner& operator=(const MetalPreparedGacRunner&) = delete;

  absl::Status Prepare();
  absl::StatusOr<MetalGacStats> Run();

  const std::vector<uint32_t>& bit_dom() const;
  const std::vector<int32_t>& domain_sizes() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace cpim::solver::metal
