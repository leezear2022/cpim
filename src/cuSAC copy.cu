#include <glog/logging.h>
#include <thrust/extrema.h>
#include <thrust/functional.h>

#include <cfloat>

#include "cuSAC.cuh"

namespace cpim {

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

// 全局传播是否成功，默认值是true
__managed__ int GAC_success = true;

// TODO: 这里未来可以改成纯device内存
__managed__ u32* bitDom;
__managed__ u32* bitSubDom;

__managed__ int* S_VarPre;
__managed__ uint3* S_Var;
__managed__ int* S_ConPre;
__managed__ int3* S_ConEvt;
__managed__ int3* S_Con;
// 刚刚用于赋值的变量值设备上的
__device__ int assigned_var;
__device__ int assigned_val;


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
      kBitSubDomsIntSize(kNumVars * kMaxDomSize * kBitDomsIntSize) {
  dom_size.resize(kNumVars);
  for (int i = 0; i < kNumVars; ++i) {
    const HVar v = xm->Vars(i);
    dom_size[i] = v->vals.size();
  }
  // 初始化GPU常量
  initialGPUConstant();
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
  int x = blockIdx.x * blockDim.x + threadIdx.x;
  int y = blockIdx.y * blockDim.y + threadIdx.y;

  if (x < width && y < height) {
    // 访问纹理内存
    int value = tex2D<int>(texObj, x, y);
    // 打印值
    printf("Texture value at (%d, %d): %d\n", x, y, value);
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

// 每个线程对应一个论域
__global__ void CsCheckMain(i32* mConPre, const u32x3* mCon, u32* bitDom,
                            const i32* dom_size, cudaTextureObject_t bitSup,
                            cudaTextureObject_t neiCon, int num_ConEvt) {
  // mCon索引ID
  const int bid = blockIdx.x;
  // bitDom索引
  const int a_0 = threadIdx.x;
  // bitDom索引
  const int a_1 = threadIdx.y;
  // 块内全局线程索引
  const int tid = threadIdx.x + threadIdx.y * blockDim.x;

  // printf("bid: %d, a_0: %d, a_1: %d, num_ConEvt: %d\n", bid, a_0, a_1,
  //        num_ConEvt);

  // 每个线程都拿到当前约束信息
  auto c = mCon[bid];
  const int xid = c.x;
  const int yid = c.y;
  const int cid = c.z;

  // 论域大小
  // int xsize = dom_size[xid];
  // int ysize = dom_size[yid];

  // 动态分配共享内存，共三段：
  // 1. bitDom[x]
  // 2. bitDom[y]
  // 3. empty_dom，用于标记失败，默认值是1，即假设它失败，
  //    在最后检查的时候，如有一段word非空则置为0。
  extern __shared__ u32 shared_mem[];
  u32* s_bitDom_x = shared_mem;
  u32* s_bitDom_y = &shared_mem[kDeviceBitDomIntSize];
  u32* empty_dom = &shared_mem[2 * kDeviceBitDomIntSize];
  if (a_0 == 0 && a_1 == 0) empty_dom[0] = 1;

  // 把bitDom写入两段共享内存
  if (a_0 < kDeviceBitDomIntSize && a_1 == 0) {
    s_bitDom_x[a_0] = bitDom[DeviceGetBitDomByIndex(xid, a_0)];
    s_bitDom_y[a_0] = bitDom[DeviceGetBitDomByIndex(yid, a_0)];
    // printf(
    //     "----cid: %d, a_0: %d, a_1: %d, mConPre: %d, s_bitDom_x: %x, "
    //     "s_bitDom_y: %x\n",
    //     cid, a_0, a_1, mConPre[cid], s_bitDom_x[a_0], s_bitDom_y[a_0]);
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
  bool changed = false;
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
        changed = true;
        u32 oldVal =
            atomicAnd(&bitDom[DeviceGetBitDomByIndex(xid, bitIdx)], vote_x);
        // if (oldVal & vote_x != 0) empty_dom[0] = 0;
        printf("v: %d, bitDom = %x, now = %x\n", xid, vote_x,
               bitDom[DeviceGetBitDomByIndex(xid, bitIdx)]);
      }

      if (s_bitDom_y[bitIdx] ^ vote_y) {
        changed = true;
        auto oldVal =
            atomicAnd(&bitDom[DeviceGetBitDomByIndex(yid, bitIdx)], vote_y);
        // if (oldVal & vote_y != 0) empty_dom[0] = 0;
        // printf("a_0: %d, a_1: %d, empty_dom: %d\n", a_0, a_1, empty_dom[0]);
        printf("v: %d, bitDom = %x, now = %x\n", yid, vote_y,
               bitDom[DeviceGetBitDomByIndex(yid, bitIdx)]);
      }
    }
  }

  __syncthreads();
  // printf("changed: %d\n", changed);
  // return;
  // 再load一次检查是否为0
  if (a_0 < kDeviceBitDomIntSize && a_1 == 0) {
    if (bitDom[DeviceGetBitDomByIndex(xid, a_0)] != 0) empty_dom[0] = 0;
    if (bitDom[DeviceGetBitDomByIndex(yid, a_0)] != 0) empty_dom[0] = 0;
  }
  __syncthreads();
  if (a_0 == 0 && a_1 == 0 && empty_dom[0] == 1) {
    GAC_success = 0;
    // printf("a_0: %d, a_1: %d, empty_dom: %d\n", a_0, a_1, empty_dom[0]);
  }
  __syncthreads();
  // printf("GAC_success: %d\n", GAC_success);
  // propagate changed to neighbour constraints
  if (GAC_success && changed) {
    for (int idx = tid; idx < kDeviceNumTabs; idx += blockDim.x * blockDim.y) {
      auto val = tex2D<int>(neiCon, idx, cid);
      // printf("cid: %d, tid: %d = %d\n", cid, tid, val);
      if (val != 0) {
        mConPre[idx] = 1;
      }
    }
  }

  // 局部写回全局内存

  // // 启动规模
  //
  // // 从全局内存加载bitSup到本地内存
  // // 这里需要根据启动规模，判断一下怎么么读取bitSup
  // uint2 bitSup_cid = tex3D<uint2>(bitSup, a_0, 0 ~kbitDomIntSize, cid);
  // // 通过这个我们知道它要了几个轮读写
  // kDeviceBitDomIntSize
  //
  //     // l_sum = make_uint2(0, 0);
  //
  //     if (tid < D_MDS) l_sum = bitSup[GetBitSupIdxDevice(s_scp.z, tid)];
  // __syncthreads();
  //
  // // 归约
  // l_sum.x &= s_bitDom.y;
  // l_sum.y &= s_bitDom.x;
  //
  // // 投票并反转
  // l_res.x = __brev(__ballot(l_sum.x));
  // l_res.y = __brev(__ballot(l_sum.y));
  //
  // __syncthreads();
  // if (tid == 0) {
  //   // 存入全局内存,并记录改变
  //   l_res.x &= s_bitDom.x;
  //   if (s_bitDom.x != l_res.x) {
  //     atomicAnd(&bitDom[s_scp.x], l_res.x);
  //     mVarPre[s_scp.x] = 1;
  //
  //     if (bitDom[s_scp.x] == 0) mVarPre[s_scp.x] = INT_MIN;
  //   }
  //
  //   l_res.y &= s_bitDom.y;
  //   if (s_bitDom.y != l_res.y) {
  //     atomicAnd(&bitDom[s_scp.y], l_res.y);
  //     mVarPre[s_scp.y] = 1;
  //
  //     if (bitDom[s_scp.y] == 0) mVarPre[s_scp.y] = INT_MIN;
  //   }
  // }
}
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
  LOG(INFO) << "-----neighbour-----";
  cudaMalloc(&d_ConNeighbor, sizeof(int) * kNumTabs * kNumTabs);
  int h_ConNeighbor[kNumTabs * kNumTabs] = {};

  for (int i = 0; i < kNumTabs; ++i) {
    auto ci = xm->Tabs(i);
    for (int j = 0; j < kNumTabs; ++j) {
      auto cj = xm->Tabs(j);
      if (HModel::is_neighbour(ci, cj) && i != j) {
        h_ConNeighbor[i * kNumTabs + j] = 1;  // 简单初始化
      } else {
        h_ConNeighbor[i * kNumTabs + j] = 0;
      }
    }
  }

  // // 打印初始化后的数据
  // for (size_t i = 0; i < kNumTabs * kNumTabs; i++) {
  //   std::cout << h_ConNeighbor[i] << " ";
  // }
  // std::cout << std::endl;

  cudaMemcpy(d_ConNeighbor, h_ConNeighbor, sizeof(int) * kNumTabs * kNumTabs,
             cudaMemcpyHostToDevice);

  // Allocate CUDA array in device memory
  cudaChannelFormatDesc channelDesc = cudaCreateChannelDesc<int>();
  cudaMallocArray(&cuArray_MCon, &channelDesc, kNumTabs, kNumTabs);

  // Copy data to device memory
  const size_t spitch = kNumTabs * sizeof(int);
  cudaMemcpy2DToArray(cuArray_MCon, 0, 0, h_ConNeighbor, spitch,
                      kNumTabs * sizeof(int), kNumTabs, cudaMemcpyHostToDevice);
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
  cudaDeviceSynchronize();
  // LOG(INFO) << "-----texture-----";
#pragma endregion 约束网络信息

#pragma region 拷贝bitDom
  LOG(INFO) << "-----bitDom-----";
  // 现在少了一个回溯维度，所以不需要考虑回溯的问题
  // cudaMallocManaged(&bitDom, sizeof(u32) * BITDOMS_INTSIZE);
  // cudaMallocManaged(&M_VarPre, sizeof(int) * VS_SIZE);
  cudaMallocManaged(&bitDom, sizeof(u32) * kBitDomsIntSize);
  // cudaMallocManaged(&M_VarPre, sizeof(int) * kNumVars);

  // 初始化bitDom
  for (int i = 0; i < kNumVars; ++i) {
    const HVar v = xm->Vars(i);
    const int dom_size = v->vals.size();
    // 当前变量的实际INT长度
    const int dom_int_size = intsizeof(dom_size);

    for (int j = 0; j < kBitDomIntSize; ++j) {
      const int idx = GetBitDomByIndex(i, j);
      //  三种情况
      if (j < dom_int_size - 1)
        bitDom[idx] = UINT32_MAX;
      else if (j == dom_int_size - 1)
        bitDom[idx] = UINT32_MAX >> GetOffSet(dom_size);
      else
        bitDom[idx] = 0;
    }
  }

  for (int i = 0; i < kNumVars; ++i) {
    for (int j = 0; j < kBitDomIntSize; ++j) {
      int idx = GetBitDomByIndex(i, j);
      printf("var = %d, j = %d, idx = %d, bitDom = %x\n", i, j, idx,
             bitDom[idx]);
    }
  }
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
        bitSubDom[start_idx + k] = bitDom[k];
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
  thrust::host_vector<uint3> h_MCon(kNumTabs);
  for (int i = 0; i < kNumTabs; ++i) {
    auto c = xm->Tabs(i);
    h_MCon[i] = make_uint3(xm->Tabs(i)->scope[0]->id, xm->Tabs(i)->scope[1]->id,
                           xm->Tabs(i)->id);
  }

  // 将 M_Con 的数据拷贝到 d_MCon
  d_MCon = h_MCon;
  d_MConEvt.reserve(kNumTabs);
  d_ConPre.resize(kNumTabs, 1);

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
  cudaMemcpyToSymbol(kDeviceDomSize, thrust::raw_pointer_cast(dom_size.data()),
                     kNumVars * sizeof(int));
  cudaMemcpyToSymbol(kDeviceBitSupIntSize, &kBitSupIntSize, sizeof(int));
  cudaMemcpyToSymbol(kDeviceBitSupsIntSize, &kBitSupsIntSize, sizeof(int));
  cudaMemcpyToSymbol(kDeviceBitSubDomsIntSize, &kBitSubDomsIntSize,
                     sizeof(int));
}

bool CModel::enforceGAC() {
  printf("-------------------enforceGAC-------------------\n");
  int sharedMemSize =
      (2 * kBitDomIntSize + 1) * sizeof(u32);  // 动态共享内存大小
  // printf("d_ConPre.size = %lu\n", d_ConPre.size());
  GAC_success = 1;
  // // 1. 压缩约束
  int num_ConEvt = compress_Main();
  // // 2. 检查约束
  // // if(kBitDomIntSize==3||kBitDomIntSize==4) {
  // //
  // CsCheckMain<<<num_ConEvt,dim3(kBitDomIntSize*32,1,1)>>>(thrust::raw_pointer_cast(d_ConPre.data()),thrust::raw_pointer_cast(d_MCon.data()),bitDom,thrust::raw_pointer_cast(dom_size.data()),texObj_BitSup);
  // // }else if(kBitDomIntSize>=5||kBitDomIntSize<=8){
  // //
  // CsCheckMain<<<num_ConEvt,dim3(kBitDomIntSize*32,1,1)>>>(thrust::raw_pointer_cast(d_ConPre.data()),thrust::raw_pointer_cast(d_MCon.data()),bitDom,thrust::raw_pointer_cast(dom_size.data()),texObj_BitSup);
  // // }
  // // // CsCheckMain<<<num_ConEvt,kBitDomIntSize*32>>>
  // // 将 d_MConEvt 数据从设备复制到主机
  // // thrust::host_vector<uint3> h_MConEvt = d_MConEvt;
  // // // 还原回去
  // //
  // // // 在主机上打印数据
  // // for (size_t i = 0; i < num_ConEvt; ++i) {
  // //   std::cout << "h_MConEvt[" << i << "] = ("
  // //             << h_MConEvt[i].x << ", "
  // //             << h_MConEvt[i].y << ", "
  // //             << h_MConEvt[i].z << ")\n";
  // // }
  CsCheckMain<<<num_ConEvt, dim3(kBitDomIntSize * 32, 1, 1), sharedMemSize>>>(
      thrust::raw_pointer_cast(d_ConPre.data()),
      thrust::raw_pointer_cast(d_MCon.data()), bitDom,
      thrust::raw_pointer_cast(dom_size.data()), texObj_BitSup, texObj_MCon,
      num_ConEvt);

  // int num_ConEvt = compress_Main();
  // while (num_ConEvt != 0) {
  //   CsCheckMain<<<num_ConEvt, dim3(kBitDomIntSize * 32, 1, 1),
  //   sharedMemSize>>>(
  //       thrust::raw_pointer_cast(d_ConPre.data()),
  //       thrust::raw_pointer_cast(d_MCon.data()), bitDom,
  //       thrust::raw_pointer_cast(dom_size.data()), texObj_BitSup,
  //       texObj_MCon, num_ConEvt);
  //   if (GAC_success) {
  //     return false;
  //   }
  //   num_ConEvt = compress_Main();
  // }
  return true;
}

void CModel::enforceSAC() {}

void CModel::solve() {}

CModel::~CModel() {
  LOG(INFO) << "CModel析构函数";
  // cudaFree(d_bitSup);
  cudaFree(d_ConNeighbor);
  cudaFree(bitDom);
  // cudaFree(M_VarPre);
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
}

void CModel::DelGPUModel() const {
  // cudaFree(scope);
}

}  // namespace cpim
