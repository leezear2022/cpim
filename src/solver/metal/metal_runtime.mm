#include "solver/metal/metal_runtime.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "absl/status/status.h"
#include "absl/strings/str_format.h"

namespace cpim::solver::metal {
namespace {

std::string NSErrorToString(NSError* error) {
  if (error == nil) {
    return "unknown Metal error";
  }
  NSString* description = [error localizedDescription];
  if (description == nil) {
    return "unknown Metal error";
  }
  const char* raw = [description UTF8String];
  return raw == nullptr ? "unknown Metal error" : std::string(raw);
}

std::string NSStringToString(NSString* value) {
  if (value == nil) {
    return "";
  }
  const char* raw = [value UTF8String];
  return raw == nullptr ? "" : std::string(raw);
}

NSUInteger SafeBufferLength(size_t size_bytes) {
  return static_cast<NSUInteger>(std::max<size_t>(size_bytes, 4));
}

}  // namespace

struct MetalBuffer::Impl {
  id<MTLBuffer> buffer = nil;
  size_t size_bytes = 0;
  std::string label;
};

MetalBuffer::MetalBuffer() = default;
MetalBuffer::MetalBuffer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
MetalBuffer::~MetalBuffer() = default;
MetalBuffer::MetalBuffer(MetalBuffer&&) noexcept = default;
MetalBuffer& MetalBuffer::operator=(MetalBuffer&&) noexcept = default;

void* MetalBuffer::contents() {
  return impl_ == nullptr || impl_->buffer == nil ? nullptr : [impl_->buffer contents];
}

const void* MetalBuffer::contents() const {
  return impl_ == nullptr || impl_->buffer == nil ? nullptr : [impl_->buffer contents];
}

size_t MetalBuffer::size_bytes() const {
  return impl_ == nullptr ? 0 : impl_->size_bytes;
}

const std::string& MetalBuffer::label() const {
  static const std::string kEmpty;
  return impl_ == nullptr ? kEmpty : impl_->label;
}

bool MetalBuffer::valid() const {
  return impl_ != nullptr && impl_->buffer != nil;
}

struct MetalPipeline::Impl {
  id<MTLLibrary> library = nil;
  id<MTLComputePipelineState> pipeline = nil;
};

MetalPipeline::MetalPipeline() = default;
MetalPipeline::MetalPipeline(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
MetalPipeline::~MetalPipeline() = default;
MetalPipeline::MetalPipeline(MetalPipeline&&) noexcept = default;
MetalPipeline& MetalPipeline::operator=(MetalPipeline&&) noexcept = default;

size_t MetalPipeline::max_threads_per_threadgroup() const {
  if (impl_ == nullptr || impl_->pipeline == nil) {
    return 0;
  }
  return static_cast<size_t>(impl_->pipeline.maxTotalThreadsPerThreadgroup);
}

bool MetalPipeline::valid() const {
  return impl_ != nullptr && impl_->pipeline != nil;
}

struct MetalRuntime::Impl {
  id<MTLDevice> device = nil;
  id<MTLCommandQueue> queue = nil;
  std::string device_name;
};

MetalRuntime::MetalRuntime() = default;
MetalRuntime::MetalRuntime(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
MetalRuntime::~MetalRuntime() = default;
MetalRuntime::MetalRuntime(MetalRuntime&&) noexcept = default;
MetalRuntime& MetalRuntime::operator=(MetalRuntime&&) noexcept = default;

absl::StatusOr<MetalRuntime> MetalRuntime::CreateDefault() {
  @autoreleasepool {
    auto impl = std::make_unique<Impl>();
    impl->device = MTLCreateSystemDefaultDevice();
    if (impl->device == nil) {
      return absl::FailedPreconditionError("No Metal device is available");
    }
    impl->device_name = NSStringToString([impl->device name]);
    if (impl->device_name.empty()) {
      impl->device_name = "unknown Metal device";
    }
    impl->queue = [impl->device newCommandQueue];
    if (impl->queue == nil) {
      return absl::InternalError("Failed to create Metal command queue");
    }
    return MetalRuntime(std::move(impl));
  }
}

const std::string& MetalRuntime::device_name() const {
  static const std::string kEmpty;
  return impl_ == nullptr ? kEmpty : impl_->device_name;
}

absl::StatusOr<MetalPipeline> MetalRuntime::LoadComputePipeline(
    const std::string& metallib_path, const std::string& function_name) const {
  @autoreleasepool {
    if (impl_ == nullptr || impl_->device == nil) {
      return absl::FailedPreconditionError("Metal runtime is not initialized");
    }
    if (metallib_path.empty()) {
      return absl::InvalidArgumentError("Metal pipeline requires a metallib path");
    }

    NSString* path = [NSString stringWithUTF8String:metallib_path.c_str()];
    NSURL* url = [NSURL fileURLWithPath:path];
    NSError* error = nil;
    id<MTLLibrary> library = [impl_->device newLibraryWithURL:url error:&error];
    if (library == nil) {
      return absl::InternalError(absl::StrFormat(
          "Failed to load metallib '%s': %s", metallib_path, NSErrorToString(error)));
    }

    NSString* function_name_ns =
        [NSString stringWithUTF8String:function_name.c_str()];
    id<MTLFunction> function = [library newFunctionWithName:function_name_ns];
    if (function == nil) {
      return absl::InternalError(absl::StrFormat(
          "Failed to find Metal function '%s'", function_name));
    }

    id<MTLComputePipelineState> pipeline =
        [impl_->device newComputePipelineStateWithFunction:function error:&error];
    if (pipeline == nil) {
      return absl::InternalError(absl::StrFormat(
          "Failed to create Metal compute pipeline: %s", NSErrorToString(error)));
    }

    auto pipeline_impl = std::make_unique<MetalPipeline::Impl>();
    pipeline_impl->library = library;
    pipeline_impl->pipeline = pipeline;
    return MetalPipeline(std::move(pipeline_impl));
  }
}

absl::StatusOr<MetalBuffer> MetalRuntime::NewSharedBuffer(
    size_t size_bytes, const std::string& label) const {
  @autoreleasepool {
    if (impl_ == nullptr || impl_->device == nil) {
      return absl::FailedPreconditionError("Metal runtime is not initialized");
    }

    const NSUInteger length = SafeBufferLength(size_bytes);
    id<MTLBuffer> buffer =
        [impl_->device newBufferWithLength:length
                                   options:MTLResourceStorageModeShared];
    if (buffer == nil) {
      return absl::ResourceExhaustedError(absl::StrFormat(
          "Failed to allocate Metal shared buffer '%s' (%zu bytes)",
          label, size_bytes));
    }

    std::memset([buffer contents], 0, length);
    NSString* label_ns = [NSString stringWithUTF8String:label.c_str()];
    [buffer setLabel:label_ns];

    auto buffer_impl = std::make_unique<MetalBuffer::Impl>();
    buffer_impl->buffer = buffer;
    buffer_impl->size_bytes = size_bytes;
    buffer_impl->label = label;
    return MetalBuffer(std::move(buffer_impl));
  }
}

absl::StatusOr<MetalBuffer> MetalRuntime::NewSharedBufferWithBytes(
    const void* data, size_t size_bytes, const std::string& label) const {
  auto buffer_or = NewSharedBuffer(size_bytes, label);
  if (!buffer_or.ok()) {
    return buffer_or.status();
  }
  if (data != nullptr && size_bytes > 0) {
    std::memcpy(buffer_or->contents(), data, size_bytes);
  }
  return std::move(*buffer_or);
}

absl::StatusOr<MetalBuffer> MetalRuntime::NewPrivateBufferWithBytes(
    const void* data, size_t size_bytes, const std::string& label) const {
  @autoreleasepool {
    if (impl_ == nullptr || impl_->device == nil || impl_->queue == nil) {
      return absl::FailedPreconditionError("Metal runtime is not initialized");
    }

    const NSUInteger length = SafeBufferLength(size_bytes);
    id<MTLBuffer> private_buffer =
        [impl_->device newBufferWithLength:length
                                   options:MTLResourceStorageModePrivate];
    if (private_buffer == nil) {
      return absl::ResourceExhaustedError(absl::StrFormat(
          "Failed to allocate Metal private buffer '%s' (%zu bytes)",
          label, size_bytes));
    }
    NSString* label_ns = [NSString stringWithUTF8String:label.c_str()];
    [private_buffer setLabel:label_ns];

    if (data != nullptr && size_bytes > 0) {
      id<MTLBuffer> staging =
          [impl_->device newBufferWithBytes:data
                                     length:static_cast<NSUInteger>(size_bytes)
                                    options:MTLResourceStorageModeShared];
      if (staging == nil) {
        return absl::ResourceExhaustedError(absl::StrFormat(
            "Failed to allocate Metal private staging buffer '%s' (%zu bytes)",
            label, size_bytes));
      }

      id<MTLCommandBuffer> command_buffer = [impl_->queue commandBuffer];
      if (command_buffer == nil) {
        return absl::InternalError("Failed to create Metal blit command buffer");
      }
      id<MTLBlitCommandEncoder> encoder = [command_buffer blitCommandEncoder];
      if (encoder == nil) {
        return absl::InternalError("Failed to create Metal blit encoder");
      }
      [encoder copyFromBuffer:staging
                 sourceOffset:0
                     toBuffer:private_buffer
            destinationOffset:0
                         size:static_cast<NSUInteger>(size_bytes)];
      [encoder endEncoding];
      [command_buffer commit];
      [command_buffer waitUntilCompleted];

      if ([command_buffer status] == MTLCommandBufferStatusError) {
        return absl::InternalError(absl::StrFormat(
            "Metal private buffer upload failed: %s",
            NSErrorToString([command_buffer error])));
      }
    }

    auto buffer_impl = std::make_unique<MetalBuffer::Impl>();
    buffer_impl->buffer = private_buffer;
    buffer_impl->size_bytes = size_bytes;
    buffer_impl->label = label;
    return MetalBuffer(std::move(buffer_impl));
  }
}

absl::StatusOr<MetalDispatchTimings> MetalRuntime::Dispatch1D(
    const MetalPipeline& pipeline,
    const std::vector<MetalBufferBinding>& bindings,
    const void* params,
    size_t params_size,
    int params_index,
    uint64_t grid_size,
    uint64_t threads_per_threadgroup) const {
  @autoreleasepool {
    if (impl_ == nullptr || impl_->queue == nil) {
      return absl::FailedPreconditionError("Metal runtime is not initialized");
    }
    if (!pipeline.valid()) {
      return absl::InvalidArgumentError("Metal dispatch requires a valid pipeline");
    }
    if (grid_size == 0 || threads_per_threadgroup == 0) {
      return absl::InvalidArgumentError("Metal dispatch requires non-zero grid size");
    }

    const auto encode_start = std::chrono::steady_clock::now();
    id<MTLCommandBuffer> command_buffer = [impl_->queue commandBuffer];
    if (command_buffer == nil) {
      return absl::InternalError("Failed to create Metal command buffer");
    }

    id<MTLComputeCommandEncoder> encoder = [command_buffer computeCommandEncoder];
    if (encoder == nil) {
      return absl::InternalError("Failed to create Metal command encoder");
    }

    [encoder setComputePipelineState:pipeline.impl_->pipeline];
    for (const auto& binding : bindings) {
      if (binding.buffer == nullptr || !binding.buffer->valid()) {
        return absl::InvalidArgumentError(
            absl::StrFormat("Invalid Metal buffer binding at index %d",
                            binding.index));
      }
      [encoder setBuffer:binding.buffer->impl_->buffer offset:0 atIndex:binding.index];
    }
    if (params != nullptr && params_size > 0) {
      [encoder setBytes:params length:params_size atIndex:params_index];
    }

    const MTLSize grid = MTLSizeMake(static_cast<NSUInteger>(grid_size), 1, 1);
    const MTLSize group =
        MTLSizeMake(static_cast<NSUInteger>(threads_per_threadgroup), 1, 1);
    [encoder dispatchThreads:grid threadsPerThreadgroup:group];
    [encoder endEncoding];
    const auto encode_end = std::chrono::steady_clock::now();

    const auto start = std::chrono::steady_clock::now();
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    const auto end = std::chrono::steady_clock::now();

    if ([command_buffer status] == MTLCommandBufferStatusError) {
      return absl::InternalError(absl::StrFormat(
          "Metal command buffer failed: %s", NSErrorToString([command_buffer error])));
    }

    MetalDispatchTimings timings;
    timings.encode_ms =
        std::chrono::duration<double, std::milli>(encode_end - encode_start)
            .count();
    timings.wall_ms =
        std::chrono::duration<double, std::milli>(end - start).count();
    const double gpu_start = [command_buffer GPUStartTime];
    const double gpu_end = [command_buffer GPUEndTime];
    if (gpu_start > 0.0 && gpu_end >= gpu_start) {
      timings.kernel_ms = (gpu_end - gpu_start) * 1000.0;
      timings.gpu_timing_available = true;
    }
    return timings;
  }
}

absl::StatusOr<MetalDispatchTimings> MetalRuntime::Dispatch1DBatch(
    const std::vector<MetalComputeDispatch1D>& dispatches) const {
  @autoreleasepool {
    if (impl_ == nullptr || impl_->queue == nil) {
      return absl::FailedPreconditionError("Metal runtime is not initialized");
    }
    if (dispatches.empty()) {
      return MetalDispatchTimings();
    }

    const auto encode_start = std::chrono::steady_clock::now();
    id<MTLCommandBuffer> command_buffer = [impl_->queue commandBuffer];
    if (command_buffer == nil) {
      return absl::InternalError("Failed to create Metal batch command buffer");
    }

    for (const MetalComputeDispatch1D& dispatch : dispatches) {
      if (dispatch.grid_size == 0) {
        continue;
      }
      if (dispatch.pipeline == nullptr || !dispatch.pipeline->valid()) {
        return absl::InvalidArgumentError(
            "Metal batch dispatch requires a valid pipeline");
      }
      if (dispatch.threads_per_threadgroup == 0) {
        return absl::InvalidArgumentError(
            "Metal batch dispatch requires non-zero threadgroup size");
      }

      id<MTLComputeCommandEncoder> encoder =
          [command_buffer computeCommandEncoder];
      if (encoder == nil) {
        return absl::InternalError("Failed to create Metal batch encoder");
      }

      [encoder setComputePipelineState:dispatch.pipeline->impl_->pipeline];
      for (const auto& binding : dispatch.bindings) {
        if (binding.buffer == nullptr || !binding.buffer->valid()) {
          return absl::InvalidArgumentError(absl::StrFormat(
              "Invalid Metal batch buffer binding at index %d", binding.index));
        }
        [encoder setBuffer:binding.buffer->impl_->buffer
                    offset:0
                   atIndex:binding.index];
      }
      if (!dispatch.params.empty()) {
        [encoder setBytes:dispatch.params.data()
                   length:dispatch.params.size()
                  atIndex:dispatch.params_index];
      }

      const MTLSize grid =
          MTLSizeMake(static_cast<NSUInteger>(dispatch.grid_size), 1, 1);
      const MTLSize group = MTLSizeMake(
          static_cast<NSUInteger>(dispatch.threads_per_threadgroup), 1, 1);
      [encoder dispatchThreads:grid threadsPerThreadgroup:group];
      [encoder endEncoding];
    }
    const auto encode_end = std::chrono::steady_clock::now();

    const auto start = std::chrono::steady_clock::now();
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    const auto end = std::chrono::steady_clock::now();

    if ([command_buffer status] == MTLCommandBufferStatusError) {
      return absl::InternalError(absl::StrFormat(
          "Metal batch command buffer failed: %s",
          NSErrorToString([command_buffer error])));
    }

    MetalDispatchTimings timings;
    timings.encode_ms =
        std::chrono::duration<double, std::milli>(encode_end - encode_start)
            .count();
    timings.wall_ms =
        std::chrono::duration<double, std::milli>(end - start).count();
    const double gpu_start = [command_buffer GPUStartTime];
    const double gpu_end = [command_buffer GPUEndTime];
    if (gpu_start > 0.0 && gpu_end >= gpu_start) {
      timings.kernel_ms = (gpu_end - gpu_start) * 1000.0;
      timings.gpu_timing_available = true;
    }
    return timings;
  }
}

absl::StatusOr<MetalDispatchTimings> MetalRuntime::BlitCopyAndFill(
    const std::vector<MetalBufferCopy>& copies,
    const std::vector<MetalBufferFill>& fills) const {
  @autoreleasepool {
    if (impl_ == nullptr || impl_->queue == nil) {
      return absl::FailedPreconditionError("Metal runtime is not initialized");
    }
    if (copies.empty() && fills.empty()) {
      return MetalDispatchTimings();
    }

    const auto encode_start = std::chrono::steady_clock::now();
    id<MTLCommandBuffer> command_buffer = [impl_->queue commandBuffer];
    if (command_buffer == nil) {
      return absl::InternalError("Failed to create Metal blit command buffer");
    }

    id<MTLBlitCommandEncoder> encoder = [command_buffer blitCommandEncoder];
    if (encoder == nil) {
      return absl::InternalError("Failed to create Metal blit encoder");
    }

    for (const auto& copy : copies) {
      if (copy.size_bytes == 0) {
        continue;
      }
      if (copy.source == nullptr || copy.destination == nullptr ||
          !copy.source->valid() || !copy.destination->valid()) {
        return absl::InvalidArgumentError(
            "Metal blit copy requires valid source and destination buffers");
      }
      if (copy.size_bytes > copy.source->size_bytes() ||
          copy.size_bytes > copy.destination->size_bytes()) {
        return absl::InvalidArgumentError(absl::StrFormat(
            "Metal blit copy size %zu exceeds buffer sizes (%zu -> %zu)",
            copy.size_bytes, copy.source->size_bytes(),
            copy.destination->size_bytes()));
      }
      [encoder copyFromBuffer:copy.source->impl_->buffer
                 sourceOffset:0
                     toBuffer:copy.destination->impl_->buffer
            destinationOffset:0
                         size:static_cast<NSUInteger>(copy.size_bytes)];
    }

    for (const auto& fill : fills) {
      if (fill.size_bytes == 0) {
        continue;
      }
      if (fill.buffer == nullptr || !fill.buffer->valid()) {
        return absl::InvalidArgumentError(
            "Metal blit fill requires a valid buffer");
      }
      if (fill.size_bytes > fill.buffer->size_bytes()) {
        return absl::InvalidArgumentError(absl::StrFormat(
            "Metal blit fill size %zu exceeds buffer size %zu",
            fill.size_bytes, fill.buffer->size_bytes()));
      }
      [encoder fillBuffer:fill.buffer->impl_->buffer
                    range:NSMakeRange(0, static_cast<NSUInteger>(fill.size_bytes))
                    value:fill.value];
    }

    [encoder endEncoding];
    const auto encode_end = std::chrono::steady_clock::now();

    const auto start = std::chrono::steady_clock::now();
    [command_buffer commit];
    [command_buffer waitUntilCompleted];
    const auto end = std::chrono::steady_clock::now();

    if ([command_buffer status] == MTLCommandBufferStatusError) {
      return absl::InternalError(absl::StrFormat(
          "Metal blit command buffer failed: %s",
          NSErrorToString([command_buffer error])));
    }

    MetalDispatchTimings timings;
    timings.encode_ms =
        std::chrono::duration<double, std::milli>(encode_end - encode_start)
            .count();
    timings.wall_ms =
        std::chrono::duration<double, std::milli>(end - start).count();
    const double gpu_start = [command_buffer GPUStartTime];
    const double gpu_end = [command_buffer GPUEndTime];
    if (gpu_start > 0.0 && gpu_end >= gpu_start) {
      timings.kernel_ms = (gpu_end - gpu_start) * 1000.0;
      timings.gpu_timing_available = true;
    }
    return timings;
  }
}

}  // namespace cpim::solver::metal
