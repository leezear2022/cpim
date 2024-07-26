/*
 * Timer.h
 *
 *  Created on: 2017年9月27日
 *      Author: leezear
 */

#pragma once
#include <chrono>
#include <cuda_runtime.h>
using namespace std;
using namespace chrono;

namespace cpim {
class Timer {
 public:
  Timer() : m_begin(high_resolution_clock::now()) {}
  void reset() { m_begin = high_resolution_clock::now(); }

  //	//默认输出秒
  //	double elapsed() const {
  //		return
  // duration_cast<duration<double>>(high_resolution_clock::now() -
  // m_begin).count();
  //	}

  // 默认输出毫秒
  int64_t elapsed() const {
    return duration_cast<milliseconds>(high_resolution_clock::now() - m_begin)
        .count();
  }

  // 微秒
  int64_t elapsed_micro() const {
    return duration_cast<microseconds>(high_resolution_clock::now() - m_begin)
        .count();
  }

  // 纳秒
  int64_t elapsed_nano() const {
    return duration_cast<nanoseconds>(high_resolution_clock::now() - m_begin)
        .count();
  }

  // 秒
  int64_t elapsed_seconds() const {
    return duration_cast<seconds>(high_resolution_clock::now() - m_begin)
        .count();
  }

  ////分
  // int64_t elapsed_minutes() const {
  //	return duration_cast<minutes>(high_resolution_clock::now() -
  // m_begin).count();
  // }

  ////时
  // int64_t elapsed_hours() const {
  //	return duration_cast<hours>(high_resolution_clock::now() -
  // m_begin).count();
  // }

 private:
  time_point<high_resolution_clock> m_begin;
};



// CUDA error checking macro
#define CUDA_CHECK(call)                                            \
  {                                                                 \
    const cudaError_t error = call;                                 \
    if (error != cudaSuccess) {                                     \
      std::cerr << "Error: " << __FILE__ << ":" << __LINE__ << ", " \
                << cudaGetErrorString(error) << std::endl;          \
      exit(1);                                                      \
    }                                                               \
  }

class CudaTimer {
 public:
  CudaTimer() {
    CUDA_CHECK(cudaEventCreate(&startEvent));
    CUDA_CHECK(cudaEventCreate(&stopEvent));
  }

  ~CudaTimer() {
    CUDA_CHECK(cudaEventDestroy(startEvent));
    CUDA_CHECK(cudaEventDestroy(stopEvent));
  }

  void start() { CUDA_CHECK(cudaEventRecord(startEvent, 0)); }

  void stop() {
    CUDA_CHECK(cudaEventRecord(stopEvent, 0));
    CUDA_CHECK(cudaEventSynchronize(stopEvent));
  }

  float elapsedMilliseconds() {
    float milliseconds = 0.0f;
    CUDA_CHECK(cudaEventElapsedTime(&milliseconds, startEvent, stopEvent));
    return milliseconds;
  }

  float elapsedSeconds() { return elapsedMilliseconds() / 1000.0f; }

  float elapsedMicroseconds() { return elapsedMilliseconds() * 1000.0f; }

  float elapsedNanoseconds() { return elapsedMilliseconds() * 1000000.0f; }

 private:
  cudaEvent_t startEvent, stopEvent;
};

}
