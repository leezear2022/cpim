#include <thrust/extrema.h>
#include <thrust/functional.h>

#include <algorithm>
#include <cfloat>
#include <iostream>
#include <vector>

#include "cuSAC.cuh"
#include <glog/logging.h>
#include "model/cmodel_adapter.h"
#include "xcsp3model/HModel.h"

namespace cpim {

__managed__ DeviceStats g_device_stats = {0ULL, 0U};
// #define RATO_MAX __FLT_MAX__ - 1;
// 初始化常量
__constant__ int kDeviceBitDomIntSize;
__constant__ int kDeviceMaxDomSize;
__constant__ int kDeviceBitDomsIntSize;
__constant__ int kDeviceNumVars;
__constant__ int kDeviceNumTabs;
__constant__ int* kDeviceDomSize;

__constant__ int kDeviceBitSupIntSize;
__constant__ int kDeviceBitSupsIntSize;
__constant__ int kDeviceBitSubDomsIntSize;
__constant__ inline int kDeg[kMaxNumVars];
// 全局传播是否成功，默认值是true
__managed__ int GAC_success = true;

// TODO: 这里未来可以改成纯device内存

__managed__ u32* bitSubDom;

__managed__ int* S_VarPre;
__managed__ uint3* S_Var;
__managed__ int* S_ConPre;
__managed__ int3* S_ConEvt;
__managed__ int3* S_Con;

// // 刚刚用于赋值的变量值设备上的
// __device__ int assigned_var;
// __device__ int assigned_val;

// 赋值栈
// __device__ int2* assigned;
// 栈定标记
// 标记赋值堆栈,及栈顶
// i32x2* h_assigned;
i32x2* d_assigned;
__device__ int d_assigned_size = 0;

int intsizeof(const int nbits) {
  return ((nbits + kBitsPerWord - 1) / kBitsPerWord);
}

// ============================================================================
// Stub GPU kernels and helper functions (minimal implementations for cpim_dump)
// ============================================================================

// Predicate for filtering
struct is_one {
  __host__ __device__ bool operator()(const int x) const { return x == 1; }
};

// Helper function for device bitdom index calculation
__device__ inline int DeviceGetBitDomByIndex(int x, int i) {
  return x * kDeviceBitDomIntSize + i;
}

// Stub kernels - minimal implementations since cpim_dump doesn't solve
__global__ void CsCheckMain(i32* mConPre, const u32x3* mCon, u32* bitDom,
                            const i32* dom_size, cudaTextureObject_t bitSup,
                            cudaTextureObject_t neiCon, int num_ConEvt, int level) {
  // Stub implementation - not used by cpim_dump
}

__global__ void CsCheckMainAfterDecision(
    i32* mConPre, const u32x3* mCon, const u32x3* subscriptions,
    const int* subscription_offset, u32* bitDom, const i32* dom_size,
    cudaTextureObject_t bitSup, cudaTextureObject_t neiCon, int num_ConEvt,
    i32x2* assigned, int varid, int type, int level) {
  // Stub implementation - not used by cpim_dump
}

__global__ void AssignValue(i32x2* assigned, u32* bitDom, i32* dom_size,
                           int varid, int level) {
  // Stub implementation - not used by cpim_dump
}

__global__ void RemoveValue(i32x2* assigned, u32* bitDom, i32* dom_size,
                           int varid, int level) {
  // Stub implementation - not used by cpim_dump
}

int CModel::compress_Main() {
  d_MConEvt.resize(d_MCon.size());
  auto end = thrust::copy_if(d_MCon.begin(), d_MCon.end(),
                             d_ConPre.begin(),
                             d_MConEvt.begin(),
                             is_one());
  thrust::fill(d_ConPre.begin(), d_ConPre.end(), 0);
  d_MConEvt.resize(thrust::distance(d_MConEvt.begin(), end));
  return static_cast<int>(d_MConEvt.size());
}

int CModel::heuristic() {
  // Stub - not used by cpim_dump
  return kNumVars;
}

int CModel::CreateNewLevel() {
  current_level_++;

  // 计算源地址和目标地址
  u32* src = d_bitDom + (current_level_ - 1) * kBitDomsIntSize;
  u32* dst = d_bitDom + current_level_ * kBitDomsIntSize;

  // 计算需要复制的字节数
  size_t copySize = kBitDomsIntSize * sizeof(u32);
  GPU_PRINTF("xixi~ at level %d\n", current_level_);
  bitDomCopy();
  // 使用 cudaMemcpy 进行复制操作
  CUDA_CHECK(cudaMemcpy(dst, src, copySize, cudaMemcpyDeviceToDevice));
  GPU_PRINTF("xixi2~ at level %d\n", current_level_);

  bitDomCopy();
  // 复制 d_cur_dom_size 的新一层值
  thrust::copy(d_cur_dom_size.begin() + (current_level_ - 1) * kNumVars,
               d_cur_dom_size.begin() + current_level_ * kNumVars,
               d_cur_dom_size.begin() + current_level_ * kNumVars);

  // 返回当前级别
  return current_level_;
}

void CModel::initialGPUConstant() {
  std::cout << "[DEBUG] initialGPUConstant: DISABLED for debugging" << std::endl;
  return;

  // std::cout << "[DEBUG] initialGPUConstant: h_Deg.size()=" << h_Deg.size()
  //           << " kNumVars=" << kNumVars << " bytes=" << (kNumVars * sizeof(int)) << std::endl;
  //
  // CUDA_CHECK(cudaMemcpyToSymbol(kDeviceBitDomIntSize, &kBitDomIntSize, sizeof(int)));
  // CUDA_CHECK(cudaMemcpyToSymbol(kDeviceBitDomsIntSize, &kBitDomsIntSize, sizeof(int)));
  // CUDA_CHECK(cudaMemcpyToSymbol(kDeviceNumTabs, &kNumTabs, sizeof(int)));
  // CUDA_CHECK(cudaMemcpyToSymbol(kDeviceNumVars, &kNumVars, sizeof(int)));
  // CUDA_CHECK(cudaMemcpyToSymbol(kDeviceMaxDomSize, &kMaxDomSize, sizeof(int)));
  // CUDA_CHECK(cudaMemcpyToSymbol(kDeviceBitSupIntSize, &kBitSupIntSize, sizeof(int)));
  // CUDA_CHECK(cudaMemcpyToSymbol(kDeviceBitSupsIntSize, &kBitSupsIntSize, sizeof(int)));
  // CUDA_CHECK(cudaMemcpyToSymbol(kDeviceBitSubDomsIntSize, &kBitSubDomsIntSize,
  //                               sizeof(int)));
}

bool CModel::enforceGAC() {
  GPU_PRINTF("-----------enforeGAC-----------\n");
  g_device_stats.deletions = 0;
  g_device_stats.gac_iterations = 0;
  unsigned int host_iterations = 0;
  int num_ConEvt = compress_Main();
  GPU_DEBUG_SYNC();
  while (num_ConEvt != 0) {
    ++host_iterations;
    GPU_PRINTF("-----------iteration-----------\n");
    CsCheckMain<<<num_ConEvt, dim3(kBitDomIntSize * 32, 1, 1),
                  kSharedMemSize>>>(
        thrust::raw_pointer_cast(d_ConPre.data()),
        thrust::raw_pointer_cast(d_MCon.data()), d_bitDom,
        thrust::raw_pointer_cast(d_cur_dom_size.data()), texObj_BitSup,
        texObj_MCon, num_ConEvt, current_level_);
    GPU_DEBUG_SYNC();
    if (!GAC_success) {
      return false;
    }
    GPU_PRINTF("-----------end iteration-----------\n");
    // std::cout << "h_ConPre: ";
    // thrust::host_vector<int> h_ConPre = d_ConPre;
    // for (size_t i = 0; i < h_ConPre.size(); ++i) {
    //   std::cout << h_ConPre[i] << " ";
    // }
    // std::cout << std::endl;

    num_ConEvt = compress_Main();
    // return true;
  }

  g_device_stats.gac_iterations = host_iterations;

  CUDA_CHECK(cudaDeviceSynchronize());
  GPU_PRINTF("device stats: deletions=%llu iterations=%u\n",
             g_device_stats.deletions, g_device_stats.gac_iterations);

  return true;
}

bool CModel::enforceGAC(int var, int type) {
  GPU_PRINTF("-----------enforeGAC for disicion-----------\n");
  int num_ConEvt = h_subscription_offset[var + 1] - h_subscription_offset[var];
  // 填充最后一个标记位
  h_subscription_offset[kNumVars] = h_subscription.size();
  GPU_PRINTF("h_subscription_offset: ");
#ifdef CPIM_GPU_DEBUG
  for (auto i : h_subscription_offset) {
    std::cout << i << " ";
  }
  std::cout << std::endl;
#endif

  for (int i = h_subscription_offset[var]; i < h_subscription_offset[var + 1];
       ++i) {
    int c_id = h_subscription[i].z;
    int v1_id = h_subscription[i].x;
    int v2_id = h_subscription[i].y;
    GPU_PRINTF("c_id = %d, v1_id = %d, v2_id = %d\n", c_id, v1_id, v2_id);
  }

  GPU_PRINTF("num_ConEvt = %d\n", num_ConEvt);
  // return false;
  CsCheckMainAfterDecision<<<num_ConEvt, dim3(kBitDomIntSize * 32, 1, 1),
                             kSharedMemSize>>>(
      thrust::raw_pointer_cast(d_ConPre.data()),
      thrust::raw_pointer_cast(d_MCon.data()),
      thrust::raw_pointer_cast(d_subscription.data()),
      thrust::raw_pointer_cast(d_subscription_offset.data()), d_bitDom,
      thrust::raw_pointer_cast(d_cur_dom_size.data()), texObj_BitSup,
      texObj_MCon, num_ConEvt, d_assigned, var, type, current_level_);
  GPU_DEBUG_SYNC();
  h_cur_dom_size = d_cur_dom_size;
  GPU_PRINTF("h_cur_dom_size at level %d:\n", current_level_);
  for (size_t i = 0; i < kNumVars; i++) {
    int j = i + kNumVars * current_level_;
    GPU_PRINTF("%d ", h_cur_dom_size[j]);
  }
  GPU_PRINTF("\n");
  GPU_PRINTF("GAC_success = %d\n", GAC_success);
  // return false;
  num_ConEvt = compress_Main();
  GPU_PRINTF("num_ConEvt: %d\n", num_ConEvt);
  GPU_DEBUG_SYNC();
  while (num_ConEvt != 0) {
    GPU_PRINTF("-----------iteration-----------\n");
    CsCheckMain<<<num_ConEvt, dim3(kBitDomIntSize * 32, 1, 1),
                  kSharedMemSize>>>(
        thrust::raw_pointer_cast(d_ConPre.data()),
        thrust::raw_pointer_cast(d_MCon.data()), d_bitDom,
        thrust::raw_pointer_cast(d_cur_dom_size.data()), texObj_BitSup,
        texObj_MCon, num_ConEvt, current_level_);
    GPU_DEBUG_SYNC();
    if (!GAC_success) {
      return false;
    }
    GPU_PRINTF("-----------end iteration-----------\n");
    // std::cout << "h_ConPre: ";
    // thrust::host_vector<int> h_ConPre = d_ConPre;
    // for (size_t i = 0; i < h_ConPre.size(); ++i) {
    //   std::cout << h_ConPre[i] << " ";
    // }
    // std::cout << std::endl;

    num_ConEvt = compress_Main();
    // return true;
  }

  CUDA_CHECK(cudaDeviceSynchronize());

  return true;
}

//
//
// bool CModel::enforceGAC() {
//   GPU_PRINTF("-------------------enforceGAC-------------------\n");
//   int sharedMemSize =
//       (2 * kBitDomIntSize + 1) * sizeof(u32);  // 动态共享内存大小
//   // // GPU_PRINTF("d_ConPre.size = %lu\n", d_ConPre.size());
//   // GAC_success = 1;
//   // // // 1. 压缩约束
//   // int num_ConEvt = compress_Main();
//   // while (num_ConEvt) {
//   //   // // 2. 检查约束
//   //   // // if(kBitDomIntSize==3||kBitDomIntSize==4) {
//   //   // //
//   //   //
//   //
//   CsCheckMain<<<num_ConEvt,dim3(kBitDomIntSize*32,1,1)>>>(thrust::raw_pointer_cast(d_ConPre.data()),thrust::raw_pointer_cast(d_MCon.data()),bitDom,thrust::raw_pointer_cast(dom_size.data()),texObj_BitSup);
//   //   // // }else if(kBitDomIntSize>=5||kBitDomIntSize<=8){
//   //   // //
//   //   //
//   //
//   CsCheckMain<<<num_ConEvt,dim3(kBitDomIntSize*32,1,1)>>>(thrust::raw_pointer_cast(d_ConPre.data()),thrust::raw_pointer_cast(d_MCon.data()),bitDom,thrust::raw_pointer_cast(dom_size.data()),texObj_BitSup);
//   //   // // }
//   //   // // // CsCheckMain<<<num_ConEvt,kBitDomIntSize*32>>>
//   //   // // 将 d_MConEvt 数据从设备复制到主机
//   //   thrust::host_vector<uint3> h_MConEvt = d_MConEvt;
//   //   // // // 还原回去
//   //   // //
//   //   // // // 在主机上打印数据
//   //   std::cout<<"-------------------"<<std::endl;
//   //   for (size_t i = 0; i < num_ConEvt; ++i) {
//   //
//   //     std::cout << "h_MConEvt[" << i << "] = ("
//   //               << h_MConEvt[i].x << ", "
//   //               << h_MConEvt[i].y << ", "
//   //               << h_MConEvt[i].z << ")\n";
//   //   }
//   //   CsCheckMain<<<num_ConEvt,dim3(kBitDomIntSize*32,1,1),sharedMemSize>>>(
//   //     thrust::raw_pointer_cast(d_ConPre.data()),
//   //     thrust::raw_pointer_cast(d_MCon.data()),
//   //     bitDom,
//   //     thrust::raw_pointer_cast(dom_size.data()),
//   //     texObj_BitSup,texObj_MCon,
//   //     num_ConEvt);
//   //   num_ConEvt = compress_Main();
//   // }
//
//
//   int num_ConEvt = compress_Main();
//   while (num_ConEvt != 0) {
//     CsCheckMain<<<num_ConEvt, dim3(kBitDomIntSize * 32, 1, 1),
//     kSharedMemSize>>>(
//         thrust::raw_pointer_cast(d_ConPre.data()),
//         thrust::raw_pointer_cast(d_MCon.data()), bitDom,
//         thrust::raw_pointer_cast(dom_size.data()), texObj_BitSup,
//         texObj_MCon, num_ConEvt);
//     if (GAC_success) {
//       return false;
//     }
//     num_ConEvt = compress_Main();
//   }
//   return true;
// }

void CModel::enforceSAC() {}

void CModel::bitDomCopy() {
  CUDA_CHECK(cudaMemcpy(h_bitDom, d_bitDom, sizeof(u32) * kAllBitDomsIntSize,
             cudaMemcpyDeviceToHost));
  GPU_PRINTF("bitdom: \n");
  for (int i = 0; i < kAllBitDomsIntSize; i++) {
    // 将32位整数转换为二进制字符串
    std::string binary_str = "";
    for (int j = 31; j >= 0; j--) {
      binary_str += ((h_bitDom[i] >> j) & 1) ? '1' : '0';
      // 每4位添加一个空格
      if (j % 4 == 0 && j != 0) {
        binary_str += " ";
      }
    }
    // 打印格式化后的二进制字符串
    GPU_PRINTF("%s\n", binary_str.c_str());
  }
  GPU_PRINTF("\n");
}

SearchStatistics CModel::solve(const float time_limit) {
  CudaTimer t;
  bool finished_ = false;
  enforceGAC();
  if (!GAC_success) {
    statistics_.solve_time = t.elapsed();
    return statistics_;
  }

  while (!finished_) {
    if (t.elapsed() > time_limit) {
      statistics_.solve_time = t.elapsed();
      statistics_.time_out = true;
      return statistics_;
    }

    // 还没新层的时候先调用启发式
    auto varid = heuristic();
    GPU_PRINTF("varid = %d\n", varid);
    if (varid == kNumVars) {
      finished_ = true;
      statistics_.solve_time = t.elapsed();
      return statistics_;
    }

    GPU_PRINTF("before assign value at level %d\n", current_level_);
    bitDomCopy();
    GPU_DEBUG_SYNC();
    CreateNewLevel();
    GPU_PRINTF("after assign2 value at level %d\n", current_level_);
    GPU_DEBUG_SYNC();
    bitDomCopy();
    GPU_DEBUG_SYNC();
    AssignValue<<<1, kBitDomIntSize * 32>>>(
        d_assigned, d_bitDom, thrust::raw_pointer_cast(d_cur_dom_size.data()),
        varid, current_level_);
    GPU_DEBUG_SYNC();
    enforceGAC(varid, 1);
    GPU_PRINTF("after assign value at level %d\n", current_level_);
    GPU_DEBUG_SYNC();
    bitDomCopy();
    // return statistics_;

    while (!GAC_success && current_level_ != 0) {
      BackLevel();
      RemoveValue<<<1, kBitDomIntSize * 32>>>(
          d_assigned, d_bitDom, thrust::raw_pointer_cast(d_cur_dom_size.data()),
          varid, current_level_);
      ++statistics_.num_negative;
      enforceGAC(varid, 0);
    }
    if (!GAC_success) finished_ = true;
  }

  statistics_.solve_time = t.elapsed();
  return statistics_;
}

CModel::~CModel() {
  std::cout << "[DEBUG] CModel destructor" << std::endl;
  // cudaFree(d_bitSup);
  if (d_ConNeighbor) cudaFree(d_ConNeighbor);
  if (d_bitDom) cudaFree(d_bitDom);
  if (h_bitDom) free(h_bitDom);
  // cudaFree(M_VarPre);
  // cudaFree(d_current_domain_size);
  // free(h_current_domain_size);
  if (d_assigned) cudaFree(d_assigned);
  if (d_solution) cudaFree(d_solution);
  // cudaFree(d_assigned_at_level);

  if (bitSubDom) cudaFree(bitSubDom);
  // cudaFree(bitSup);
  // cudaFree(M_Con);
  // cudaFree(M_ConEvt);
  // cudaFree(M_ConPre);
  if (S_ConPre) cudaFree(S_ConPre);
  if (S_ConEvt) cudaFree(S_ConEvt);
  if (S_Con) cudaFree(S_Con);
  if (S_Var) cudaFree(S_Var);
  if (S_VarPre) cudaFree(S_VarPre);

  // 析构纹理内存
  if (texObj_MCon) cudaDestroyTextureObject(texObj_MCon);
  if (cuArray_MCon) cudaFreeArray(cuArray_MCon);

  if (texObj_BitSup) cudaDestroyTextureObject(texObj_BitSup);
  if (cuArray3D) cudaFreeArray(cuArray3D);

  std::cout << "[DEBUG] CModel destructor complete" << std::endl;
  // free(h_Deg);
}

void CModel::DelGPUModel() const {
  // cudaFree(scope);
}

// ============================================================================
// CModel 构造函数和模型构建逻辑（原 cuSAC_host.cu 内容）
// ============================================================================

CModel::CModel(const HModel& xm)
    : CModel(model::CModelAdapter::FromHModel(xm)) {}

CModel::CModel(model::CModelAdapter adapter)
    : kNumVars(adapter.num_vars()),
      kNumTabs(adapter.num_tabs()),
      kDepth(adapter.num_vars() + 1),
      kMaxDomSize(adapter.max_dom_size()),
      kBitDomIntSize(adapter.bit_dom_int_size()),
      kBitDomsIntSize(kBitDomIntSize * kNumVars),
      kAllBitDomsIntSize(kBitDomsIntSize * kDepth),
      kBitSupIntSize(kMaxDomSize * kBitDomIntSize),
      kBitSupsIntSize(kBitSupIntSize * kNumTabs),
      kBitSubDomsIntSize(kNumVars * kMaxDomSize * kBitDomsIntSize),
      kSharedMemSize((2 * kBitDomIntSize) * sizeof(u32)) {
  g_device_stats.deletions = 0;
  g_device_stats.gac_iterations = 0;

  std::cout << "[DEBUG] CModel constructor start" << std::endl;
  std::cout << "  kNumVars=" << kNumVars
            << " kNumTabs=" << kNumTabs
            << " kMaxDomSize=" << kMaxDomSize << std::endl;
  std::cout << "  kDepth=" << kDepth
            << " kBitDomIntSize=" << kBitDomIntSize << std::endl;
  std::cout << "  kBitDomsIntSize=" << kBitDomsIntSize
            << " kAllBitDomsIntSize=" << kAllBitDomsIntSize << std::endl;
  std::cout << "  kBitSupIntSize=" << kBitSupIntSize
            << " kBitSupsIntSize=" << kBitSupsIntSize << std::endl;
  std::cout << "  kBitSubDomsIntSize=" << kBitSubDomsIntSize << std::endl;

  CHECK_LE(kNumVars, kMaxNumVars)
      << "Number of variables exceeds the maximum limit of " << kMaxNumVars;

  std::cout << "[DEBUG] Assigning domain sizes..." << std::endl;
  h_dom_size.assign(adapter.domain_sizes().begin(), adapter.domain_sizes().end());
  std::cout << "[DEBUG] Domain sizes assigned: " << h_dom_size.size() << std::endl;

  std::cout << "[DEBUG] Assigning degrees..." << std::endl;
  h_Deg.assign(adapter.degrees().begin(), adapter.degrees().end());
  std::cout << "[DEBUG] Degrees assigned: " << h_Deg.size() << std::endl;

  std::cout << "[DEBUG] Skipping d_Deg and d_ratio for now..." << std::endl;
  // d_Deg = h_Deg;
  // d_ratio.resize(kNumVars + 1);
  // d_ratio[kNumVars] = FLT_MAX;
  std::cout << "[DEBUG] Thrust operations skipped" << std::endl;

  std::cout << "[DEBUG] Initializing GPU constants..." << std::endl;
  initialGPUConstant();

  std::cout << "[DEBUG] Building from adapter..." << std::endl;
  BuildFromAdapter(adapter);

  std::cout << "[DEBUG] CModel constructor complete!" << std::endl;
}

void CModel::BuildFromAdapter(const model::CModelAdapter& adapter) {
  std::cout << "[DEBUG] BuildFromAdapter: Creating h_ConNeighbor (size="
            << (kNumTabs * kNumVars) << ")..." << std::endl;

  // Test malloc first
  void* test_ptr = malloc(1024);
  std::cout << "[DEBUG] Test malloc returned: " << test_ptr << std::endl;
  if (test_ptr) free(test_ptr);

  std::vector<int> h_ConNeighbor(kNumTabs * kNumVars, 0);
  std::cout << "[DEBUG] h_ConNeighbor vector created" << std::endl;

  std::cout << "[DEBUG] BuildFromAdapter: Processing subscriptions (count="
            << adapter.subscriptions().entries.size() << ")..." << std::endl;
  for (const auto& entry : adapter.subscriptions().entries) {
    const int cid = static_cast<int>(entry.z);
    const int x = static_cast<int>(entry.x);
    const int y = static_cast<int>(entry.y);
    if (cid >= kNumTabs) continue;
    if (x < kNumVars) h_ConNeighbor[x * kNumVars + cid] = 1;
    if (y < kNumVars) h_ConNeighbor[y * kNumVars + cid] = 1;
  }

  std::cout << "[DEBUG] BuildFromAdapter: Testing CUDA device..." << std::endl;
  int deviceCount = 0;
  cudaError_t err = cudaGetDeviceCount(&deviceCount);
  std::cout << "[DEBUG] cudaGetDeviceCount: " << cudaGetErrorString(err)
            << ", count=" << deviceCount << std::endl;

  std::cout << "[DEBUG] BuildFromAdapter: Allocating d_ConNeighbor (bytes="
            << (sizeof(int) * kNumTabs * kNumVars) << ")..." << std::endl;

  // Try a tiny allocation first
  void* tiny_ptr = nullptr;
  err = cudaMalloc(&tiny_ptr, 4);
  std::cout << "[DEBUG] Test cudaMalloc(4): " << cudaGetErrorString(err) << std::endl;
  if (tiny_ptr) cudaFree(tiny_ptr);

  CUDA_CHECK(cudaMalloc(&d_ConNeighbor, sizeof(int) * kNumTabs * kNumVars));
  std::cout << "[DEBUG] BuildFromAdapter: Copying to d_ConNeighbor..." << std::endl;
  CUDA_CHECK(cudaMemcpy(d_ConNeighbor, h_ConNeighbor.data(),
                        sizeof(int) * kNumTabs * kNumVars,
                        cudaMemcpyHostToDevice));

  std::cout << "[DEBUG] BuildFromAdapter: Creating texture for MCon..." << std::endl;
  cudaChannelFormatDesc channelDesc = cudaCreateChannelDesc<int>();
  CUDA_CHECK(cudaMallocArray(&cuArray_MCon, &channelDesc, kNumTabs, kNumVars));
  const size_t spitch = kNumTabs * sizeof(int);
  CUDA_CHECK(cudaMemcpy2DToArray(cuArray_MCon, 0, 0, h_ConNeighbor.data(), spitch,
                                 kNumTabs * sizeof(int), kNumVars,
                                 cudaMemcpyHostToDevice));

  memset(&resDesc_MCon, 0, sizeof(resDesc_MCon));
  resDesc_MCon.resType = cudaResourceTypeArray;
  resDesc_MCon.res.array.array = cuArray_MCon;
  memset(&texDesc_MCon, 0, sizeof(texDesc_MCon));
  texDesc_MCon.addressMode[0] = cudaAddressModeClamp;
  texDesc_MCon.addressMode[1] = cudaAddressModeClamp;
  texDesc_MCon.filterMode = cudaFilterModePoint;
  texDesc_MCon.readMode = cudaReadModeElementType;
  texDesc_MCon.normalizedCoords = 0;
  CUDA_CHECK(cudaCreateTextureObject(&texObj_MCon, &resDesc_MCon, &texDesc_MCon,
                                     nullptr));

  std::cout << "[DEBUG] BuildFromAdapter: Allocating h_bitDom (size="
            << kAllBitDomsIntSize << ")..." << std::endl;
  h_bitDom = static_cast<u32*>(malloc(sizeof(u32) * kAllBitDomsIntSize));
  if (!h_bitDom) {
    throw std::runtime_error("Failed to allocate h_bitDom");
  }
  std::cout << "[DEBUG] BuildFromAdapter: Filling h_bitDom..." << std::endl;
  std::fill(h_bitDom, h_bitDom + kAllBitDomsIntSize, 0u);
  const auto& domain_words = adapter.domains().data;
  for (int i = 0; i < kNumVars; ++i) {
    for (int j = 0; j < kBitDomIntSize; ++j) {
      h_bitDom[i * kBitDomIntSize + j] = domain_words[i * kBitDomIntSize + j];
    }
  }

  std::cout << "[DEBUG] BuildFromAdapter: Setting up h_cur_dom_size..." << std::endl;
  h_cur_dom_size.assign(kNumVars * kDepth, 0);
  for (int i = 0; i < kNumVars; ++i) {
    h_cur_dom_size[i] = h_dom_size[i];
  }
  d_cur_dom_size = h_cur_dom_size;

  std::cout << "[DEBUG] BuildFromAdapter: Allocating d_bitDom..." << std::endl;
  CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&d_bitDom),
                        sizeof(u32) * kAllBitDomsIntSize));
  CUDA_CHECK(cudaMemcpy(d_bitDom, h_bitDom,
                        sizeof(u32) * kAllBitDomsIntSize,
                        cudaMemcpyHostToDevice));

  std::cout << "[DEBUG] BuildFromAdapter: Allocating bitSubDom (managed, size="
            << (kBitDomsIntSize * kNumVars * kMaxDomSize) << ")..." << std::endl;
  CUDA_CHECK(cudaMallocManaged(&bitSubDom,
                               sizeof(u32) * kBitDomsIntSize * kNumVars *
                                   kMaxDomSize));
  for (int i = 0; i < kNumVars; ++i) {
    for (int j = 0; j < kMaxDomSize; ++j) {
      const int start_idx = GetBitSubDomStartIndex(i, j);
      for (int k = 0; k < kBitSubDomsIntSize; ++k) {
        bitSubDom[start_idx + k] = h_bitDom[k];
      }
      const int iji_start_idx = start_idx + i * kBitDomIntSize;
      for (int k = 0; k < kBitDomIntSize; ++k) bitSubDom[iji_start_idx + k] = 0;
      BITSET_SET((bitSubDom + iji_start_idx), j);
    }
  }

  cudaChannelFormatDesc channelDesc3D = cudaCreateChannelDesc<uint2>();
  cudaExtent extent = make_cudaExtent(kMaxDomSize, kBitDomIntSize, kNumTabs);
  CUDA_CHECK(cudaMalloc3DArray(&cuArray3D, &channelDesc3D, extent));

  cudaMemcpy3DParms copyParams = {0};
  copyParams.srcPtr =
      make_cudaPitchedPtr(const_cast<uint2*>(adapter.supports().data.data()),
                          kMaxDomSize * sizeof(uint2),
                          kMaxDomSize, kBitDomIntSize);
  copyParams.dstArray = cuArray3D;
  copyParams.extent = extent;
  copyParams.kind = cudaMemcpyHostToDevice;
  CUDA_CHECK(cudaMemcpy3D(&copyParams));

  memset(&resDesc3D, 0, sizeof(resDesc3D));
  resDesc3D.resType = cudaResourceTypeArray;
  resDesc3D.res.array.array = cuArray3D;
  memset(&texDesc3D, 0, sizeof(texDesc3D));
  texDesc3D.addressMode[0] = cudaAddressModeClamp;
  texDesc3D.addressMode[1] = cudaAddressModeClamp;
  texDesc3D.addressMode[2] = cudaAddressModeClamp;
  texDesc3D.filterMode = cudaFilterModePoint;
  texDesc3D.readMode = cudaReadModeElementType;
  texDesc3D.normalizedCoords = 0;
  CUDA_CHECK(cudaCreateTextureObject(&texObj_BitSup, &resDesc3D, &texDesc3D,
                                     nullptr));

  thrust::host_vector<uint3> h_MCon(adapter.constraints().begin(),
                                    adapter.constraints().end());
  d_MCon = h_MCon;
  d_MConEvt.clear();
  d_MConEvt.reserve(kNumTabs);
  d_ConPre.assign(kNumTabs, 1);

  h_subscription_offset.assign(adapter.subscriptions().offsets.begin(),
                               adapter.subscriptions().offsets.end());
  h_subscription.assign(adapter.subscriptions().entries.begin(),
                        adapter.subscriptions().entries.end());
  d_subscription = h_subscription;
  d_subscription_offset = h_subscription_offset;
}

void CModel::BuildBitModel(const HModel& xm) {
  BuildFromAdapter(model::CModelAdapter::FromHModel(xm));
}

}  // namespace cpim
