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

class CudaTimer {
public:
    CudaTimer() {
        cudaEventCreate(&start);
        cudaEventCreate(&stop);
        reset();
    }

    ~CudaTimer() {
        cudaEventDestroy(start);
        cudaEventDestroy(stop);
    }

    void reset() {
        cudaEventRecord(start, 0);
    }

    // 默认输出毫秒
    float elapsed() {
        cudaEventRecord(stop, 0);
        cudaEventSynchronize(stop);
        float milliseconds = 0.0f;
        cudaEventElapsedTime(&milliseconds, start, stop);
        cudaEventRecord(start, 0); // 重新设置start以便下次调用elapsed可以记录新的时间段
        return milliseconds;
    }

    // 微秒
    float elapsed_micro() {
        return elapsed() * 1000.0f;
    }

    // 纳秒
    float elapsed_nano() {
        return elapsed_micro() * 1000.0f;
    }

    // 秒
    float elapsed_seconds() {
        return elapsed() / 1000.0f;
    }

private:
    cudaEvent_t start, stop;
};

}
