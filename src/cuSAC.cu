#include "cuSAC.cuh"

namespace cpim {

// 初始化常量
__constant__ int D_BITDOM_INTSIZE;
__constant__ int D_BITDOMS_INTSIZE;
__constant__ int D_NUM_BD_BLOCK;
__constant__ int D_NUM_CS_SIZE_BLOCKS;
__constant__ u32 kU32Mask1[32];
__constant__ u32 kU32Mask0[32];

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
  return ((nbits + BITS_PER_WORD - 1) / BITS_PER_WORD);
}
int CModel::GetBitSupIndexById(const int cid) const {
  return cid * BITSUP_INTSIZE;
}
int2 CModel::GetBitSupIndexByTuple(int cid, int2 t) {
  return make_int2(
      cid * BITSUP_INTSIZE + t.x * BITDOM_INTSIZE + (t.y >> U32_POS),
      cid * BITSUP_INTSIZE + t.y * BITDOM_INTSIZE + (t.x >> U32_POS));
}

////////////////////////////////  CModel  ////////////////////////////////

CModel::CModel(const HModel& xm)
    : num_vars(xm->Vars().size()),
      num_tabs(xm->Tabs().size()),
      max_dom_size(xm->max_domain_size()) {
  // 初始化常量
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
    printf("kU32Mask1[%d] = 0x%x, kU32Mask0[%d] = 0x%x\n", idx, kU32Mask1[idx],
           idx, kU32Mask0[idx]);
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
  // TODO: 这里看情况可能可乘个2, 现在这里暂不乘
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
  cudaMalloc(&d_ConNeighbor, sizeof(int) * num_tabs * num_tabs);
  int h_ConNeighbor[num_tabs * num_tabs] = {};

  for (int i = 0; i < num_tabs; ++i) {
    for (int j = 0; j < num_tabs; ++j) {
      h_ConNeighbor[i * num_tabs + j] = i + j;  // 简单初始化
    }
  }

  // 打印初始化后的数据
  for (size_t i = 0; i < num_tabs * num_tabs; i++) {
    std::cout << h_ConNeighbor[i] << " ";
  }
  std::cout << std::endl;

  cudaMemcpy(d_ConNeighbor, h_ConNeighbor, sizeof(int) * num_tabs * num_tabs,
             cudaMemcpyHostToDevice);

  // Allocate CUDA array in device memory
  cudaChannelFormatDesc channelDesc = cudaCreateChannelDesc<int>();
  cudaMallocArray(&cuArray, &channelDesc, num_tabs, num_tabs);

  // Copy data to device memory
  const size_t spitch = num_tabs * sizeof(int);
  cudaMemcpy2DToArray(cuArray, 0, 0, h_ConNeighbor, spitch,
                      num_tabs * sizeof(int), num_tabs, cudaMemcpyHostToDevice);
  cudaDeviceSynchronize();

  // Specify texture resource
  memset(&resDesc, 0, sizeof(resDesc));
  resDesc.resType = cudaResourceTypeArray;
  resDesc.res.array.array = cuArray;

  // Specify texture object parameters
  memset(&texDesc, 0, sizeof(texDesc));
  texDesc.addressMode[0] = cudaAddressModeClamp;
  texDesc.addressMode[1] = cudaAddressModeClamp;
  texDesc.filterMode = cudaFilterModePoint;
  texDesc.readMode = cudaReadModeElementType;
  texDesc.normalizedCoords = 0;  // 不使用归一化坐标

  // Create texture object
  cudaCreateTextureObject(&texObj, &resDesc, &texDesc, NULL);

  // Invoke kernel
  dim3 threadsPerBlock(16, 16);
  dim3 numBlocks((num_tabs + threadsPerBlock.x - 1) / threadsPerBlock.x,
                 (num_tabs + threadsPerBlock.y - 1) / threadsPerBlock.y);
  std::cout << num_tabs << std::endl;
  std::cout << numBlocks.x << " " << numBlocks.y << " " << numBlocks.z
            << std::endl;
  std::cout << threadsPerBlock.x << " " << threadsPerBlock.y << " "
            << threadsPerBlock.z << std::endl;
  transformKernel<<<numBlocks, threadsPerBlock>>>(texObj, num_tabs, num_tabs);
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
      const int idx = GetBitDomIndex(i, j);
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
      int idx = GetBitDomIndex(i, j);
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
  cudaMallocManaged(&bitSup, sizeof(uint2) * BITSUPS_INTSIZE);
  cudaMalloc(&d_bitSup, sizeof(uint2) * BITSUPS_INTSIZE);
  for (int i = 0; i < CS_SIZE; ++i) {
    const HTab c = xm->Tabs(i);
    // 仅适用于二元约束
    if (c->Arity() != 2)
      throw std::invalid_argument("Only support binary constraint.");
    // 现在的约束元组都是支持的
    const std::array<HVar, 2> v = {c->scope[0], c->scope[1]};
    const u64 dom_size[2] = {v[0]->vals.size(), v[1]->vals.size()};

    // 初始化位矩阵
    for (int j = 0; j < MAX_DOM_SIZE; ++j) {
      for (int k = 0; k < BITDOM_INTSIZE; ++k) {
        const int idx = GetBitSupIndexByINTPrstn(c->id, j, k);
        if (j < dom_size[0] && (k < (dom_size[1] >> U32_POS))) {
          // 支持取0x0000..., 冲突取0xFFF...
          bitSup[idx].x = (!c->semantics) ? UINT32_MAX : 0;
          bitSup[idx].y = (!c->semantics) ? UINT32_MAX : 0;
        } else if (k == (v[1]->vals.size() >> U32_POS)) {
          bitSup[idx].x = (!c->semantics) ? UINT32_MAX : 0;
          bitSup[idx].y = (!c->semantics) ? UINT32_MAX : 0;
          bitSup[idx].x <<= U32_BIT - (dom_size[1] & U32_MOD_MASK);
          bitSup[idx].y <<= U32_BIT - (dom_size[1] & U32_MOD_MASK);
        } else {
          bitSup[idx].x = 0;
          bitSup[idx].y = 0;
        }
      }
    }

    // 向位矩阵中填充值
    for (int j = 0; j < c->tuples.size(); ++j) {
      const int2 t = make_int2(c->tuples[j][0], c->tuples[j][1]);
      // printf("c_id= %d, %d, %d\n", c->id, t.x, t.y);
      const int2 idx = GetBitSupIndexByTuple(c->id, t);
      // printf("idx = %d, %d\n", idx.x, idx.y);
      if (c->semantics) {
        bitSup[idx.x].x |= U32_MASK1[t.y & U32_MOD_MASK];
        bitSup[idx.y].y |= U32_MASK1[t.x & U32_MOD_MASK];
      } else {
        bitSup[idx.x].x &= U32_MASK0[t.y & U32_MOD_MASK];
        bitSup[idx.y].y &= U32_MASK0[t.x & U32_MOD_MASK];
      }
    }

    cudaMemcpy(d_bitSup, bitSup, sizeof(uint2) * BITSUPS_INTSIZE,
               cudaMemcpyDeviceToDevice);

    for (int j = 0; j < MAX_DOM_SIZE; ++j) {
      printf("c_id = %d, j = %d: ", i, j);
      for (int k = 0; k < BITDOM_INTSIZE; ++k) {
        const int idx = GetBitSupIndexByINTPrstn(c->id, j, k);
        printf("%x, %x", bitSup[idx].x, bitSup[idx].y);
      }
      printf("\n");
    }

    printf("-------------------\n");
    cudaMemcpy(bitSup, d_bitSup, sizeof(uint2) * BITSUPS_INTSIZE,
               cudaMemcpyDeviceToHost);
    for (int j = 0; j < MAX_DOM_SIZE; ++j) {
      printf("c_id = %d, j = %d: ", i, j);
      for (int k = 0; k < BITDOM_INTSIZE; ++k) {
        const int idx = GetBitSupIndexByINTPrstn(c->id, j, k);
        printf("%x, %x", bitSup[idx].x, bitSup[idx].y);
      }
      printf("\n");
    }
    printf("-------------------\n");
  }
#pragma endregion 拷贝bitSup
//
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
  cudaMemcpyToSymbol(kU32Mask1, U32_MASK1, sizeof(U32_MASK1));
  cudaMemcpyToSymbol(kU32Mask0, U32_MASK0, sizeof(U32_MASK0));
}

CModel::~CModel() {
  std::cout << "CModel析构函数" << std::endl;
  cudaFree(d_bitSup);
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
  cudaDestroyTextureObject(texObj);
  // Free device memory
  cudaFreeArray(cuArray);
  // cudaFree(d_output);
}

void CModel::DelGPUModel() const {
  // cudaFree(scope);
}

}  // namespace cpim
