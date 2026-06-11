#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "model/device_layout.h"

namespace cpim::solver::metal {

enum class MetalSacProbeStatus {
  kOk = 0,
  kDwo = 1,
  kUnknown = 2,
};

enum class MetalSacActivationMode {
  kNeighbor,
  kFull,
};

enum class MetalSacMode {
  kBatchProbe,
  kNsacq,
  kSacqAdj,
  kSacqFull,
};

enum class MetalSacProbeFusion {
  kNone,
  kBounded,
};

struct MetalSacProbeTask {
  int32_t var_id = -1;
  int32_t value = -1;
  int32_t task_id = -1;
};

struct MetalSacBudget {
  int max_probe_rounds = 10000;
};

struct MetalBatchProbeOptions {
  std::string metallib_path;
  MetalSacActivationMode activation_mode = MetalSacActivationMode::kNeighbor;
  MetalSacBudget budget;
  std::vector<int32_t> allowed_constraints;
  int threads_per_threadgroup = 256;
  bool collect_world_domains = false;
  MetalSacProbeFusion probe_fusion = MetalSacProbeFusion::kNone;
  int fusion_rounds = 4;
};

struct MetalBatchProbeStats {
  int probes = 0;
  int ok_count = 0;
  int dwo_count = 0;
  int unknown_count = 0;
  int rounds = 0;
  int dispatch_count = 0;
  int command_buffer_count = 0;
  int active_frontier_total = 0;
  int allowed_constraints_count = 0;
  int fusion_rounds = 0;
  int fused_rounds_encoded = 0;
  int fused_rounds_wasted = 0;
  bool budget_exceeded = false;
  double elapsed_ms = 0.0;
  double dispatch_ms = 0.0;
  double dispatch_encode_ms = 0.0;
  double dispatch_wait_ms = 0.0;
  double dispatch_non_kernel_ms = 0.0;
  double kernel_ms = 0.0;
  double probes_per_sec = 0.0;
  double dispatch_per_probe = 0.0;
  double command_buffer_per_probe = 0.0;
  double non_kernel_per_probe = 0.0;
  bool gpu_timing_available = false;
  std::string device_name;
};

class MetalBatchProbeRunner {
 public:
  struct Impl;

  MetalBatchProbeRunner(const cpim::model::DeviceModelLayout& layout,
                        const std::vector<uint32_t>& snapshot_bit_dom,
                        const std::vector<int32_t>& snapshot_domain_sizes,
                        std::vector<MetalSacProbeTask> tasks,
                        MetalBatchProbeOptions options);
  ~MetalBatchProbeRunner();

  MetalBatchProbeRunner(MetalBatchProbeRunner&&) noexcept;
  MetalBatchProbeRunner& operator=(MetalBatchProbeRunner&&) noexcept;

  MetalBatchProbeRunner(const MetalBatchProbeRunner&) = delete;
  MetalBatchProbeRunner& operator=(const MetalBatchProbeRunner&) = delete;

  absl::StatusOr<MetalBatchProbeStats> Run();

  const std::vector<MetalSacProbeStatus>& statuses() const;
  const std::vector<uint32_t>& world_bit_dom() const;
  const std::vector<int32_t>& world_domain_sizes() const;
  const std::vector<int32_t>& dwo_debug_words() const;

 private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace cpim::solver::metal
