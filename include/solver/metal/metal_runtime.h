#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "absl/status/statusor.h"

namespace cpim::solver::metal {

class MetalRuntime;

class MetalBuffer {
 public:
  MetalBuffer();
  ~MetalBuffer();

  MetalBuffer(MetalBuffer&&) noexcept;
  MetalBuffer& operator=(MetalBuffer&&) noexcept;

  MetalBuffer(const MetalBuffer&) = delete;
  MetalBuffer& operator=(const MetalBuffer&) = delete;

  void* contents();
  const void* contents() const;
  size_t size_bytes() const;
  const std::string& label() const;
  bool valid() const;

 private:
  friend class MetalRuntime;

  struct Impl;
  explicit MetalBuffer(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

class MetalPipeline {
 public:
  MetalPipeline();
  ~MetalPipeline();

  MetalPipeline(MetalPipeline&&) noexcept;
  MetalPipeline& operator=(MetalPipeline&&) noexcept;

  MetalPipeline(const MetalPipeline&) = delete;
  MetalPipeline& operator=(const MetalPipeline&) = delete;

  size_t max_threads_per_threadgroup() const;
  bool valid() const;

 private:
  friend class MetalRuntime;

  struct Impl;
  explicit MetalPipeline(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

struct MetalBufferBinding {
  int index = 0;
  const MetalBuffer* buffer = nullptr;
};

struct MetalBufferCopy {
  const MetalBuffer* source = nullptr;
  const MetalBuffer* destination = nullptr;
  size_t size_bytes = 0;
};

struct MetalBufferFill {
  const MetalBuffer* buffer = nullptr;
  size_t size_bytes = 0;
  uint8_t value = 0;
};

struct MetalComputeDispatch1D {
  const MetalPipeline* pipeline = nullptr;
  std::vector<MetalBufferBinding> bindings;
  std::vector<uint8_t> params;
  int params_index = 0;
  uint64_t grid_size = 0;
  uint64_t threads_per_threadgroup = 0;
};

struct MetalDispatchTimings {
  double encode_ms = 0.0;
  double wall_ms = 0.0;
  double kernel_ms = 0.0;
  bool gpu_timing_available = false;
};

class MetalRuntime {
 public:
  MetalRuntime();
  ~MetalRuntime();

  MetalRuntime(MetalRuntime&&) noexcept;
  MetalRuntime& operator=(MetalRuntime&&) noexcept;

  MetalRuntime(const MetalRuntime&) = delete;
  MetalRuntime& operator=(const MetalRuntime&) = delete;

  static absl::StatusOr<MetalRuntime> CreateDefault();

  const std::string& device_name() const;

  absl::StatusOr<MetalPipeline> LoadComputePipeline(
      const std::string& metallib_path, const std::string& function_name) const;
  absl::StatusOr<MetalBuffer> NewSharedBuffer(size_t size_bytes,
                                              const std::string& label) const;
  absl::StatusOr<MetalBuffer> NewSharedBufferWithBytes(
      const void* data, size_t size_bytes, const std::string& label) const;
  absl::StatusOr<MetalBuffer> NewPrivateBufferWithBytes(
      const void* data, size_t size_bytes, const std::string& label) const;

  absl::StatusOr<MetalDispatchTimings> Dispatch1D(
      const MetalPipeline& pipeline,
      const std::vector<MetalBufferBinding>& bindings,
      const void* params,
      size_t params_size,
      int params_index,
      uint64_t grid_size,
      uint64_t threads_per_threadgroup) const;

  absl::StatusOr<MetalDispatchTimings> Dispatch1DBatch(
      const std::vector<MetalComputeDispatch1D>& dispatches) const;

  absl::StatusOr<MetalDispatchTimings> BlitCopyAndFill(
      const std::vector<MetalBufferCopy>& copies,
      const std::vector<MetalBufferFill>& fills) const;

 private:
  struct Impl;
  explicit MetalRuntime(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;
};

}  // namespace cpim::solver::metal
