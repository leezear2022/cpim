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

__constant__ u32 kDeviceU32Mask1[32];
__constant__ u32 kDeviceU32Mask0[32];

__constant__ int D_NUM_BD_BLOCK;
__constant__ int D_NUM_CS_SIZE_BLOCKS;
__device__ __managed__ int NUM_BD_BLOCK;
__device__ __managed__ int NUM_CS_SIZE_BLOCKS;
__device__ __managed__ int BITDOM_INTSIZE;
__device__ __managed__ int BITDOMS_INTSIZE;
__device__ __managed__ int BITSUP_INTSIZE;
__device__ __managed__ int BITSUPS_INTSIZE;
__device__ __managed__ int BITSUBDOMS_INTSIZE;
__device__ __managed__ int VS_SIZE;
__device__ __managed__ int CS_SIZE;
__device__ __managed__ int MAX_ARITY;
__device__ __managed__ int MCC_BLOCK;
__device__ __managed__ int M_Qsize;

// __device__ __managed__ int* vars_size;
// __device__ __managed__ int3* scope;
__device__ __managed__ int MAX_DOM_SIZE;
// __device__ __managed__ int SUBCON_SIZE;

__device__ __managed__ u32* bitDom;
__device__ __managed__ uint2* bitSup;
__device__ __managed__ u32* bitSubDom;

// thrust::device_vector<uint3> M_Con;
// thrust::device_vector<uint3> M_ConEvt;
// thrust::device_vector<int> M_ConPre;
__device__ __managed__ int* M_ConPre;
__device__ __managed__ int* M_VarPre;
__device__ __managed__ uint3* M_ConEvt;
__device__ __managed__ uint3* M_Con;
__device__ __managed__ int* S_VarPre;
__device__ __managed__ uint3* S_Var;
__device__ __managed__ int* S_ConPre;
__device__ __managed__ int3* S_ConEvt;
__device__ __managed__ int3* S_Con;

// int* MCC_BlocksCount;
// int* MCC_BlocksOffset;
//
// thrust::device_vector<int> MCC_BCount;
// thrust::device_vector<int> MCC_BOffset;

// extern const u32 U32_MASK1[32] =

int intsizeof(const int nbits) {
  return ((nbits + kBitsPerWord - 1) / kBitsPerWord);
}

// int2 CModel::GetBitSupIndexByTuple_C_MDS_MDINTS(int cid, int2 t) {
//   return make_int2(
//       cid * BITSUP_INTSIZE + t.x * BITDOM_INTSIZE + (t.y >> U32_POS),
//       cid * BITSUP_INTSIZE + t.y * BITDOM_INTSIZE + (t.x >> U32_POS));
// }

////////////////////////////////  CModel  ////////////////////////////////

// int CModel::GetBitSupIndexByCID(const int cid) {
//   return cid * BITSUP_INTSIZE;
// }
// // c, (x, a), (y, a)
// // t.x = x, a
// // t.y = y, a
// // 若维度是[e][d][d/w],因此索引的计算公式如下:
// int2 CModel::GetBitSupIndexByTuple_C_MDS_MDINTS(const int cid, const int2 t)
// {
//   return make_int2(
//       cid * BITSUP_INTSIZE + t.x * BITDOM_INTSIZE + (t.y >> U32_POS),
//       cid * BITSUP_INTSIZE + t.y * BITDOM_INTSIZE + (t.x >> U32_POS));
// }
//
// // c, (x, a), (y, a)
// // t.x = x, a
// // t.y = y, a
// // 若维度是[e][d/w][d],因此索引的计算公式如下:
// int2 CModel::GetBitSupIndexByTuple_C_MDINTS_MDS(const int cid, const int2 t)
// {
//   return make_int2(
//       cid * BITSUP_INTSIZE + (t.y >> U32_POS) * MAX_DOM_SIZE + t.x,
//       cid * BITSUP_INTSIZE + (t.x >> U32_POS) * MAX_DOM_SIZE + t.y);
// }

CModel::CModel(const HModel& xm)
    : kNumVars(xm->Vars().size()),
      kNumTabs(xm->Tabs().size()),
      kMaxDomSize(xm->max_domain_size()),
      kBitDomIntSize(intsizeof(xm->max_domain_size())),
      kBitDomsIntSize(kBitDomIntSize * kNumVars),
      kBitSupIntSize(kMaxDomSize * kBitDomIntSize),
      kBitSupsIntSize(kMaxDomSize * kBitDomIntSize * kNumTabs),
      kBitSubDomsIntSize(kNumVars * kMaxDomSize * kBitDomsIntSize) {
  dom_size.resize(kNumVars);
  for (int i = 0; i < kNumVars; ++i) {
    const HVar v = xm->Vars(i);
    dom_size[i] = v->vals.size();
  }

  // 初始化GPU常量
  initialCPUConstant();
  // 初始化GPU数据
  BuildBitModel(xm);
}

// int2  CModel::GetBitSupIndexByTuple(const int cid, const int2 t) {

// int GetBitSupIndexById(int cid) { return cid * BITSUP_INTSIZE; }
__global__ void exampleKernel(uint3* MCon, int size) {
  int idx = threadIdx.x + blockIdx.x * blockDim.x;
  if (idx < size) {
    uint3 val = MCon[idx];
    printf("d_MCon[%d] = (%u, %u, %u)\n", idx, val.x, val.y, val.z);
  }
}

// 一个示例 kernel 函数，使用常量内存
__global__ void exampleKernelUMask() {
  int idx = threadIdx.x;
  if (idx < 32) {
    printf("kU32Mask1[%d] = 0x%x, kU32Mask0[%d] = 0x%x\n", idx,
           kDeviceU32Mask1[idx], idx, kDeviceU32Mask0[idx]);
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
    uint2 value3D = tex3D<uint2>(texObj3D, x, y, z);
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
__global__ void CsCheckMain(i32* mConPre, const i32x3* mCon, int* mVarPre,
                            u32* bitDom, const i32* dom_size,
                            cudaTextureObject_t bitSup) {
  // 约束ID
  const int cid = blockIdx.x;
  // bitDom索引
  const int a_0 = threadIdx.x;
  // bitDom索引
  const int a_1 = threadIdx.y;
  // 此约束活动置0
  if (a_0 == 0 && a_1 == 0) mConPre[cid] = 0;

  uint2 l_res = make_uint2(0, 0);
  // 每个线程都拿到当前约束信息
  i32x3 c = mCon[cid];
  int xid = c.x;
  int yid = c.y;
  // 论域大小
  int xsize = dom_size[xid];
  int ysize = dom_size[yid];

  // bitDom 当前(x,a)(y,a)是否存在
  // int l_xa = 0, l_ya = 0;
  __shared__ u32 s_bitDom_x[kDeviceBitDomIntSize];
  __shared__ u32 s_bitDom_y[kDeviceBitDomIntSize];
  // 把共享内存写入bitDom
  // 还原回去
  if (a_0 < kDeviceMaxDomSize * 32) {
    s_bitDom_x[a_0] = bitDom[DeviceGetBitDomByIndex(xid, a_0)];
    s_bitDom_y[a_0] = bitDom[DeviceGetBitDomByIndex(yid, a_0)];
  }
  __syncthreads();
  // 取当前值(x, a_0)是否有效
  // 取当前值(y, a_0)是否有效
  int l_xa = BITSET_GET(s_bitDom_x, a_0);
  int l_ya = BITSET_GET(s_bitDom_y, a_0);

  u32 val_x = 0;
  u32 val_y = 0;
  // 取得cid里支持(x, a_0)的bitDom->bitSup[c][a_1][a_0]->bitSup[c][~][a],
  // TODO:这里有问题，没有进行好块内归约，我需要按threadIdx.y的对数步长归约
  if (kDeviceBitDomIntSize == 1) {
    // Case 1: MaxDomSize \in (0,32]
    auto bitSup_cid = tex3D<uint2>(bitSup, a_0, 0, cid);
    val_x |= l_xa && (bitSup_cid.x & s_bitDom_y[0]);
    val_y |= l_ya && (bitSup_cid.y & s_bitDom_x[0]);
  }

  __syncthreads();
  // 只有threadIdx.x的那一维度归约
  if (a_1 == 0) {
    // 线程束内投票
    unsigned int vote_x = __ballot_sync(0xFFFFFFFF, val_x == 0);
    unsigned int vote_y = __ballot_sync(0xFFFFFFFF, val_y == 0);
    // 只是线程束里的第一个线程做如下操作：
    // 只写回自己那块bitDom
    // 先获取bitDom的分块索引
    // 先与共享内存里的bitDom比较有改变才写回
    if (a_0 % warpSize == 0) {
      int bitIdx = a_0 / 32;
      if (s_bitDom_x[bitIdx] ^ vote_x) {
        mConPre[cid] = 1;
        atomicAnd(&bitDom[DeviceGetBitDomByIndex(xid, bitIdx)], vote_x);
      }

      if (s_bitDom_y[bitIdx] ^ vote_y) {
        mConPre[cid] = 1;
        atomicAnd(&bitDom[DeviceGetBitDomByIndex(yid, bitIdx)], vote_y);
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

void compress_Main(const thrust::device_vector<uint3>& d_MCon,
                   const thrust::device_vector<int>& d_ConPre,
                   thrust::device_vector<uint3>& d_MConEvt) {
  d_MConEvt.resize(d_MCon.size());
  // 使用 thrust::copy_if 进行流压缩
  auto end = thrust::copy_if(d_MCon.begin(), d_MCon.end(),  // 输入范围
                             d_ConPre.begin(),   // 输入范围的判断条件
                             d_MConEvt.begin(),  // 输出范围
                             is_one()            // 判断条件
  );

  // 调整 d_MConEvt 的大小以匹配实际复制的元素数
  d_MConEvt.resize(thrust::distance(d_MConEvt.begin(), end));
}

void CModel::BuildBitModel(const HModel& xm) {
#pragma region 计算常量
  // 变量个数
  VS_SIZE = xm->Vars().size();
  // 约束个数
  CS_SIZE = xm->Tabs().size();
  // 约束最大元数
  MAX_ARITY = xm->max_arity();
  // 最大变量论域大小
  MAX_DOM_SIZE = xm->max_domain_size();
  // 计算有多少个int可以表示，一个bitDom[x]
  BITDOM_INTSIZE = intsizeof(MAX_DOM_SIZE);
  // 总bitDom长度，一个子问题的所有bitDoms的长度
  BITDOMS_INTSIZE = BITDOM_INTSIZE * VS_SIZE;
  // 一个bitSup的int长度
  // TODO: 这里看情况可能可乘个2, 现在这里暂不乘，因为用了int2的数据类型
  BITSUP_INTSIZE = MAX_DOM_SIZE * BITDOM_INTSIZE;
  // 所有bitSup的int长度
  BITSUPS_INTSIZE = BITSUP_INTSIZE * CS_SIZE;
  // 总长度
  BITSUBDOMS_INTSIZE = VS_SIZE * MAX_DOM_SIZE * BITDOMS_INTSIZE;
  // SUBCON_SIZE = VS_SIZE * MAX_DOM_SIZE * CS_SIZE;
#pragma endregion 计算常量
#pragma region 约束网络信息
  // cudaMallocManaged(&vars_size, sizeof(int) * VS_SIZE);
  //// 初始化变量域大小
  // for (int i = 0; i < xm->feature.vs_size; ++i)
  //{
  //	XVar* v = xm->vars[i];
  //	XDom* d = xm->doms[v->dom_id];
  //	vars_size[i] = d->size;
  // }

  // 初始化scope
  // cudaMallocManaged(&scope, sizeof(int3) * CS_SIZE);
  // for (int i = 0; i < CS_SIZE; ++i) {
  //   const HTab c = xm->Tabs(i);
  //   // XCon* c = xm->cons[i];
  //   scope[i].x = c->scope[0]->id;
  //   scope[i].y = c->scope[1]->id;
  //   scope[i].z = c->id;
  // }

  // 显示
  // for (int i = 0; i < CS_SIZE; ++i) {
  //   printf("scope[%d] = {%d, %d}\n", scope[i].z, scope[i].x, scope[i].y);
  // }
  // 这里存一些类内的常量什么的吧

  // 初始化数据
  std::cout << "-----texture-----" << std::endl;
  cudaMalloc(&d_ConNeighbor, sizeof(int) * kNumTabs * kNumTabs);
  int h_ConNeighbor[kNumTabs * kNumTabs] = {};

  for (int i = 0; i < kNumTabs; ++i) {
    for (int j = 0; j < kNumTabs; ++j) {
      h_ConNeighbor[i * kNumTabs + j] = i + j;  // 简单初始化
    }
  }

  // 打印初始化后的数据
  for (size_t i = 0; i < kNumTabs * kNumTabs; i++) {
    std::cout << h_ConNeighbor[i] << " ";
  }
  std::cout << std::endl;

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
  dim3 threadsPerBlock(16, 16);
  dim3 numBlocks((kNumTabs + threadsPerBlock.x - 1) / threadsPerBlock.x,
                 (kNumTabs + threadsPerBlock.y - 1) / threadsPerBlock.y);
  std::cout << kNumTabs << std::endl;
  std::cout << numBlocks.x << " " << numBlocks.y << " " << numBlocks.z
            << std::endl;
  std::cout << threadsPerBlock.x << " " << threadsPerBlock.y << " "
            << threadsPerBlock.z << std::endl;
  transformKernel<<<numBlocks, threadsPerBlock>>>(texObj_MCon, kNumTabs,
                                                  kNumTabs);
  cudaDeviceSynchronize();
  std::cout << "-----texture-----" << std::endl;

  // std::cout << "-----texture-----" << std::endl;
  // cudaMalloc(&d_ConNeighbor, sizeof(int) * num_tabs * num_tabs);
  // int h_ConNeighbor[num_tabs * num_tabs] = {};

  // for (int i = 0; i < num_tabs; ++i) {
  //   const auto c = xm->Tabs(i);
  //   // subscriptions
  //   for (const auto& v : c->scope) {
  //     for (const auto& cc : xm->subscriptions[v]) {
  //       h_ConNeighbor[i * CS_SIZE + cc->id] = 1;
  //       h_ConNeighbor[cc->id * CS_SIZE + i] = 1;
  //     }
  //   }
  // }

  // for (size_t i = 0; i < num_tabs * num_tabs; i++) {
  //   /* code */
  //   std::cout << h_ConNeighbor[i] << " ";
  // }
  // std::cout << std::endl;

  // cudaMemcpy(d_ConNeighbor, h_ConNeighbor, sizeof(int) * num_tabs * num_tabs,
  //            cudaMemcpyHostToDevice);

  // // Allocate CUDA array in device memory
  // cudaChannelFormatDesc channelDesc = cudaCreateChannelDesc<int>();
  // cudaMallocArray(&cuArray, &channelDesc, num_tabs, num_tabs);

  // // Copy data to device memory
  // const size_t spitch = num_tabs * sizeof(int);
  // cudaMemcpy2DToArray(cuArray, 0, 0, h_ConNeighbor, spitch,
  //                     num_tabs * sizeof(int), num_tabs,
  //                     cudaMemcpyHostToDevice);
  // cudaDeviceSynchronize();
  // // Specify texture resource
  // memset(&resDesc, 0, sizeof(resDesc));
  // resDesc.resType = cudaResourceTypeArray;
  // resDesc.res.array.array = cuArray;

  // // Specify texture object parameters
  // memset(&texDesc, 0, sizeof(texDesc));
  // texDesc.addressMode[0] = cudaAddressModeWrap;
  // texDesc.addressMode[1] = cudaAddressModeWrap;
  // texDesc.filterMode = cudaFilterModeLinear;
  // texDesc.readMode = cudaReadModeElementType;
  // texDesc.normalizedCoords = 1;

  // // Create texture object
  // cudaCreateTextureObject(&texObj, &resDesc, &texDesc, NULL);

  // // Invoke kernel
  // dim3 threadsPerBlock(16, 16);
  // dim3 numBlocks((num_tabs + threadsPerBlock.x - 1) / threadsPerBlock.x,
  //                (num_tabs + threadsPerBlock.y - 1) / threadsPerBlock.y);
  // std::cout << num_tabs << std::endl;
  // std::cout << numBlocks.x << numBlocks.y << numBlocks.z << std::endl;
  // std::cout << threadsPerBlock.x << threadsPerBlock.y << threadsPerBlock.z
  //           << std::endl;
  // transformKernel<<<numBlocks, threadsPerBlock>>>(texObj, num_tabs,
  // num_tabs); cudaDeviceSynchronize(); std::cout << "-----texture-----" <<
  // std::endl;
#pragma endregion 约束网络信息

#pragma region 拷贝bitDom
  // 现在少了一个回溯维度，所以不需要考虑回溯的问题
  cudaMallocManaged(&bitDom, sizeof(u32) * BITDOMS_INTSIZE);
  cudaMallocManaged(&M_VarPre, sizeof(int) * VS_SIZE);

  // 初始化bitDom
  for (int i = 0; i < VS_SIZE; ++i) {
    const HVar v = xm->Vars(i);
    const int dom_size = v->vals.size();
    // 当前变量的实际INT长度
    const int dom_int_size = intsizeof(dom_size);

    for (int j = 0; j < BITDOM_INTSIZE; ++j) {
      const int idx = GetBitDomByIndex(i, j);
      // printf("idx = %d\n", idx);
      //  三种情况
      if (j < dom_int_size - 1)
        bitDom[idx] = UINT32_MAX;
      else if (j == dom_int_size - 1)
        bitDom[idx] = UINT32_MAX << GetOffSet(dom_size);
      else
        bitDom[idx] = 0;
    }

    M_VarPre[i] = 1;
  }

  for (int i = 0; i < VS_SIZE; ++i) {
    for (int j = 0; j < BITDOM_INTSIZE; ++j) {
      int idx = GetBitDomByIndex(i, j);
      printf("var = %d, j = %d, idx = %d, bitDom = %x, pre= %x\n", i, j, idx,
             bitDom[idx], M_VarPre[i]);
    }
  }
#pragma endregion 拷贝bitDom

#pragma region 创建bitSubDom
  // 现在少了一个回溯维度，所以不需要考虑回溯的问题
  cudaMallocManaged(&bitSubDom,
                    sizeof(u32) * BITDOMS_INTSIZE * VS_SIZE * MAX_DOM_SIZE);
  for (int i = 0; i < VS_SIZE; ++i) {
    for (int j = 0; j < MAX_DOM_SIZE; ++j) {
      const int start_idx = GetBitSubDomStartIndex(i, j);
      for (int k = 0; k < BITSUBDOMS_INTSIZE; ++k)
        bitSubDom[start_idx + k] = bitDom[k];
      // 最后将(i,j)的bitDom 改掉
      // 获取i,j,i的起始地址，
      const int ijistart = start_idx + i * BITDOM_INTSIZE;
      for (int k = 0; k < BITDOM_INTSIZE; ++k)
        // j在索引K的范围内:j/32,将第j%32位置为1
        if (k == j >> U32_POS)
          bitSubDom[ijistart + k] = U32_MASK1[j & U32_MOD_MASK];
        // 其它位置为0
        else
          bitSubDom[ijistart + k] = 0;
    }
  }

  for (int i = 0; i < VS_SIZE; ++i) {
    for (int j = 0; j < MAX_DOM_SIZE; ++j) {
      printf("sub problem:(%d, %d): ", i, j);
      const int start_idx = GetBitSubDomStartIndex(i, j);
      for (int k = 0; k < BITDOMS_INTSIZE; ++k) {
        printf("%x ", bitSubDom[start_idx + k]);
      }
      printf("\n");
    }
  }

#pragma endregion 创建bitSubDom

#pragma region 拷贝bitSup
  printf("-------------bitSup-------------\n");
  cudaMallocManaged(&bitSup, sizeof(uint2) * BITSUPS_INTSIZE);
  // cudaMalloc(&d_bitSup, sizeof(uint2) * BITSUPS_INTSIZE);
  auto* h_bitSup = new uint2[BITSUPS_INTSIZE]();

  // 填充bitSup
  for (int i = 0; i < CS_SIZE; ++i) {
    const HTab c = xm->Tabs(i);
    // 仅适用于二元约束，支持语义
    if (c->Arity() != 2)
      throw std::invalid_argument("Only support binary constraint.");
    if (!c->semantics)
      throw std::invalid_argument("Only support support semantics.");
    // 向位矩阵中填充值，这里假设bitSup和h_bitSup的维度顺序是不一样的
    // 维度是[e,d,d/w]
    for (int j = 0; j < c->tuples.size(); ++j) {
      const int2 t = make_int2(c->tuples[j][0], c->tuples[j][1]);
      const int2 idx = GetBitSupIndexByTuple_C_MDS_MDINTS(c->id, t);
      bitSup[idx.x].x |= U32_MASK1[t.y & U32_MOD_MASK];
      bitSup[idx.y].y |= U32_MASK1[t.x & U32_MOD_MASK];
    }

    // 维度是[e,d,d/w]
    for (int j = 0; j < c->tuples.size(); ++j) {
      const int2 t = make_int2(c->tuples[j][0], c->tuples[j][1]);
      const int2 idx = GetBitSupIndexByTuple_C_MDINTS_MDS(c->id, t);
      h_bitSup[idx.x].x |= U32_MASK1[t.y & U32_MOD_MASK];
      h_bitSup[idx.y].y |= U32_MASK1[t.x & U32_MOD_MASK];
    }
  }

  printf("----------bitSup---------\n");
  for (int i = 0; i < CS_SIZE; ++i) {
    for (int j = 0; j < MAX_DOM_SIZE; ++j) {
      printf("c_id = %d, j = %d: ", i, j);
      for (int k = 0; k < BITDOM_INTSIZE; ++k) {
        const int idx = GetBitSupIndexByINTPrstn(i, j, k);
        printf("%x, %x", bitSup[idx].x, bitSup[idx].y);
      }
      printf("\n");
    }
  }

  printf("----------h_bitSup---------\n");
  for (int i = 0; i < CS_SIZE; ++i) {
    for (int j = 0; j < BITDOM_INTSIZE; ++j) {
      printf("c_id = %d, j = %d: ", i, j);
      for (int k = 0; k < MAX_DOM_SIZE; ++k) {
        const int idx = GetBitSupIndexByINTPrstn(i, j, k);
        // printf("%x, %x", bitSup[idx].x, bitSup[idx].y);
        printf("%x, %x | ", h_bitSup[idx].x, h_bitSup[idx].y);
      }
      printf("\n");
    }
  }
  // cudaMemcpy(d_bitSup, h_bitSup, sizeof(uint2) * BITSUPS_INTSIZE,
  //            cudaMemcpyDeviceToDevice);

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
  cudaCreateTextureObject(&texObj3D, &resDesc3D, &texDesc3D, NULL);

  // 调用内核函数
  dim3 threadsPerBlock3(8, 8, 8);
  dim3 numBlocks3((kNumTabs + threadsPerBlock.x - 1) / threadsPerBlock.x,
                  (kNumVars + threadsPerBlock.y - 1) / threadsPerBlock.y,
                  (kBitDomIntSize + threadsPerBlock.z - 1) / threadsPerBlock.z);

  transformKernel3D<<<numBlocks3, threadsPerBlock3>>>(texObj3D, kMaxDomSize,
                                                      kBitDomIntSize, kNumTabs);
  cudaDeviceSynchronize();

  delete[] h_bitSup;
  printf("==================bitSup==================\n");
#pragma endregion 拷贝bitSup

#pragma region 生成约束
  cudaMallocManaged(&M_Con, sizeof(uint3) * CS_SIZE);
  cudaMallocManaged(&M_ConEvt, sizeof(uint3) * CS_SIZE);
  cudaMallocManaged(&M_ConPre, sizeof(int) * CS_SIZE);
  // thrust::host_vector<uint3>
  // thrust::host_vector<uint3> HMCon(CS_SIZE);
  // M_Con.resize(CS_SIZE);
  // M_ConEvt.reserve(CS_SIZE);
  // M_ConPre.resize(CS_SIZE, 1);
  // // cudaMallocManaged(&M_ConPre, sizeof(int) * CS_SIZE);
  // //
  for (int i = 0; i < CS_SIZE; ++i) {
    auto c = xm->Tabs(i);
    M_Con[i] = make_uint3(xm->Tabs(i)->scope[0]->id, xm->Tabs(i)->scope[1]->id,
                          xm->Tabs(i)->id);
    M_ConPre[i] = 1;
  }

  d_MCon.resize(CS_SIZE);
  // 将 M_Con 的数据拷贝到 d_MCon
  thrust::copy(M_Con, M_Con + CS_SIZE, d_MCon.begin());
  d_MConEvt.reserve(CS_SIZE);
  d_ConPre.resize(CS_SIZE, 1);

  // 验证数据是否正确拷贝
  for (int i = 0; i < CS_SIZE; ++i) {
    uint3 val = d_MCon[i];
    std::cout << "d_MCon[" << i << "] = (" << val.x << ", " << val.y << ", "
              << val.z << ")\n";
  }

  printf("-------\n");
  uint3* d_MCon_ptr = thrust::raw_pointer_cast(d_MCon.data());

  int threadsPerBlock2 = 32;
  int blocksPerGrid = (CS_SIZE + threadsPerBlock2 - 1) / threadsPerBlock2;
  exampleKernel<<<blocksPerGrid, threadsPerBlock2>>>(d_MCon_ptr, CS_SIZE);

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

void CModel::initialCPUConstant() {
  // 将 CPU 数据复制到 GPU 常量内存
  cudaMemcpyToSymbol(kDeviceU32Mask1, U32_MASK1, sizeof(U32_MASK1));
  cudaMemcpyToSymbol(kDeviceU32Mask0, U32_MASK0, sizeof(U32_MASK0));
  cudaMemcpyToSymbol(kDeviceBitDomIntSize, &kBitDomIntSize, sizeof(int));
  cudaMemcpyToSymbol(kDeviceBitDomsIntSize, &kBitDomsIntSize, sizeof(int));
  cudaMemcpyToSymbol(kDeviceMaxDomSize, &kMaxDomSize, sizeof(int));
  cudaMemcpyToSymbol(kDeviceNumTabs, &kNumTabs, sizeof(int));
  cudaMemcpyToSymbol(kDeviceNumVars, &kNumVars, sizeof(int));
  cudaMemcpyToSymbol(kDeviceMaxDomSize, &kMaxDomSize, sizeof(int));
  cudaMemcpyToSymbol(kDeviceDomSize, thrust::raw_pointer_cast(dom_size.data()), kNumVars * sizeof(int));
  cudaMemcpyToSymbol(kDeviceBitSupIntSize, &kBitSupIntSize, sizeof(int));
  cudaMemcpyToSymbol(kDeviceBitSupsIntSize, &kBitSupsIntSize, sizeof(int));
  cudaMemcpyToSymbol(kDeviceBitSubDomsIntSize, &kBitSubDomsIntSize, sizeof(int));

}

CModel::~CModel() {
  std::cout << "CModel析构函数" << std::endl;
  // cudaFree(d_bitSup);
  cudaFree(d_ConNeighbor);
  cudaFree(bitDom);
  cudaFree(M_VarPre);
  cudaFree(bitSubDom);
  cudaFree(bitSup);
  cudaFree(M_Con);
  cudaFree(M_ConEvt);
  cudaFree(M_ConPre);
  cudaFree(S_ConPre);
  cudaFree(S_ConEvt);
  cudaFree(S_Con);
  cudaFree(S_Var);
  cudaFree(S_VarPre);

  // Destroy texture object
  cudaDestroyTextureObject(texObj_MCon);
  // Free device memory
  cudaFreeArray(cuArray_MCon);
  // cudaFree(d_output);

  // 销毁纹理对象
  cudaDestroyTextureObject(texObj3D);
  // 释放设备内存
  cudaFreeArray(cuArray3D);
}

void CModel::DelGPUModel() const {
  // cudaFree(scope);
}

}  // namespace cpim
