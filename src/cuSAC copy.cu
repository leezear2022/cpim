#include <glog/logging.h>
#include <thrust/extrema.h>
#include <thrust/functional.h>

#include <cfloat>

#include "cuSAC.cuh"

namespace cpim {
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

////////////////////////////////  CModel  ////////////////////////////////

CModel::CModel(const HModel& xm)
    : kNumVars(xm->Vars().size()),
      kNumTabs(xm->Tabs().size()),
      kDepth(xm->Vars().size() + 1),
      kMaxDomSize(xm->max_domain_size()),
      kBitDomIntSize(intsizeof(xm->max_domain_size())),
      kBitDomsIntSize(kBitDomIntSize * kNumVars),
      kAllBitDomsIntSize(kBitDomsIntSize * kDepth),
      kBitSupIntSize(kMaxDomSize * kBitDomIntSize),
      kBitSupsIntSize(kMaxDomSize * kBitDomIntSize * kNumTabs),
      kBitSubDomsIntSize(kNumVars * kMaxDomSize * kBitDomsIntSize),
      kSharedMemSize((2 * kBitDomIntSize) * sizeof(u32)) {
  printf("===============build===============\n");
  // 检查问题不要大于kMaxNumVars
  CHECK_LE(kNumVars, kMaxNumVars)
      << "Number of variables exceeds the maximum limit of " << kMaxNumVars;
  // 初始化GPU常量
  initialGPUConstant();

  h_dom_size.resize(kNumVars);
  for (int i = 0; i < kNumVars; ++i) {
    const HVar v = xm->Vars(i);
    printf("%d\n", int(v->vals.size()));
    h_dom_size[i] = int(v->vals.size());
  }
  printf("h_Deg: ");
  // 生成Deg
  h_Deg.resize(kNumVars);
  for (int i = 0; i < kNumVars; ++i) {
    const auto v = xm->Vars(i);
    h_Deg[i] = xm->subscriptions[v].size();
    printf("%d ", h_Deg[i]);
  }
  printf("\n");

  d_Deg = h_Deg;
  d_ratio.resize(kNumVars + 1);
  d_ratio[kNumVars] = FLT_MAX;

  // 初始化GPU数据
  BuildBitModel(xm);
}

__global__ void exampleKernel(uint3* MCon, int size) {
  int idx = threadIdx.x + blockIdx.x * blockDim.x;
  if (idx < size) {
    uint3 val = MCon[idx];
    printf("d_MCon[%d] = (%u, %u, %u)\n", idx, val.x, val.y, val.z);
  }
}

// Simple transformation kernel
__global__ void transformKernel(cudaTextureObject_t texObj, int width,
                                int height) {
  int c = threadIdx.x;
  int x = blockIdx.x;
  if (c < width) {
    // 访问纹理内存
    int value = tex2D<int>(texObj, c, x);
    // 打印值
    printf("Texture value at cid = %d, vid = %d: %d\n", c, x, value);
  }
}

// CUDA kernel function to read from 3D texture object
// width -> dom
// height-> bit
// depth -> c
__global__ void transformKernel3D(cudaTextureObject_t texObj3D, int width,
                                  int height, int depth) {
  int x = blockIdx.x * blockDim.x + threadIdx.x;
  int y = blockIdx.y * blockDim.y + threadIdx.y;
  int z = blockIdx.z * blockDim.z + threadIdx.z;

  if (x < width && y < height && z < depth) {
    // 访问3D纹理内存
    auto value3D = tex3D<uint2>(texObj3D, x, y, z);
    printf("Texture3D value at (%d, %d, %d): (%x, %x)\n", z, x, y, value3D.x,
           value3D.y);
  }
}

__inline__ __device__ int DeviceGetBitDomByValue(const int x, const int a) {
  return x * kDeviceBitDomIntSize + a & U32_MOD_MASK;
}

// 通过值(x,ith)拿到(x,ith)所在的word
__inline__ __device__ int DeviceGetBitDomByIndex(const int x, const int i) {
  return x * kDeviceBitDomIntSize + i;
}
// struct calculate_ratio {
//   __host__ __device__ float operator()(const thrust::tuple<int, int>& t)
//   const {
//     int dom_size = thrust::get<0>(t);
//     int deg = thrust::get<1>(t);
//     return (dom_size != 1) ? static_cast<float>(dom_size) / deg : FLT_MAX;
//   }
// };
struct calculate_ratio {
  __host__ __device__ float operator()(int dom_size, int deg) const {
    return (dom_size != 1) ? static_cast<float>(dom_size) / deg : FLT_MAX - 1;
  }
};

// 寻找dom/deg最小变量
int CModel::heuristic() {
  // 创建一个用于存储比值的设备向量
  thrust::transform(d_cur_dom_size.begin() + current_level_ * kNumVars,
                    d_cur_dom_size.begin() + (current_level_ + 1) * kNumVars,
                    d_Deg.begin(), d_ratio.begin(), calculate_ratio());

  h_cur_dom_size = d_cur_dom_size;
  thrust::host_vector<float> h_ratio = d_ratio;
  for (int i = 0; i < h_ratio.size(); ++i) {
    printf("h_ratio[%d] = %d, %d, %f\n", i,
           h_cur_dom_size[current_level_ * kNumVars + i], h_Deg[i], h_ratio[i]);
  }
  printf("\n");

  // 找到最小比值及其索引
  thrust::device_vector<float>::iterator min_element_iter =
      thrust::min_element(d_ratio.begin(), d_ratio.end());
  int min_index = min_element_iter - d_ratio.begin();

  return min_index;
}
//
// // cuda版本的
// __global__ void calculateRatiosAndFindMinIndex(const int* d_cur_dom_size,
//                                                int deg, u32* bitDom,
//                                                i32* assigned_at_level,
//                                                float* d_ratio, int*
//                                                d_min_index, int kNumVars, int
//                                                level) {
//   extern __shared__ float shared_data[];
//   int tid = threadIdx.x;
//   int idx = level * kNumVars + blockIdx.x * blockDim.x + tid;
//
//   if (tid < kNumVars) {
//     d_ratio[tid] = (d_cur_dom_size[idx] != 1)
//                        ? static_cast<float>(d_cur_dom_size[idx]) / deg
//                        : FLT_MAX;
//     shared_data[tid] = d_ratio[tid];
//   } else {
//     shared_data[tid] = FLT_MAX;
//   }
//   __syncthreads();
//
//   for (unsigned int s = blockDim.x / 2; s > 0; s >>= 1) {
//     if (tid < s && (tid + s) < kNumVars) {
//       if (shared_data[tid + s] < shared_data[tid]) {
//         shared_data[tid] = shared_data[tid + s];
//         d_min_index[blockIdx.x] = tid + s;
//       }
//     }
//     __syncthreads();
//   }
//
//   if (tid == 0) {
//     d_min_index[blockIdx.x] = shared_data[0];
//   }
// }

// 线程束级别归约函数
__inline__ __device__ float warpReduceMin(float val, int& idx, int& minIdx) {
  printf("val = %f, idx = %d\n", val, idx);
  for (int offset = warpSize / 2; offset > 0; offset >>= 1) {
    printf("---------------\n");
    float otherVal = __shfl_down_sync(0xFFFFFFFF, val, offset);
    int otherIdx = __shfl_down_sync(0xFFFFFFFF, idx, offset);
    if (otherVal < val) {
      val = otherVal;
      minIdx = otherIdx;
    }
  }
  return val;
}

// 块级别归约函数，使用共享内存
__inline__ __device__ float blockReduceMin(float val, int& idx, int& minIdx) {
  extern __shared__ float shared[];
  float* sharedVals = shared;
  int* sharedIdxs = (int*)&shared[blockDim.x];

  int tid = threadIdx.x;
  sharedVals[tid] = val;
  sharedIdxs[tid] = idx;
  printf("sharedVals[%d] = %f, sharedIdxs[%d] = %d\n", tid, sharedVals[tid],
         tid, sharedIdxs[tid]);
  __syncthreads();

  printf("val = %f, idx = %d\n", val, idx);
  for (int offset = warpSize / 2; offset > 0; offset >>= 1) {
    printf("---------------\n");
    float otherVal = __shfl_down_sync(0xFFFFFFFF, val, offset);
    int otherIdx = __shfl_down_sync(0xFFFFFFFF, idx, offset);
    printf("otherVal = %f, otherIdx = %d\n", otherVal, otherIdx);
    if (otherVal < val) {
      val = otherVal;
      minIdx = otherIdx;
    }
  }
  // 线程束内归约
  // val = warpReduceMin(val, idx, minIdx);

  // 将线程束的结果写入共享内存
  if ((tid % warpSize) == 0) {
    sharedVals[tid / warpSize] = val;
    sharedIdxs[tid / warpSize] = minIdx;
  }
  __syncthreads();

  // 仅在块内第一个线程束内进行最终归约
  if (tid < warpSize) {
    val = (tid < (blockDim.x / warpSize)) ? sharedVals[tid] : FLT_MAX;
    idx = (tid < (blockDim.x / warpSize)) ? sharedIdxs[tid] : -1;
    val = warpReduceMin(val, idx, minIdx);
  }

  return val;
}

// __global__ void calculateRatiosAndFindMinIndex(const int* d_cur_dom_size,
//                                                int* deg, int kNumVars,
//                                                int level) {
//   extern __shared__ float shared[];
//   float* shared_ratio = shared;
//   int* shared_index = (int*)&shared[blockDim.x];

//   int tid = threadIdx.x;
//   int idx = level * kNumVars + tid;
//   const int dom_size = d_cur_dom_size[idx];
//   const int degv = deg[tid];
//   printf("dom_size[%d] = %d, degv[%d] = %d\n", tid, dom_size, tid, degv);
//   float ratio = (tid < kNumVars && dom_size != 1 && degv != 0)
//                     ? static_cast<float>(dom_size) / degv
//                     : FLT_MAX;
//   printf("ratio[%d] = %f\n", tid, ratio);
//   __syncthreads();
//   shared_ratio[tid] = ratio;
//   shared_index[tid] = tid;
//   // __syncthreads();
//   printf("shared_index[%d] = %d, shared_ratio[%d]=%f\n", tid,
//   shared_index[tid],
//          tid, shared_ratio[tid]);
//   __syncthreads();
//   int minIdx = tid;
//   float minValue = blockReduceMin(ratio, minIdx, shared_index[tid]);

//   printf("val = %f, idx = %d\n", val, idx);
//   for (int offset = warpSize / 2; offset > 0; offset >>= 1) {
//     printf("---------------\n");
//     float otherVal = __shfl_down_sync(0xFFFFFFFF, val, offset);
//     int otherIdx = __shfl_down_sync(0xFFFFFFFF, idx, offset);
//     printf("otherVal = %f, otherIdx = %d\n", otherVal, otherIdx);
//     if (otherVal < val) {
//       val = otherVal;
//       minIdx = otherIdx;
//     }
//   }

//   if (tid == 0) {
//     // d_ratio[0] = minValue;
//     // d_min_index[0] = shared_index[0];
//     printf("d_min_index[0] = %d\n", shared_index[0]);
//   }
// }

// // CUDA内核函数，用于计算比值并找到最小值的索引
// __global__ void calculateRatiosAndFindMinIndex(const int* d_cur_dom_size,
//                                                int deg, float* d_ratio,
//                                                int* d_min_index, int
//                                                kNumVars, int level) {
//   int tid = threadIdx.x;
//   int idx = level * kNumVars + tid;

//   float ratio = (tid < kNumVars && d_cur_dom_size[idx] != 1)
//                     ? static_cast<float>(d_cur_dom_size[idx]) / deg
//                     : FLT_MAX;

//   int minIdx = idx;
//   float minValue = blockReduceMin(ratio, idx, minIdx);

//   if (tid == 0) {
//     d_ratio[0] = minValue;
//     d_min_index[0] = minIdx;
//   }
// }
// 定义结构体用于计算dom/deg

// CUDA内核函数，用于计算比值并找到最小值的索引
__global__ void calculateRatiosAndFindMinIndex(const int* d_cur_dom_size,
                                               const int* deg, int kNumVars,
                                               int level) {
  extern __shared__ float shared[];
  float* shared_ratio = shared;
  int* shared_index = (int*)&shared[blockDim.x * sizeof(float)];

  int tid = threadIdx.x;
  int idx = level * kNumVars + tid;

  const int dom_size = d_cur_dom_size[idx];
  const int degv = deg[tid];
  float ratio = (tid < kNumVars && dom_size != 1 && degv != 0)
                    ? static_cast<float>(dom_size) / degv
                    : FLT_MAX;
  printf("ratio[%d] = %f\n", tid, ratio);
  shared_ratio[tid] = ratio;
  shared_index[tid] = idx;
  __syncthreads();

  // 线程束级别归约
  for (int offset = warpSize / 2; offset > 0; offset >>= 1) {
    float otherVal = __shfl_down_sync(0xFFFFFFFF, ratio, offset);
    int otherIdx = __shfl_down_sync(0xFFFFFFFF, idx, offset);

    if (otherVal < ratio && otherVal > 0) {
      printf("otherVal = %f, otherIdx = %d, ratio = %f\n", otherVal, otherIdx,
             ratio);
      ratio = otherVal;
      idx = otherIdx;
    }
  }

  // 将线程束的结果写入共享内存
  if ((tid % warpSize) == 0) {
    shared_ratio[tid / warpSize] = ratio;
    shared_index[tid / warpSize] = idx;
    printf("ratio = %f, idx = %d\n", ratio, idx);
  }
  __syncthreads();

  // 仅在块内第一个线程束内进行最终归约
  if (tid < warpSize) {
    if (tid < (blockDim.x / warpSize)) {
      ratio = shared_ratio[tid];
      idx = shared_index[tid];
    } else {
      ratio = FLT_MAX;
      idx = -1;
    }
    printf("3ratio = %f, idx = %d\n", ratio, idx);
    for (int offset = warpSize / 2; offset > 0; offset >>= 1) {
      float otherVal = __shfl_down_sync(0xFFFFFFFF, ratio, offset);
      int otherIdx = __shfl_down_sync(0xFFFFFFFF, idx, offset);
      if (otherVal < ratio) {
        ratio = otherVal;
        idx = otherIdx;
      }
    }
  }

  if (tid == 0) {
    // d_ratio[0] = ratio;
    // d_min_index[0] = idx;
    printf("d_min_index[0] = %d\n", idx);
  }
}

// 每个线程对应一个论域
__global__ void CsCheckMain(i32* mConPre, const u32x3* mCon, u32* bitDom,
                            i32* dom_size, cudaTextureObject_t bitSup,
                            cudaTextureObject_t neiCon, int num_ConEvt,
                            int current_level) {
  // mCon索引ID
  const int bid = blockIdx.x;
  // bitDom索引
  const int a_0 = threadIdx.x;
  // bitDom索引
  const int a_1 = threadIdx.y;
  // 块内全局线程索引
  // const int tid = threadIdx.x + threadIdx.y * blockDim.x;
  const int tid = threadIdx.y * blockDim.x + threadIdx.x;
  // 计算当前层的偏移量
  int level_offset = current_level * kDeviceBitDomsIntSize;

  // printf("bid: %d, a_0: %d, a_1: %d\n", bid, a_0, a_1);

  // 每个线程都拿到当前约束信息
  auto c = mCon[bid];
  const int xid = c.x;
  const int yid = c.y;
  const int cid = c.z;

  // 动态分配共享内存，共三段：
  // 1. bitDom[x]
  // 2. bitDom[y]
  // 3. empty_dom，用于标记失败，默认值是1，即假设它失败，
  //    在最后检查的时候，如有一段word非空则置为0。
  extern __shared__ u32 shared_mem[];
  u32* s_bitDom_x = shared_mem;
  u32* s_bitDom_y = &shared_mem[kDeviceBitDomIntSize];
  // u32* empty_dom = &shared_mem[2 * kDeviceBitDomIntSize];
  __shared__ int changex;
  __shared__ int changey;
  // 块内的第一个线程修改下面的值
  if (tid == 0) {
    // empty_dom[0] = 1;
    changex = 0;
    changey = 0;
  }

  // 把bitDom写入两段共享内存
  if (a_0 < kDeviceBitDomIntSize && a_1 == 0) {
    s_bitDom_x[a_0] = bitDom[level_offset + DeviceGetBitDomByIndex(xid, a_0)];
    s_bitDom_y[a_0] = bitDom[level_offset + DeviceGetBitDomByIndex(yid, a_0)];
    printf(
        "----cid: %d, a_0: %d, a_1: %d, mConPre: %d, s_bitDom_x: %x, "
        "s_bitDom_y: %x\n",
        cid, a_0, a_1, mConPre[cid], s_bitDom_x[a_0], s_bitDom_y[a_0]);
  }
  __syncthreads();

  // 取当前值(x, a_0)是否有效
  // 取当前值(y, a_0)是否有效
  const int l_xa = BITSET_GET(s_bitDom_x, a_0);
  const int l_ya = BITSET_GET(s_bitDom_y, a_0);

  u32 val_x = 0;
  u32 val_y = 0;

  // 取得cid里支持(x, a_0)的bitDom->bitSup[c][a_1][a_0]->bitSup[c][~][a],
  // TODO:这里有问题，没有进行好块内归约，我需要按threadIdx.y的对数步长归约
  if (kDeviceBitDomIntSize == 1) {
    // Case 1: MaxDomSize \in (0,32]
    auto [x, y] = tex3D<uint2>(bitSup, a_0, 0, bid);
    val_x |= l_xa && (x & s_bitDom_y[0]);
    val_y |= l_ya && (y & s_bitDom_x[0]);
  } else if (kDeviceBitDomIntSize == 2) {
    // Case 2: MaxDomSize \in (32,64]
    for (int i = 0; i < kDeviceBitDomIntSize; ++i) {
      auto bitSup_cid = tex3D<uint2>(bitSup, a_0, i, bid);
      val_x |= l_xa && (bitSup_cid.x & s_bitDom_y[i]);
      val_y |= l_ya && (bitSup_cid.y & s_bitDom_x[i]);
    }
  }
  // Case 3……
  // TODO: 束内归约应该并不用同步
  __syncthreads();
  // // 束内归约
  // // 只有threadIdx.x=0的那一维度归约
  // bool changed = false;
  if (a_1 == 0) {
    //   // 线程束内投票
    unsigned int vote_x = __ballot_sync(0xFFFFFFFF, val_x != 0);
    unsigned int vote_y = __ballot_sync(0xFFFFFFFF, val_y != 0);

    // printf(
    //     "----cid: %d, a_0: %d, a_1: %d, s_bitDom_x: %x, s_bitDom_y: %x, l_xa:
    //     "
    //     "%d, l_ya: %d, vote_x:%x, vote_y :%x\n",
    //     cid, a_0, a_1, s_bitDom_x[a_0 / 32], s_bitDom_y[a_0 / 32], l_xa,
    //     l_ya, vote_x, vote_y);

    // 只是线程束里的第一个线程做如下操作：
    // 只写回自己那块bitDom
    // 先获取bitDom的分块索引
    // 先与共享内存里的bitDom比较有改变才写回
    // TODO:写回后能拿到写前的原值，这个值再与vote值与一下能拿到那一个bitdom的分块，
    // TODO:如果这个果为0那么就是空的，这个时候就可以设置empty_dom为0，这个想法需要验证
    // TODO:这里的写回操作应该是原子操作，如果可行，则可以省略一个检查是否为0的过程。
    if (a_0 % warpSize == 0) {
      int bitIdx = a_0 / 32;
      if (s_bitDom_x[bitIdx] ^ vote_x) {
        changex = true;
        u32 oldVal = atomicAnd(
            &bitDom[level_offset + DeviceGetBitDomByIndex(xid, bitIdx)],
            vote_x);

        // 重做一次按位与，获得当时做原子运算后，论域个数，再减原论域，
        // 得到减少的论域值的个数delete_num_value
        // 全局论域原子减去delete_num_value得到当时的论域情况，这里不会出现重复减的情况
        int delete_num_values = __popc(oldVal) - __popc(oldVal & vote_x);
        int old_domian_size = atomicSub(
            &dom_size[current_level * kDeviceNumVars + xid], delete_num_values);
        // TODO: 这里可以取消注释验证是否正确。
        // 或者检验old_domian_size-delete_num_value?=0,直接修改全局变量，退出GAC
        if (!(old_domian_size - delete_num_values)) GAC_success = false;
        // if (oldVal & vote_x != 0) empty_dom[0] = 0;
        printf(
            "x_v: %d, bitDom = %x, ori = %x, now = %x, changex = %d, "
            "delete_num_values = %d\n",
            xid, vote_x, oldVal,
            bitDom[level_offset + DeviceGetBitDomByIndex(xid, bitIdx)], changex,
            delete_num_values);
      }

      if (s_bitDom_y[bitIdx] ^ vote_y) {
        changey = true;
        auto oldVal = atomicAnd(
            &bitDom[level_offset + DeviceGetBitDomByIndex(yid, bitIdx)],
            vote_y);

        int delete_num_values = __popc(oldVal) - __popc(oldVal & vote_y);
        int old_domian_size = atomicSub(
            &dom_size[current_level * kDeviceNumVars + yid], delete_num_values);
        // TODO: 这里可以取消注释验证是否正确。
        // 或者检验old_domian_size-delete_num_value?=0,直接修改全局变量，退出GAC
        if (!(old_domian_size - delete_num_values)) GAC_success = false;
        // if (oldVal & vote_y != 0) empty_dom[0] = 0;
        // printf("a_0: %d, a_1: %d, empty_dom: %d\n", a_0, a_1, empty_dom[0]);
        printf(
            "y_v: %d, bitDom = %x, ori = %x, now = %x, changey = "
            "%d, delete_num_values = %d\n",
            yid, vote_y, oldVal,
            bitDom[level_offset + DeviceGetBitDomByIndex(yid, bitIdx)], changey,
            delete_num_values);
      }
    }
  }

  // __syncthreads();
  // printf("changed: %d\n", changed);
  // return;
  // 再load一次检查是否为0
  // 这里要计算论域大小，方法为
  // if (a_0 < kDeviceBitDomIntSize && a_1 == 0) {
  //   if (bitDom[level_offset + DeviceGetBitDomByIndex(xid, a_0)] != 0)
  //     empty_dom[0] = 0;
  //   if (bitDom[level_offset + DeviceGetBitDomByIndex(yid, a_0)] != 0)
  //     empty_dom[0] = 0;
  // }
  // __syncthreads();
  // if (a_0 == 0 && a_1 == 0 && empty_dom[0] == 1) {
  //   GAC_success = 0;
  //   printf("a_0: %d, a_1: %d, empty_dom: %d\n", a_0, a_1, empty_dom[0]);
  // }
  __syncthreads();
  // printf("GAC_success: %d\n", GAC_success);
  // propagate changed to neighbour constraints
  // 每个线程一个约束，如果约束个数大于线程块长度，要用for循环,
  // TODO::两次可以合到一起
  if (GAC_success && changex) {
    for (int idx = tid; idx < kDeviceNumTabs; idx += blockDim.x * blockDim.y) {
      auto val = tex2D<int>(neiCon, idx, xid);
      printf("xid: %d, tid: %d = %d\n", xid, tid, val);
      if (val != 0) {
        mConPre[idx] = 1;
      }
    }
  }

  if (GAC_success && changey) {
    for (int idx = tid; idx < kDeviceNumTabs; idx += blockDim.x * blockDim.y) {
      auto val = tex2D<int>(neiCon, idx, yid);
      printf("yid: %d, tid: %d = %d\n", yid, tid, val);
      if (val != 0) {
        mConPre[idx] = 1;
      }
    }
  }
  // printf("end check\n");
}

// 这里只提供值
__global__ void AssignValue(int2* assigned, u32* bitDom, int* dom_size,
                            int varid, int current_level) {
  // printf("xixi2,currentlevel: %d\n", current_level);
  // 在一个线程束内完成，因为只能改变一个变量论域，它不大于1024，所以是一个里完成
  int tid = threadIdx.x;
  u32 bd = 0;

  // if (tid <= kDeviceNumVars * kDeviceNumVars) {
  //   printf("domsize[%d]: %d\n", tid, dom_size[tid]);
  //   dom_size[tid] = 1;
  //   printf("domsize[%d]: %d\n", tid, dom_size[tid]);
  // }

  int level_offset = current_level * kDeviceBitDomsIntSize;

  if (tid < kDeviceBitDomIntSize) {
    bd = bitDom[level_offset + DeviceGetBitDomByIndex(varid, tid)];
    bitDom[level_offset + DeviceGetBitDomByIndex(varid, tid)] = 0;
  }

  // 每个线程拿到一个bitDom, 一个线程束肯定能拿完该变量所有的bitDom
  // 检查当前bitdom在u32是否为0，投票取mask，然后取第一个
  // 看看哪个变量被赋值了
  // 计算最小值，并赋值

  u32 nonZero_bitdom = __ballot_sync(0xFFFFFFFF, bd != 0);
  int offset_int_idx = __ffs(nonZero_bitdom) - 1;

  // printf(
  //     "varid == c.x, cid: %d, c.x: %d, c.y: %d, varid: %d, bd: %x, tid:
  //     "
  //     "%d, nonZero_bitdom: %x, offset_int_idx: %d\n",
  //     cid, c.x, c.y, varid, bd, tid, nonZero_bitdom, offset_int_idx);

  // 计算最小值，修改全局变量和共享内存和bitDom
  if (tid == offset_int_idx) {
    int ffs = __ffs(bd) - 1;
    int min_value = offset_int_idx * U32_BIT + ffs;
    assigned[current_level].y = min_value;
    // s_bitDom_x[offset_int_idx] = 1U << WORD_OFFSET(ffs);
    bitDom[level_offset + DeviceGetBitDomByIndex(varid, tid)] =
        1U << WORD_OFFSET(ffs);
    assigned[current_level].x = varid;
    assigned[current_level].y = min_value;
    // dom_size[current_level * kMaxNumVars + varid] = 1;
    dom_size[current_level * kDeviceNumVars + varid] = 1;
    printf("min_value: (%d, %d) at level: %d, offset: %d, dom_size[%d] = %d\n",
           varid, min_value, current_level,
           level_offset + DeviceGetBitDomByIndex(varid, tid), varid,
           dom_size[current_level * kMaxNumVars + varid]);
  }

  if (tid < kDeviceBitDomIntSize) {
    u32 xx = bitDom[level_offset + DeviceGetBitDomByIndex(varid, tid)];
    printf("var: %d, bitDom: %x\n", varid, xx);
  }
}

// 只启动一个线程
__global__ void RemoveValue(int2* assigned, u32* bitDom, int* dom_size,
                            int varid, int current_level) {
  int tid = threadIdx.x;
  int level_offset = current_level * kDeviceBitDomsIntSize;
  int var = assigned[current_level].x;
  int val = assigned[current_level].y;
  if (tid == 0) {
    bitDom[level_offset + DeviceGetBitDomByIndex(varid, val)] &=
        ~(1U << WORD_OFFSET(val));
    dom_size[current_level * kDeviceNumVars + varid]--;
  }
}

// 每个线程对应一个论域
__global__ void CsCheckMainAfterDecision(
    i32* mConPre, const u32x3* mCon, const u32x3* sub, const int* sub_offset,
    u32* bitDom, i32* dom_size, cudaTextureObject_t bitSup,
    cudaTextureObject_t neiCon, int num_ConEvt, int2* assigned, int varid,
    int action, int current_level) {
  // mCon索引ID
  const int bid = blockIdx.x;
  // bitDom索引
  const int a_0 = threadIdx.x;
  // bitDom索引
  const int a_1 = threadIdx.y;
  // 块内全局线程索引
  // const int tid = threadIdx.x + threadIdx.y * blockDim.x;
  const int tid = threadIdx.y * blockDim.x + threadIdx.x;
  // 计算当前层的偏移量
  int level_offset = current_level * kDeviceBitDomsIntSize;

  // printf("bid: %d, a_0: %d, a_1: %d\n", bid, a_0, a_1);
  // auto asg = assigned

  // const auto asg = assigned[d_assigned_size];
  // 每个线程都拿到当前约束信息
  int offs = bid + sub_offset[varid];
  auto c = sub[offs];
  const int xid = c.x;
  const int yid = c.y;
  const int cid = c.z;
  if (tid == 0) {
    printf(
        "cid: %d, cx: %d, cy: %d, varid: %d, action: %d, current_level: %d\n",
        cid, c.x, c.y, varid, action, current_level);
  }

  // return;
  // 动态分配共享内存，共三段：
  // 1. bitDom[x]
  // 2. bitDom[y]
  // 3. empty_dom，用于标记失败，默认值是1，即假设它失败，
  //    在最后检查的时候，如有一段word非空则置为0。

  extern __shared__ u32 shared_mem[];
  u32* s_bitDom_x = shared_mem;
  u32* s_bitDom_y = &shared_mem[kDeviceBitDomIntSize];

  __shared__ int min_value;
  __shared__ int changex;
  __shared__ int changey;
  __shared__ int s_assigned_var;
  // __shared__ int s_assigned_var_idx;
  __shared__ int s_assigned_val;
  // int assi_id = xid == varid ? xid : yid;

  // 块内的第一个线程修改下面的值
  // 刚赋值的变量给全局
  // 默认xy都改动了
  if (tid == 0) {
    changex = 0;
    changey = 0;
    s_assigned_var = varid;
    // 全局赋值堆栈是一个数组
    assigned[current_level].x = varid;
    // assigned_var = varid;
  }
  // return;
  // 先把bitDom写入两段共享内存
  if (a_0 < kDeviceBitDomIntSize && a_1 == 0) {
    s_bitDom_x[a_0] = bitDom[level_offset + DeviceGetBitDomByIndex(xid, a_0)];
    s_bitDom_y[a_0] = bitDom[level_offset + DeviceGetBitDomByIndex(yid, a_0)];
    printf("cid: %d, s_bitDom: %x, %x\n", cid, s_bitDom_x[a_0],
           s_bitDom_y[a_0]);
  }
  __syncthreads();
  // 先计算varid的最小值，

  // 仅第一个线程束工作，因为kBitDomIntSize<32
  // if (tid / warpSize == 0) {
  // 如果是赋值，我不知道最小值是多少先算最小值赋值给全局变量
  // }
  // return;
  // __syncthreads();
  // __syncthreads();

  // 取当前值(x, a_0)是否有效
  // 取当前值(y, a_0)是否有效
  const int l_xa = BITSET_GET(s_bitDom_x, a_0);
  const int l_ya = BITSET_GET(s_bitDom_y, a_0);

  u32 val_x = 0;
  u32 val_y = 0;
  // printf("xixi\n");
  // return;
  __syncthreads();
  // 取得cid里支持(x, a_0)的bitDom->bitSup[c][a_1][a_0]->bitSup[c][~][a],
  // TODO:这里有问题，没有进行好块内归约，我需要按threadIdx.y的对数步长归约
  if (kDeviceBitDomIntSize == 1) {
    // Case 1: MaxDomSize \in (0,32]
    auto [x, y] = tex3D<uint2>(bitSup, a_0, 0, bid);
    val_x |= l_xa && (x & s_bitDom_y[0]);
    val_y |= l_ya && (y & s_bitDom_x[0]);
    // printf("cid: %d, val[%d]: %x, %x\n", cid, a_0, val_x, val_y);
  } else if (kDeviceBitDomIntSize == 2) {
    // Case 2: MaxDomSize \in (32,64]
    for (int i = 0; i < kDeviceBitDomIntSize; ++i) {
      auto bitSup_cid = tex3D<uint2>(bitSup, a_0, i, bid);
      val_x |= l_xa && (bitSup_cid.x & s_bitDom_y[i]);
      val_y |= l_ya && (bitSup_cid.y & s_bitDom_x[i]);
    }
  }

  // return;
  // Case 3……
  // TODO: 束内归约应该并不用同步
  __syncthreads();
  // // 束内归约
  // // 只有threadIdx.x=0的那一维度归约
  // bool changed = false;
  if (a_1 == 0) {
    //   // 线程束内投票
    unsigned int vote_x = __ballot_sync(0xFFFFFFFF, val_x != 0);
    unsigned int vote_y = __ballot_sync(0xFFFFFFFF, val_y != 0);

    // printf(
    //     "----cid: %d, a_0: %d, a_1: %d, s_bitDom_x: %x, s_bitDom_y: %x, l_xa:
    //     "
    //     "%d, l_ya: %d, vote_x:%x, vote_y :%x\n",
    //     cid, a_0, a_1, s_bitDom_x[a_0 / 32], s_bitDom_y[a_0 / 32], l_xa,
    //     l_ya, vote_x, vote_y);

    // 只是线程束里的第一个线程做如下操作：
    // 只写回自己那块bitDom
    // 先获取bitDom的分块索引
    // 先与共享内存里的bitDom比较有改变才写回
    // TODO:写回后能拿到写前的原值，这个值再与vote值与一下能拿到那一个bitdom的分块，
    // TODO:如果这个果为0那么就是空的，这个时候就可以设置empty_dom为0，这个想法需要验证
    // TODO:这里的写回操作应该是原子操作，如果可行，则可以省略一个检查是否为0的过程。
    if (a_0 % warpSize == 0) {
      int bitIdx = a_0 / 32;
      if (s_bitDom_x[bitIdx] ^ vote_x) {
        changex = true;
        u32 oldVal = atomicAnd(
            &bitDom[level_offset + DeviceGetBitDomByIndex(xid, bitIdx)],
            vote_x);

        // 重做一次按位与，获得当时做原子运算后，论域个数，再减原论域，
        // 得到减少的论域值的个数delete_num_value
        // 全局论域原子减去delete_num_value得到当时的论域情况，这里不会出现重复减的情况
        int delete_num_values = __popc(oldVal) - __popc(oldVal & vote_x);
        int old_domian_size = atomicSub(
            &dom_size[current_level * kDeviceNumVars + xid], delete_num_values);
        // TODO: 这里可以取消注释验证是否正确。
        // 或者检验old_domian_size-delete_num_value?=0,直接修改全局变量，退出GAC
        if (!(old_domian_size - delete_num_values)) GAC_success = false;
        // if (oldVal & vote_x != 0) empty_dom[0] = 0;
        printf(
            "x_v: %d, bitDom = %x, ori = %x, now = %x, changex = %d, "
            "delete_num_values = %d, old_domian_size = %d, dom_size[%d] = %d, "
            "level = %d\n",
            xid, vote_x, oldVal,
            bitDom[level_offset + DeviceGetBitDomByIndex(xid, bitIdx)], changex,
            delete_num_values, old_domian_size, xid,
            dom_size[current_level * kDeviceNumVars + xid], current_level);
      }

      if (s_bitDom_y[bitIdx] ^ vote_y) {
        changey = true;
        auto oldVal = atomicAnd(
            &bitDom[level_offset + DeviceGetBitDomByIndex(yid, bitIdx)],
            vote_y);

        int delete_num_values = __popc(oldVal) - __popc(oldVal & vote_y);
        int old_domian_size = atomicSub(
            &dom_size[current_level * kDeviceNumVars + yid], delete_num_values);
        // TODO: 这里可以取消注释验证是否正确。
        // 或者检验old_domian_size-delete_num_value?=0,直接修改全局变量，退出GAC
        if (!(old_domian_size - delete_num_values)) GAC_success = false;
        // if (oldVal & vote_y != 0) empty_dom[0] = 0;
        // printf("a_0: %d, a_1: %d, empty_dom: %d\n", a_0, a_1, empty_dom[0]);
        printf(
            "y_v: %d, bitDom = %x, ori = %x, now = %x, changey = "
            "%d, delete_num_values = %d, old_domian_size = %d, dom_size[%d] = "
            "%d, "
            "level = %d\n",
            yid, vote_y, oldVal,
            bitDom[level_offset + DeviceGetBitDomByIndex(yid, bitIdx)], changey,
            delete_num_values, old_domian_size, yid,
            dom_size[current_level * kDeviceNumVars + yid], current_level);
      }
    }
  }

  // __syncthreads();
  // printf("changed: %d\n", changed);
  // return;
  // 再load一次检查是否为0
  // 这里要计算论域大小，方法为
  // if (a_0 < kDeviceBitDomIntSize && a_1 == 0) {
  //   if (bitDom[level_offset + DeviceGetBitDomByIndex(xid, a_0)] != 0)
  //     empty_dom[0] = 0;
  //   if (bitDom[level_offset + DeviceGetBitDomByIndex(yid, a_0)] != 0)
  //     empty_dom[0] = 0;
  // }
  // __syncthreads();
  // if (a_0 == 0 && a_1 == 0 && empty_dom[0] == 1) {
  //   GAC_success = 0;
  //   printf("a_0: %d, a_1: %d, empty_dom: %d\n", a_0, a_1, empty_dom[0]);
  // }
  __syncthreads();
  // printf("GAC_success: %d\n", GAC_success);
  // propagate changed to neighbour constraints
  // 每个线程一个约束，如果约束个数大于线程块长度，要用for循环,
  // TODO::两次可以合到一起
  if (GAC_success && changex) {
    for (int idx = tid; idx < kDeviceNumTabs; idx += blockDim.x * blockDim.y) {
      auto val = tex2D<int>(neiCon, idx, xid);
      printf("xid: %d, tid: %d = %d\n", xid, tid, val);
      if (val != 0) {
        mConPre[idx] = 1;
      }
    }
  }

  if (GAC_success && changey) {
    for (int idx = tid; idx < kDeviceNumTabs; idx += blockDim.x * blockDim.y) {
      auto val = tex2D<int>(neiCon, idx, yid);
      printf("yid: %d, tid: %d = %d\n", yid, tid, val);
      if (val != 0) {
        mConPre[idx] = 1;
      }
    }
  }
  // printf("end check\n");
}

// // action = 0 是删值，action=1是赋值
// __global__ void CsCheckMainAfterSigned(i32* mConPre, const u32x3* mCon,
//                                        u32* bitDom, const i32* dom_size,
//                                        cudaTextureObject_t bitSup,
//                                        cudaTextureObject_t neiCon,
//                                        int num_ConEvt, int varid, int val_a,
//                                        int action, int current_level) {
//   // mCon索引ID
//   const int bid = blockIdx.x;
//   // bitDom索引
//   const int a_0 = threadIdx.x;
//   // bitDom索引
//   const int a_1 = threadIdx.y;
//   // 块内全局线程索引
//   const int tid = threadIdx.x + threadIdx.y * blockDim.x;
//   // 计算当前层的偏移量
//   int level_offset = current_level * kDeviceBitDomsIntSize;

//   // printf("bid: %d, a_0: %d, a_1: %d, num_ConEvt: %d\n", bid, a_0, a_1,
//   //        num_ConEvt);

//   // 每个线程都拿到当前约束信息
//   auto c = mCon[bid];
//   const int xid = c.x;
//   const int yid = c.y;
//   const int cid = c.z;

//   // 动态分配共享内存，共三段：
//   // 1. bitDom[x]
//   // 2. bitDom[y]
//   // 3. empty_dom，用于标记失败，默认值是1，即假设它失败，
//   //    在最后检查的时候，如有一段word非空则置为0。
//   extern __shared__ u32 shared_mem[];
//   u32* s_bitDom_x = shared_mem;
//   u32* s_bitDom_y = &shared_mem[kDeviceBitDomIntSize];
//   u32* empty_dom = &shared_mem[2 * kDeviceBitDomIntSize];
//   u32* minValue = &shared_mem[2 * kDeviceBitDomIntSize + 1];
//   if (a_0 == 0 && a_1 == 0) empty_dom[0] = 1;

//   // 把bitDom写入两段共享内存
//   // 这里把要原变量dom通过赋值，删值计算写到共享内存,再将这个值写回全局
//   // 这里的a_0bitvector 的一个 int
//   if (a_0 < kDeviceBitDomIntSize && a_1 == 0) {
//     // 对于删值任务，是从全局内存写入赋值的
//     assigned_var = varid;
//     const int a = assigned_val;
//     if (action == 0) {
//       const int wordIndex = WORD_INDEX(a);
//       if (varid == c.x && a_0 == wordIndex) {
//         s_bitDom_x[a_0] =
//             bitDom[level_offset + DeviceGetBitDomByIndex(xid, a_0)] &
//             ~(1U << WORD_OFFSET(a));
//         bitDom[level_offset + DeviceGetBitDomByIndex(xid, a_0)] &=
//             ~(1U << WORD_OFFSET(a));
//       } else if (varid == c.y && a_0 == wordIndex) {
//         s_bitDom_y[a_0] =
//             bitDom[level_offset + DeviceGetBitDomByIndex(yid, a_0)] &
//             ~(1U << WORD_OFFSET(a));
//         bitDom[level_offset + DeviceGetBitDomByIndex(yid, a_0)] &=
//             ~(1U << WORD_OFFSET(a));
//       }
//       // s_bitDom_x[a_0] =
//     } else if (action == 1) {
//       // 赋值,需要先找到最小值
//       // 对于赋值任务，赋值是写入全局内存的
//       // 对于删值任务，是从全局内存写入赋值的
//       assigned_var = varid;

//       // s_bitDom_x[a_0] = 0;
//       if (varid == c.x) {
//         // 先写入局部内存a中
//         const int a = bitDom[level_offset + DeviceGetBitDomByIndex(xid,
//         a_0)];
//         // 再计算得到最小值=a_0是dom的偏移量×32+ffs
//         // 共享内中原子写回得到最小值，
//         const int b =
//             (__ffs(a) != 0) ? a_0 * U32_BIT + (__ffs(a) - 1) : FLT_MAX;
//         atomicMin(minValue, b);

//         int wordIndex = WORD_INDEX(minValue[0]);

//         s_bitDom_x[a_0] = 0;
//         bitDom[level_offset + DeviceGetBitDomByIndex(xid, a_0)] = 0;
//         if (a_0 == wordIndex) {
//           const u32 tmp_mask = ~(1U << WORD_OFFSET(a));
//           s_bitDom_x[a_0] = tmp_mask;
//           bitDom[level_offset + DeviceGetBitDomByIndex(xid, a_0)] = tmp_mask;
//         }
//       } else if (varid == c.y) {
//         // 先写入局部内存a中
//         const int a = bitDom[level_offset + DeviceGetBitDomByIndex(yid,
//         a_0)];
//         // 再计算得到最小值=a_0是dom的偏移量×32+ffs
//         // 共享内中原子写回得到最小值，
//         const int b =
//             (__ffs(a) != 0) ? a_0 * U32_BIT + (__ffs(a) - 1) : FLT_MAX;
//         atomicMin(minValue, b);

//         int wordIndex = WORD_INDEX(minValue[0]);

//         s_bitDom_y[a_0] = 0;
//         bitDom[level_offset + DeviceGetBitDomByIndex(yid, a_0)] = 0;
//         if (a_0 == wordIndex) {
//           const u32 tmp_mask = ~(1U << WORD_OFFSET(a));
//           s_bitDom_y[a_0] = tmp_mask;
//           bitDom[level_offset + DeviceGetBitDomByIndex(yid, a_0)] = tmp_mask;
//         }
//       }
//     } else {
//     }
//   }

//   // if (a_0 < kDeviceBitDomIntSize && a_1 == 0) {
//   //   s_bitDom_x[a_0] = bitDom[level_offset + DeviceGetBitDomByIndex(xid,
//   //   a_0)]; s_bitDom_y[a_0] = bitDom[level_offset +
//   //   DeviceGetBitDomByIndex(yid, a_0)];
//   //   // printf(
//   //   //     "----cid: %d, a_0: %d, a_1: %d, mConPre: %d, s_bitDom_x: %x, "
//   //   //     "s_bitDom_y: %x\n",
//   //   //     cid, a_0, a_1, mConPre[cid], s_bitDom_x[a_0], s_bitDom_y[a_0]);
//   // }
//   __syncthreads();

//   // 取当前值(x, a_0)是否有效
//   // 取当前值(y, a_0)是否有效
//   const int l_xa = BITSET_GET(s_bitDom_x, a_0);
//   const int l_ya = BITSET_GET(s_bitDom_y, a_0);

//   u32 val_x = 0;
//   u32 val_y = 0;

//   // 取得cid里支持(x, a_0)的bitDom->bitSup[c][a_1][a_0]->bitSup[c][~][a],
//   // TODO:这里有问题，没有进行好块内归约，我需要按threadIdx.y的对数步长归约
//   if (kDeviceBitDomIntSize == 1) {
//     // Case 1: MaxDomSize \in (0,32]
//     auto [x, y] = tex3D<uint2>(bitSup, a_0, 0, bid);
//     val_x |= l_xa && (x & s_bitDom_y[0]);
//     val_y |= l_ya && (y & s_bitDom_x[0]);
//   } else if (kDeviceBitDomIntSize == 2) {
//     // Case 2: MaxDomSize \in (32,64]
//     for (int i = 0; i < kDeviceBitDomIntSize; ++i) {
//       auto bitSup_cid = tex3D<uint2>(bitSup, a_0, i, bid);
//       val_x |= l_xa && (bitSup_cid.x & s_bitDom_y[i]);
//       val_y |= l_ya && (bitSup_cid.y & s_bitDom_x[i]);
//     }
//   }
//   // Case 3……
//   // TODO: 束内归约应该并不用同步
//   __syncthreads();
//   // // 束内归约
//   // // 只有threadIdx.x=0的那一维度归约
//   bool changed = false;
//   if (a_1 == 0) {
//     //   // 线程束内投票
//     unsigned int vote_x = __ballot_sync(0xFFFFFFFF, val_x != 0);
//     unsigned int vote_y = __ballot_sync(0xFFFFFFFF, val_y != 0);

//     // printf(
//     //     "----cid: %d, a_0: %d, a_1: %d, s_bitDom_x: %x, s_bitDom_y: %x,
//     l_xa:
//     //     "
//     //     "%d, l_ya: %d, vote_x:%x, vote_y :%x\n",
//     //     cid, a_0, a_1, s_bitDom_x[a_0 / 32], s_bitDom_y[a_0 / 32], l_xa,
//     //     l_ya, vote_x, vote_y);

//     // 只是线程束里的第一个线程做如下操作：
//     // 只写回自己那块bitDom
//     // 先获取bitDom的分块索引
//     // 先与共享内存里的bitDom比较有改变才写回
//     //
//     TODO:写回后能拿到写前的原值，这个值再与vote值与一下能拿到那一个bitdom的分块，
//     //
//     TODO:如果这个果为0那么就是空的，这个时候就可以设置empty_dom为0，这个想法需要验证
//     //
//     TODO:这里的写回操作应该是原子操作，如果可行，则可以省略一个检查是否为0的过程。
//     if (a_0 % warpSize == 0) {
//       int bitIdx = a_0 / 32;
//       if (s_bitDom_x[bitIdx] ^ vote_x) {
//         changed = true;
//         u32 oldVal = atomicAnd(
//             &bitDom[level_offset + DeviceGetBitDomByIndex(xid, bitIdx)],
//             vote_x);
//         // if (oldVal & vote_x != 0) empty_dom[0] = 0;
//         printf("v: %d, bitDom = %x, now = %x\n", xid, vote_x,
//                bitDom[level_offset + DeviceGetBitDomByIndex(xid, bitIdx)]);
//       }

//       if (s_bitDom_y[bitIdx] ^ vote_y) {
//         changed = true;
//         auto oldVal = atomicAnd(
//             &bitDom[level_offset + DeviceGetBitDomByIndex(yid, bitIdx)],
//             vote_y);
//         // if (oldVal & vote_y != 0) empty_dom[0] = 0;
//         // printf("a_0: %d, a_1: %d, empty_dom: %d\n", a_0, a_1,
//         empty_dom[0]); printf("v: %d, bitDom = %x, now = %x\n", yid, vote_y,
//                bitDom[level_offset + DeviceGetBitDomByIndex(yid, bitIdx)]);
//       }
//     }
//   }

//   __syncthreads();
//   // printf("changed: %d\n", changed);
//   // return;
//   // 再load一次检查是否为0
//   if (a_0 < kDeviceBitDomIntSize && a_1 == 0) {
//     if (bitDom[level_offset + DeviceGetBitDomByIndex(xid, a_0)] != 0)
//       empty_dom[0] = 0;
//     if (bitDom[level_offset + DeviceGetBitDomByIndex(yid, a_0)] != 0)
//       empty_dom[0] = 0;
//   }
//   __syncthreads();
//   if (a_0 == 0 && a_1 == 0 && empty_dom[0] == 1) {
//     GAC_success = 0;
//     printf("a_0: %d, a_1: %d, empty_dom: %d\n", a_0, a_1, empty_dom[0]);
//   }
//   __syncthreads();
//   // printf("GAC_success: %d\n", GAC_success);
//   // propagate changed to neighbour constraints
//   if (GAC_success && changed) {
//     for (int idx = tid; idx < kDeviceNumTabs; idx += blockDim.x * blockDim.y)
//     {
//       auto val = tex2D<int>(neiCon, idx, cid);
//       printf("cid: %d, tid: %d = %d\n", cid, tid, val);
//       if (val != 0) {
//         mConPre[idx] = 1;
//       }
//     }
//   }
//   // printf("end check\n");
// }

// 定义一个判断条件的谓词
struct is_one {
  __host__ __device__ bool operator()(const int x) const { return x == 1; }
};

int CModel::compress_Main() {
  d_MConEvt.resize(d_MCon.size());
  // 使用 thrust::copy_if 进行流压缩
  auto end = thrust::copy_if(d_MCon.begin(), d_MCon.end(),  // 输入范围
                             d_ConPre.begin(),   // 输入范围的判断条件
                             d_MConEvt.begin(),  // 输出范围
                             is_one()            // 判断条件
  );

  thrust::fill(d_ConPre.begin(), d_ConPre.end(), 0);
  // 调整 d_MConEvt 的大小以匹配实际复制的元素数
  d_MConEvt.resize(thrust::distance(d_MConEvt.begin(), end));
  return d_MConEvt.size();
}

void CModel::BuildBitModel(const HModel& xm) {
#pragma region 计算常量
  // 变量个数
  // VS_SIZE = xm->Vars().size();
  // 约束个数
  // CS_SIZE = xm->Tabs().size();
  // 约束最大元数
  // MAX_ARITY = xm->max_arity();
  // 最大变量论域大小
  // MAX_DOM_SIZE = xm->max_domain_size();
  // 计算有多少个int可以表示，一个bitDom[x]
  // BITDOM_INTSIZE = intsizeof(MAX_DOM_SIZE);
  // 总bitDom长度，一个子问题的所有bitDoms的长度
  // BITDOMS_INTSIZE = BITDOM_INTSIZE * VS_SIZE;
  // 一个bitSup的int长度
  // TODO: 这里看情况可能可乘个2, 现在这里暂不乘，因为用了int2的数据类型
  // BITSUP_INTSIZE = MAX_DOM_SIZE * BITDOM_INTSIZE;
  // 所有bitSup的int长度
  // BITSUPS_INTSIZE = BITSUP_INTSIZE * CS_SIZE;
  // 总长度
  // BITSUBDOMS_INTSIZE = VS_SIZE * MAX_DOM_SIZE * BITDOMS_INTSIZE;
  // SUBCON_SIZE = VS_SIZE * MAX_DOM_SIZE * CS_SIZE;
#pragma endregion 计算常量
#pragma region 约束网络信息
  // 初始化数据
  // LOG(INFO) << "-----neighbour-----";
  // cudaMalloc(&d_ConNeighbor, sizeof(int) * kNumTabs * kNumTabs);
  // int h_ConNeighbor[kNumTabs * kNumTabs] = {};

  // for (int i = 0; i < kNumTabs; ++i) {
  //   auto ci = xm->Tabs(i);
  //   for (int j = 0; j < kNumTabs; ++j) {
  //     auto cj = xm->Tabs(j);
  //     if (HModel::is_neighbour(ci, cj) && i != j) {
  //       h_ConNeighbor[i * kNumTabs + j] = 1;  // 简单初始化
  //     } else {
  //       h_ConNeighbor[i * kNumTabs + j] = 0;
  //     }
  //   }
  // }

  // // // 打印初始化后的数据
  // // for (size_t i = 0; i < kNumTabs * kNumTabs; i++) {
  // //   std::cout << h_ConNeighbor[i] << " ";
  // // }
  // // std::cout << std::endl;

  // cudaMemcpy(d_ConNeighbor, h_ConNeighbor, sizeof(int) * kNumTabs * kNumTabs,
  //            cudaMemcpyHostToDevice);

  // // Allocate CUDA array in device memory
  // cudaChannelFormatDesc channelDesc = cudaCreateChannelDesc<int>();
  // cudaMallocArray(&cuArray_MCon, &channelDesc, kNumTabs, kNumTabs);

  // // Copy data to device memory
  // const size_t spitch = kNumTabs * sizeof(int);
  // cudaMemcpy2DToArray(cuArray_MCon, 0, 0, h_ConNeighbor, spitch,
  //                     kNumTabs * sizeof(int), kNumTabs,
  //                     cudaMemcpyHostToDevice);
  // cudaDeviceSynchronize();

  // // Specify texture resource
  // memset(&resDesc_MCon, 0, sizeof(resDesc_MCon));
  // resDesc_MCon.resType = cudaResourceTypeArray;
  // resDesc_MCon.res.array.array = cuArray_MCon;

  // // Specify texture object parameters
  // memset(&texDesc_MCon, 0, sizeof(texDesc_MCon));
  // texDesc_MCon.addressMode[0] = cudaAddressModeClamp;
  // texDesc_MCon.addressMode[1] = cudaAddressModeClamp;
  // texDesc_MCon.filterMode = cudaFilterModePoint;
  // texDesc_MCon.readMode = cudaReadModeElementType;
  // texDesc_MCon.normalizedCoords = 0;  // 不使用归一化坐标

  // // Create texture object
  // cudaCreateTextureObject(&texObj_MCon, &resDesc_MCon, &texDesc_MCon, NULL);

  // Invoke kernel
  // dim3 threadsPerBlock(16, 16);
  // dim3 numBlocks((kNumTabs + threadsPerBlock.x - 1) / threadsPerBlock.x,
  //                (kNumTabs + threadsPerBlock.y - 1) / threadsPerBlock.y);
  // std::cout << kNumTabs << std::endl;
  // std::cout << numBlocks.x << " " << numBlocks.y << " " << numBlocks.z
  //           << std::endl;
  // std::cout << threadsPerBlock.x << " " << threadsPerBlock.y << " "
  //           << threadsPerBlock.z << std::endl;
  // transformKernel<<<numBlocks, threadsPerBlock>>>(texObj_MCon, kNumTabs,
  //                                                 kNumTabs);

  // 初始化数据
  LOG(INFO) << "-----neighbour-----";
  cudaMalloc(&d_ConNeighbor, sizeof(int) * kNumTabs * kNumVars);
  // 它的最内维应是约束，外维是变量，但由于纹理内存的维度是相反的，所以这里是反的
  // 行是变量，列是约束
  int h_ConNeighbor[kNumTabs * kNumVars] = {};

  for (int i = 0; i < kNumVars; ++i) {
    // auto ci = xm->Tabs(i);
    auto v = xm->Vars(i);
    for (const auto& c : xm->subscriptions[v]) {
      h_ConNeighbor[i * kNumVars + c->Id()] = 1;
    }
  }

  // 打印初始化后的数据
  for (size_t i = 0; i < kNumTabs * kNumTabs; i++) {
    std::cout << h_ConNeighbor[i] << " ";
  }
  std::cout << std::endl;

  cudaMemcpy(d_ConNeighbor, h_ConNeighbor, sizeof(int) * kNumTabs * kNumVars,
             cudaMemcpyHostToDevice);

  // Allocate CUDA array in device memory
  cudaChannelFormatDesc channelDesc = cudaCreateChannelDesc<int>();
  cudaMallocArray(&cuArray_MCon, &channelDesc, kNumTabs,
                  kNumVars);  // 注意这里是 kNumTabs 行，kNumVars 列

  // Copy data to device memory
  const size_t spitch = kNumTabs * sizeof(int);
  cudaMemcpy2DToArray(cuArray_MCon, 0, 0, h_ConNeighbor, spitch,
                      kNumTabs * sizeof(int), kNumVars, cudaMemcpyHostToDevice);
  cudaDeviceSynchronize();

  // Specify texture resource
  memset(&resDesc_MCon, 0, sizeof(resDesc_MCon));
  resDesc_MCon.resType = cudaResourceTypeArray;
  resDesc_MCon.res.array.array = cuArray_MCon;

  // Specify texture object parameters
  memset(&texDesc_MCon, 0, sizeof(texDesc_MCon));
  texDesc_MCon.addressMode[0] = cudaAddressModeClamp;
  texDesc_MCon.addressMode[1] = cudaAddressModeClamp;
  texDesc_MCon.filterMode = cudaFilterModePoint;
  texDesc_MCon.readMode = cudaReadModeElementType;
  texDesc_MCon.normalizedCoords = 0;  // 不使用归一化坐标

  // Create texture object
  cudaCreateTextureObject(&texObj_MCon, &resDesc_MCon, &texDesc_MCon, NULL);
  cudaDeviceSynchronize();

  // Invoke kernel
  transformKernel<<<1, kNumTabs>>>(texObj_MCon, kNumTabs, kNumVars);
  LOG(INFO) << "-----texture-----";
#pragma endregion 约束网络信息

#pragma region 拷贝bitDom和Assigned
  LOG(INFO) << "-----bitDom-----";
  // 现在少了一个回溯维度，所以不需要考虑回溯的问题
  // cudaMallocManaged(&bitDom, sizeof(u32) * BITDOMS_INTSIZE);
  // cudaMallocManaged(&M_VarPre, sizeof(int) * VS_SIZE);
  // 在主机上分配h_bitDom
  h_bitDom = (u32*)malloc(sizeof(u32) * kAllBitDomsIntSize);
  if (h_bitDom != nullptr) {
    memset(h_bitDom, 0u, sizeof(u32) * kAllBitDomsIntSize);
  }
  // h_current_domain_size = (int*)malloc(sizeof(int) * kDepth * kNumVars);
  // d_assigned = (i32x2*)malloc(sizeof(i32x2) * kNumVars);
  // if (h_current_domain_size != nullptr) {
  //   memset(h_current_domain_size, 0, sizeof(int) * kDepth * kNumVars);
  // }

  h_cur_dom_size.resize(kNumVars * kDepth, 0);

  d_solution = (i32*)malloc(sizeof(i32) * kNumVars);
  if (d_solution != nullptr) {
    memset(d_solution, -1, sizeof(i32) * kNumVars);
  }
  // d_assigned = (i32x2*)malloc(sizeof(i32x2) * kNumVars);
  cudaMalloc((void**)&d_assigned, sizeof(i32x2) * kNumVars);
  // cudaMallocManaged(&d_bitDom, sizeof(u32) * kAllBitDomsIntSize);
  // cudaMallocManaged(&M_VarPre, sizeof(int) * kNumVars);
  // d_assigned_at_level = (i32*)malloc(sizeof(i32) * kNumVars);

  // 初始化第0层h_bitDom和h_current_domain_size
  for (int i = 0; i < kNumVars; ++i) {
    const HVar v = xm->Vars(i);
    const int dom_size = static_cast<int>(v->vals.size());
    // h_current_domain_size[i] = dom_size;
    h_cur_dom_size[i] = dom_size;
    const int dom_int_size = intsizeof(dom_size);

    for (int j = 0; j < kBitDomIntSize; ++j) {
      const int idx = GetBitDomByIndex(i, j);
      if (j < dom_int_size - 1)
        h_bitDom[idx] = UINT32_MAX;
      else if (j == dom_int_size - 1)
        h_bitDom[idx] = UINT32_MAX >> GetOffSet(dom_size);
      else
        h_bitDom[idx] = 0;
    }
  }

  // 在设备上分配d_bitDom
  cudaMalloc((void**)&d_bitDom, sizeof(u32) * kAllBitDomsIntSize);
  // cudaMalloc((void**)&d_current_domain_size, sizeof(int) * kDepth *
  // kNumVars);

  // 将h_bitDom复制到d_bitDom
  cudaMemcpy(d_bitDom, h_bitDom, sizeof(u32) * kAllBitDomsIntSize,
             cudaMemcpyHostToDevice);
  // cudaMemcpy(d_current_domain_size, h_current_domain_size,
  //            sizeof(int) * kDepth * kNumVars, cudaMemcpyHostToDevice);

  d_cur_dom_size = h_cur_dom_size;
  // d
  //  // 初始化bitDom
  //  for (int i = 0; i < kNumVars; ++i) {
  //    const HVar v = xm->Vars(i);
  //    const int dom_size = v->vals.size();
  //    // 当前变量的实际INT长度
  //    const int dom_int_size = intsizeof(dom_size);
  //
  //    for (int j = 0; j < kBitDomIntSize; ++j) {
  //      const int idx = GetBitDomByIndex(i, j);
  //      //  三种情况
  //      if (j < dom_int_size - 1)
  //        d_bitDom[idx] = UINT32_MAX;
  //      else if (j == dom_int_size - 1)
  //        d_bitDom[idx] = UINT32_MAX >> GetOffSet(dom_size);
  //      else
  //        d_bitDom[idx] = 0;
  //    }
  //  }

  // for (int i = 0; i < kNumVars; ++i) {
  //   for (int j = 0; j < kBitDomIntSize; ++j) {
  //     int idx = GetBitDomByIndex(i, j);
  //     printf("var = %d, j = %d, idx = %d, bitDom = %x\n", i, j, idx,
  //            h_bitDom[idx]);
  //   }
  // }
#pragma endregion 拷贝bitDom

#pragma region 创建bitSubDom
  LOG(INFO) << "-----bitSubDom-----";
  // 现在少了一个回溯维度，所以不需要考虑回溯的问题
  // cudaMalloc(&bitSubDom,
  //                   sizeof(u32) * BITDOMS_INTSIZE * VS_SIZE * MAX_DOM_SIZE);
  cudaMallocManaged(&bitSubDom,
                    sizeof(u32) * kBitDomsIntSize * kNumVars * kMaxDomSize);
  for (int i = 0; i < kNumVars; ++i) {
    for (int j = 0; j < kMaxDomSize; ++j) {
      const int start_idx = GetBitSubDomStartIndex(i, j);
      for (int k = 0; k < kBitSubDomsIntSize; ++k) {
        bitSubDom[start_idx + k] = h_bitDom[k];
      }
      // (i,j,i,j),子问题(i,j)的singleton问题：
      // (i,j)值需要singleton即清空bitSubDom[i,j,i]，
      // 并设置bitSubDom[i,j,i,j]=1bit
      // 做法：  获取i,j,i的起始地址，
      //        将bitSubDom从ijistart到ijistart+kBitDomIntSize的范围清零
      //        设置bitSubDom[i,j,i,j]的第j位为1
      const int iji_start_idx = start_idx + i * kBitDomIntSize;
      for (int k = 0; k < kBitDomIntSize; ++k) bitSubDom[iji_start_idx + k] = 0;
      BITSET_SET((bitSubDom + iji_start_idx), j);
    }
  }

  // for (int i = 0; i < kNumVars; ++i) {
  //   for (int j = 0; j < kMaxDomSize; ++j) {
  //     printf("sub problem:(%d, %d): ", i, j);
  //     const int start_idx = GetBitSubDomStartIndex(i, j);
  //     for (int k = 0; k < kBitDomsIntSize; ++k) {
  //       printf("%x ", bitSubDom[start_idx + k]);
  //     }
  //     printf("\n");
  //   }
  // }

#pragma endregion 创建bitSubDom

#pragma region 拷贝bitSup
  printf("-------------bitSup-------------\n");
  // cudaMallocManaged(&bitSup, sizeof(uint2) * kBitSupsIntSize);
  // cudaMalloc(&d_bitSup, sizeof(uint2) * BITSUPS_INTSIZE);
  auto* h_bitSup = new uint2[kBitSupsIntSize]();

  // 填充bitSup
  for (int i = 0; i < kNumTabs; ++i) {
    const HTab c = xm->Tabs(i);
    // 仅适用于二元约束，支持语义
    if (c->Arity() != 2)
      throw std::invalid_argument("Only support binary constraint.");
    if (!c->semantics)
      throw std::invalid_argument("Only support support semantics.");

    // 维度是[e,d,d/w]
    for (int j = 0; j < c->tuples.size(); ++j) {
      const int2 t = make_int2(c->tuples[j][0], c->tuples[j][1]);
      const int2 idx = GetBitSupIndexByTuple_C_MDINTS_MDS(c->id, t);
      BITSET_SET(reinterpret_cast<uint32_t*>(&h_bitSup[idx.x].x), t.y);
      BITSET_SET(reinterpret_cast<uint32_t*>(&h_bitSup[idx.y].y), t.x);
    }
  }

  // printf("----------h_bitSup---------\n");
  // for (int i = 0; i < kNumTabs; ++i) {
  //   for (int j = 0; j < kBitDomIntSize; ++j) {
  //     printf("c_id = %d, j = %d: ", i, j);
  //     for (int k = 0; k < kMaxDomSize; ++k) {
  //       const int idx = GetBitSupIndexByINTPrstn(i, j, k);
  //       printf("%x, %x | ", h_bitSup[idx].x, h_bitSup[idx].y);
  //     }
  //     printf("\n");
  //   }
  // }

  printf("---------bitSup texture---------\n");
  // 分配3D CUDA数组内存
  // cudaChannelFormatDesc channelDesc3D = cudaCreateChannelDesc<uint2>();
  // cudaExtent extent = make_cudaExtent(kNumTabs, kNumVars, kBitDomIntSize);
  // cudaMalloc3DArray(&cuArray3D, &channelDesc3D, extent);
  // udaExtent extent = make_cudaExtent(a, b, c);
  // 这里a是最内维，b是次内维，c是最外维
  // 维度变换后是: kMaxDomSize, kBitDomIntSize, kNumTabs -> kNumTabs, kNumVars,
  // kBitDomIntSize
  cudaChannelFormatDesc channelDesc3D = cudaCreateChannelDesc<uint2>();
  cudaExtent extent = make_cudaExtent(kMaxDomSize, kBitDomIntSize, kNumTabs);
  cudaMalloc3DArray(&cuArray3D, &channelDesc3D, extent);

  // 将3D数据复制到设备内存
  cudaMemcpy3DParms copyParams = {0};
  copyParams.srcPtr = make_cudaPitchedPtr(h_bitSup, kMaxDomSize * sizeof(uint2),
                                          kMaxDomSize, kBitDomIntSize);
  copyParams.dstArray = cuArray3D;
  copyParams.extent = extent;
  copyParams.kind = cudaMemcpyHostToDevice;
  cudaMemcpy3D(&copyParams);
  cudaDeviceSynchronize();

  // 设置3D纹理资源描述符
  memset(&resDesc3D, 0, sizeof(resDesc3D));
  resDesc3D.resType = cudaResourceTypeArray;
  resDesc3D.res.array.array = cuArray3D;

  // 设置3D纹理对象描述符
  memset(&texDesc3D, 0, sizeof(texDesc3D));
  texDesc3D.addressMode[0] = cudaAddressModeClamp;
  texDesc3D.addressMode[1] = cudaAddressModeClamp;
  texDesc3D.addressMode[2] = cudaAddressModeClamp;
  texDesc3D.filterMode = cudaFilterModePoint;
  texDesc3D.readMode = cudaReadModeElementType;
  texDesc3D.normalizedCoords = 0;

  // 创建3D纹理对象
  cudaCreateTextureObject(&texObj_BitSup, &resDesc3D, &texDesc3D, NULL);
  //
  // // 调用内核函数
  // dim3 threadsPerBlock3(8, 8, 8);
  // dim3 numBlocks3((kNumTabs + threadsPerBlock.x - 1) / threadsPerBlock.x,
  //                 (kNumVars + threadsPerBlock.y - 1) / threadsPerBlock.y,
  //                 (kBitDomIntSize + threadsPerBlock.z - 1) /
  //                 threadsPerBlock.z);
  //
  // transformKernel3D<<<numBlocks3, threadsPerBlock3>>>(
  //     texObj_BitSup, kMaxDomSize, kBitDomIntSize, kNumTabs);
  cudaDeviceSynchronize();

  delete[] h_bitSup;
  printf("==================bitSup==================\n");
#pragma endregion 拷贝bitSup

#pragma region 生成约束
  // 初始化h_MCon信息
  thrust::host_vector<u32x3> h_MCon(kNumTabs);

  for (int i = 0; i < kNumTabs; ++i) {
    auto c = xm->Tabs(i);
    h_MCon[i] = make_uint3(xm->Tabs(i)->scope[0]->id, xm->Tabs(i)->scope[1]->id,
                           xm->Tabs(i)->id);
  }

  // 将 M_Con 的数据拷贝到 d_MCon
  d_MCon = h_MCon;
  d_MConEvt.reserve(kNumTabs);
  d_ConPre.resize(kNumTabs, 1);
  h_subscription_offset.resize(kNumVars + 1, 0);
  int offset = 0;
  for (size_t i = 0; i < kNumVars; i++) {
    auto v = xm->Vars(i);
    h_subscription_offset[i] = offset;
    offset += xm->subscriptions[v].size();
    // h_subscription_offset[i] = xm->subscriptions[v].size();
    for (const auto& c : xm->subscriptions[v]) {
      h_subscription.push_back(
          make_uint3(c->scope[0]->id, c->scope[1]->id, c->id));
      printf("c_id = %d, v1_id = %d, v2_id = %d\n", c->id, c->scope[0]->id,
             c->scope[1]->id);
    }
    printf("-------------------\n");
    // offset += h_subscription.size();
  }
  // 填充最后一个标记位
  h_subscription_offset[kNumVars] = offset;
  d_subscription_offset = h_subscription_offset;
  d_subscription = h_subscription;

  // // 验证数据是否正确拷贝
  // for (int i = 0; i < kNumTabs; ++i) {
  //   uint3 val = d_MC
  // on[i];
  //   std::cout << "d_MCon[" << i << "] = (" << val.x << ", " << val.y << ", "
  //             << val.z << "): " << d_ConPre[i] << "\n";
  // }

  // // 验证数据是否正确拷贝
  // for (int i = 0; i < kNumTabs; ++i) {
  //   uint3 val = d_MCon[i];
  //   std::cout << "d_MCon[" << i << "] = (" << val.x << ", " << val.y << ", "
  //             << val.z << "): " << d_ConPre[i] << "\n";
  // }
#pragma endregion 生成约束
#pragma region 子问题约束队列
//   cudaMallocManaged(&S_ConPre, sizeof(int) * SUBCON_SIZE);
//   cudaMallocManaged(&S_ConEvt, sizeof(int3) * SUBCON_SIZE);
//   cudaMallocManaged(&S_Con, sizeof(int3) * SUBCON_SIZE);
//   cudaMallocManaged(&S_VarPre, sizeof(int) * VS_SIZE * MAX_DOM_SIZE *
//   VS_SIZE); cudaMallocManaged(&S_Var, sizeof(int3) * VS_SIZE * MAX_DOM_SIZE
//   * VS_SIZE);
//
//   for (int i = 0; i < VS_SIZE; ++i) {
//     const HVar v = xm->Vars(i);
//     for (int j = 0; j < MAX_DOM_SIZE; ++j) {
//       for (int k = 0; k < CS_SIZE; ++k) {
//         // 子问题(i, j) k为约束id
//         const int idx = (i * MAX_DOM_SIZE + j) * CS_SIZE + k;
//         // i*xm->feature.max_dom_size*xm->feature.cs_size +
//         // j*xm->feature.cs_size + k;
//
//         S_Con[idx].x = i;
//         S_Con[idx].y = j;
//         S_Con[idx].z = k;
//
//         S_ConEvt[idx].x = i;
//         S_ConEvt[idx].y = j;
//         S_ConEvt[idx].z = k;
//
//         S_ConPre[idx] = 1;
//         printf("S_Con = (%d, %d, %d), S_ConEvt = (%d, %d, %d), pre = %d\n",
//         S_Con[idx].x, S_Con[idx].y, S_Con[idx].z, S_ConEvt[idx].x,
//         S_ConEvt[idx].y, S_ConEvt[idx].z, S_ConPre[idx]);
//       }
//
//       for (int k = 0; k < VS_SIZE; ++k) {
//         // 子问题(i, j) k为变量id
//         const int idx = (i * MAX_DOM_SIZE + j) * VS_SIZE + k;
//         S_Var[idx].x = i;
//         S_Var[idx].y = j;
//         S_Var[idx].z = k;
//
//         S_VarPre[idx] = 1;
//
//         printf("S_Var = (%d, %d, %d), S_VarPre = %d\n", S_Var[idx].x,
//                S_Var[idx].y, S_Var[idx].z, S_VarPre[idx]);
//       }
//     }
//   }
#pragma endregion 子问题约束队列
  //
  // #pragma region 程序运行规格
  //   // 获得主问题压缩的BLOCK数
  //   MCC_BLOCK = GetTopNum(CS_SIZE, num_threads);
  //   MCC_BCount.resize(MCC_BLOCK, 0);
  //   MCC_BOffset.resize(MCC_BLOCK, 0);
  //   MCC_BlocksCount = thrust::raw_pointer_cast(MCC_BCount.data());
  //   MCC_BlocksOffset = thrust::raw_pointer_cast(MCC_BOffset.data());
  // #pragma endregion
}

int CModel::CreateNewLevel() {
  current_level_++;

  // 计算源地址和目标地址
  u32* src = d_bitDom + (current_level_ - 1) * kBitDomsIntSize;
  u32* dst = d_bitDom + current_level_ * kBitDomsIntSize;

  // 计算需要复制的字节数
  size_t copySize = kBitDomsIntSize * sizeof(u32);
  printf("xixi~ at level %d\n", current_level_);
  bitDomCopy();
  // 使用 cudaMemcpy 进行复制操作
  cudaMemcpy(dst, src, copySize, cudaMemcpyDeviceToDevice);
  printf("xixi2~ at level %d\n", current_level_);

  bitDomCopy();
  // 复制 d_cur_dom_size 的新一层值
  thrust::copy(d_cur_dom_size.begin() + (current_level_ - 1) * kNumVars,
               d_cur_dom_size.begin() + current_level_ * kNumVars,
               d_cur_dom_size.begin() + current_level_ * kNumVars);

  // 返回当前级别
  return current_level_;
}

void CModel::initialGPUConstant() {
  // 将 CPU 数据复制到 GPU 常量内存
  // cudaMemcpyToSymbol(kDeviceU32Mask1, U32_MASK1, sizeof(U32_MASK1));
  // cudaMemcpyToSymbol(kDeviceU32Mask0, U32_MASK0, sizeof(U32_MASK0));
  cudaMemcpyToSymbol(kDeviceBitDomIntSize, &kBitDomIntSize, sizeof(int));
  cudaMemcpyToSymbol(kDeviceBitDomsIntSize, &kBitDomsIntSize, sizeof(int));
  cudaMemcpyToSymbol(kDeviceMaxDomSize, &kMaxDomSize, sizeof(int));
  cudaMemcpyToSymbol(kDeviceNumTabs, &kNumTabs, sizeof(int));
  cudaMemcpyToSymbol(kDeviceNumVars, &kNumVars, sizeof(int));
  cudaMemcpyToSymbol(kDeviceMaxDomSize, &kMaxDomSize, sizeof(int));
  cudaMemcpyToSymbol(kDeviceDomSize,
                     thrust::raw_pointer_cast(h_dom_size.data()),
                     kNumVars * sizeof(int));
  cudaMemcpyToSymbol(kDeviceBitSupIntSize, &kBitSupIntSize, sizeof(int));
  cudaMemcpyToSymbol(kDeviceBitSupsIntSize, &kBitSupsIntSize, sizeof(int));
  cudaMemcpyToSymbol(kDeviceBitSubDomsIntSize, &kBitSubDomsIntSize,
                     sizeof(int));
  cudaMemcpyToSymbol(kDeg, thrust::raw_pointer_cast(h_Deg.data()),
                     kNumVars * sizeof(int));
}

bool CModel::enforceGAC() {
  printf("-----------enforeGAC-----------\n");
  int num_ConEvt = compress_Main();
  cudaDeviceSynchronize();
  while (num_ConEvt != 0) {
    printf("-----------iteration-----------\n");
    CsCheckMain<<<num_ConEvt, dim3(kBitDomIntSize * 32, 1, 1),
                  kSharedMemSize>>>(
        thrust::raw_pointer_cast(d_ConPre.data()),
        thrust::raw_pointer_cast(d_MCon.data()), d_bitDom,
        thrust::raw_pointer_cast(d_cur_dom_size.data()), texObj_BitSup,
        texObj_MCon, num_ConEvt, current_level_);
    cudaDeviceSynchronize();
    if (!GAC_success) {
      return false;
    }
    printf("-----------end iteration-----------\n");
    // std::cout << "h_ConPre: ";
    // thrust::host_vector<int> h_ConPre = d_ConPre;
    // for (size_t i = 0; i < h_ConPre.size(); ++i) {
    //   std::cout << h_ConPre[i] << " ";
    // }
    // std::cout << std::endl;

    num_ConEvt = compress_Main();
    // return true;
  }

  return true;
}

bool CModel::enforceGAC(int var, int type) {
  printf("-----------enforeGAC for disicion-----------\n");
  int num_ConEvt = h_subscription_offset[var + 1] - h_subscription_offset[var];
  // 填充最后一个标记位
  h_subscription_offset[kNumVars] = h_subscription.size();
  printf("h_subscription_offset: ");
  for (auto i : h_subscription_offset) {
    std::cout << i << " ";
  }
  std::cout << std::endl;

  for (int i = h_subscription_offset[var]; i < h_subscription_offset[var + 1];
       ++i) {
    int c_id = h_subscription[i].z;
    int v1_id = h_subscription[i].x;
    int v2_id = h_subscription[i].y;
    printf("c_id = %d, v1_id = %d, v2_id = %d\n", c_id, v1_id, v2_id);
  }

  printf("num_ConEvt = %d\n", num_ConEvt);
  // return false;
  CsCheckMainAfterDecision<<<num_ConEvt, dim3(kBitDomIntSize * 32, 1, 1),
                             kSharedMemSize>>>(
      thrust::raw_pointer_cast(d_ConPre.data()),
      thrust::raw_pointer_cast(d_MCon.data()),
      thrust::raw_pointer_cast(d_subscription.data()),
      thrust::raw_pointer_cast(d_subscription_offset.data()), d_bitDom,
      thrust::raw_pointer_cast(d_cur_dom_size.data()), texObj_BitSup,
      texObj_MCon, num_ConEvt, d_assigned, var, type, current_level_);
  cudaDeviceSynchronize();
  h_cur_dom_size = d_cur_dom_size;
  printf("h_cur_dom_size at level %d:\n", current_level_);
  for (size_t i = 0; i < kNumVars; i++) {
    int j = i + kNumVars * current_level_;
    printf("%d ", h_cur_dom_size[j]);
  }
  printf("\n");
  cout << "GAC_success = " << GAC_success << endl;
  // return false;
  num_ConEvt = compress_Main();
  printf("num_ConEvt: %d\n", num_ConEvt);
  cudaDeviceSynchronize();
  while (num_ConEvt != 0) {
    printf("-----------iteration-----------\n");
    CsCheckMain<<<num_ConEvt, dim3(kBitDomIntSize * 32, 1, 1),
                  kSharedMemSize>>>(
        thrust::raw_pointer_cast(d_ConPre.data()),
        thrust::raw_pointer_cast(d_MCon.data()), d_bitDom,
        thrust::raw_pointer_cast(d_cur_dom_size.data()), texObj_BitSup,
        texObj_MCon, num_ConEvt, current_level_);
    cudaDeviceSynchronize();
    if (!GAC_success) {
      return false;
    }
    printf("-----------end iteration-----------\n");
    // std::cout << "h_ConPre: ";
    // thrust::host_vector<int> h_ConPre = d_ConPre;
    // for (size_t i = 0; i < h_ConPre.size(); ++i) {
    //   std::cout << h_ConPre[i] << " ";
    // }
    // std::cout << std::endl;

    num_ConEvt = compress_Main();
    // return true;
  }

  return true;
}

//
//
// bool CModel::enforceGAC() {
//   printf("-------------------enforceGAC-------------------\n");
//   int sharedMemSize =
//       (2 * kBitDomIntSize + 1) * sizeof(u32);  // 动态共享内存大小
//   // // printf("d_ConPre.size = %lu\n", d_ConPre.size());
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
  cudaMemcpy(h_bitDom, d_bitDom, sizeof(u32) * kAllBitDomsIntSize,
             cudaMemcpyDeviceToHost);
  printf("bitdom: \n");
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
    printf("%s\n", binary_str.c_str());
  }
  printf("\n");
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
    printf("varid = %d\n", varid);
    if (varid == kNumVars) {
      finished_ = true;
      statistics_.solve_time = t.elapsed();
      return statistics_;
    }

    printf("before assign value at level %d\n", current_level_);
    bitDomCopy();
    cudaDeviceSynchronize();
    CreateNewLevel();
    printf("after assign2 value at level %d\n", current_level_);
    cudaDeviceSynchronize();
    bitDomCopy();
    cudaDeviceSynchronize();
    AssignValue<<<1, kBitDomIntSize * 32>>>(
        d_assigned, d_bitDom, thrust::raw_pointer_cast(d_cur_dom_size.data()),
        varid, current_level_);
    cudaDeviceSynchronize();
    enforceGAC(varid, 1);
    printf("after assign value at level %d\n", current_level_);
    cudaDeviceSynchronize();
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
  LOG(INFO) << "CModel析构函数";
  // cudaFree(d_bitSup);
  cudaFree(d_ConNeighbor);
  cudaFree(d_bitDom);
  free(h_bitDom);
  // cudaFree(M_VarPre);
  // cudaFree(d_current_domain_size);
  // free(h_current_domain_size);
  cudaFree(d_assigned);
  cudaFree(d_solution);
  // cudaFree(d_assigned_at_level);

  cudaFree(bitSubDom);
  // cudaFree(bitSup);
  // cudaFree(M_Con);
  // cudaFree(M_ConEvt);
  // cudaFree(M_ConPre);
  cudaFree(S_ConPre);
  cudaFree(S_ConEvt);
  cudaFree(S_Con);
  cudaFree(S_Var);
  cudaFree(S_VarPre);

  // 析构纹理内存
  cudaDestroyTextureObject(texObj_MCon);
  cudaFreeArray(cuArray_MCon);

  cudaDestroyTextureObject(texObj_BitSup);
  cudaFreeArray(cuArray3D);

  // free(h_Deg);
}

void CModel::DelGPUModel() const {
  // cudaFree(scope);
}

}  // namespace cpim
