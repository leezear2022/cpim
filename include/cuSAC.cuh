//
// Created by lee on 24-7-13.
//

#ifndef CUSAC_CUH
#define CUSAC_CUH
// CUDA Runtime
#include <cuda_runtime.h>
#include <thrust/copy.h>
#include <thrust/device_vector.h>
#include <thrust/host_vector.h>

// #include "cuda_runtime_api.h"
#include "Solver.h"
#include "xcsp3model/HModel.h"
namespace cpim {

using u32x2 = uint2;
using u32x3 = uint3;
using u32x4 = uint4;

using i32x2 = int2;
using i32x3 = int3;
using i32x4 = int3;

#ifndef MIN
#define MIN(x, y) ((x < y) ? x : y)
#endif

struct int_predicate {
  __host__ __device__ bool operator()(const int x) { return x > 0; }
};

//__forceinline__ int  GetBitDomByIndex(int var_id)
//{
//	return var_id * BITDOM_INTSIZE;
//}

// __inline__ __device__ __host__ int GetTopNum(int num_elements,
//                                              int num_threads) {
//   return (num_elements + (num_threads - 1)) / num_threads;
// }

// inline int intsizeof(const int x) { return (int)ceil((float)x / U32_BIT); }
// #define intsizeof(x) ((nbits + kBitsPerWord - 1) / kBitsPerWord)
inline int intsizeof(int nbits);
//  {
//   return ((nbits + kBitsPerWord - 1) / kBitsPerWord);
// }

// #define GetBitSupIndexByTuple(cid, t)(make_int2(
// cid * BITSUP_INTSIZE + t.x * BITDOM_INTSIZE + (t.y >> U32_POS),
// cid * BITSUP_INTSIZE + t.y * BITDOM_INTSIZE + (t.x >> U32_POS)))
// extern __constant__ u32 kDeviceU32Mask1[32];
//
// extern __constant__ u32 kDeviceU32Mask0[32];

// const u32 U32_MASK1[32] = {
//     0x80000000, 0x40000000, 0x20000000, 0x10000000, 0x08000000, 0x04000000,
//     0x02000000, 0x01000000, 0x00800000, 0x00400000, 0x00200000, 0x00100000,
//     0x00080000, 0x00040000, 0x00020000, 0x00010000, 0x00008000, 0x00004000,
//     0x00002000, 0x00001000, 0x00000800, 0x00000400, 0x00000200, 0x00000100,
//     0x00000080, 0x00000040, 0x00000020, 0x00000010, 0x00000008, 0x00000004,
//     0x00000002, 0x00000001,
// };
// const u32 U32_MASK0[32] = {
//     0x7FFFFFFF, 0xBFFFFFFF, 0xDFFFFFFF, 0xEFFFFFFF, 0xF7FFFFFF, 0xFBFFFFFF,
//     0xFDFFFFFF, 0xFEFFFFFF, 0xFF7FFFFF, 0xFFBFFFFF, 0xFFDFFFFF, 0xFFEFFFFF,
//     0xFFF7FFFF, 0xFFFBFFFF, 0xFFFDFFFF, 0xFFFEFFFF, 0xFFFF7FFF, 0xFFFFBFFF,
//     0xFFFFDFFF, 0xFFFFEFFF, 0xFFFFF7FF, 0xFFFFFBFF, 0xFFFFFDFF, 0xFFFFFEFF,
//     0xFFFFFF7F, 0xFFFFFFBF, 0xFFFFFFDF, 0xFFFFFFEF, 0xFFFFFFF7, 0xFFFFFFFB,
//     0xFFFFFFFD, 0xFFFFFFFE,
// };
/*
* 怎么把这两个CPU常量赋值给GPU常量：extern __constant__ u32
kU32Mask1[32];和extern __constant__ u32 kU32Mask0[32];？ const u32 U32_MASK1[32]
= { 0x80000000, 0x40000000, 0x20000000, 0x10000000, 0x08000000, 0x04000000,
    0x02000000, 0x01000000, 0x00800000, 0x00400000, 0x00200000, 0x00100000,
    0x00080000, 0x00040000, 0x00020000, 0x00010000, 0x00008000, 0x00004000,
    0x00002000, 0x00001000, 0x00000800, 0x00000400, 0x00000200, 0x00000100,
    0x00000080, 0x00000040, 0x00000020, 0x00000010, 0x00000008, 0x00000004,
    0x00000002, 0x00000001,
};
const u32 U32_MASK0[32] = {
    0x7FFFFFFF, 0xBFFFFFFF, 0xDFFFFFFF, 0xEFFFFFFF, 0xF7FFFFFF, 0xFBFFFFFF,
    0xFDFFFFFF, 0xFEFFFFFF, 0xFF7FFFFF, 0xFFBFFFFF, 0xFFDFFFFF, 0xFFEFFFFF,
    0xFFF7FFFF, 0xFFFBFFFF, 0xFFFDFFFF, 0xFFFEFFFF, 0xFFFF7FFF, 0xFFFFBFFF,
    0xFFFFDFFF, 0xFFFFEFFF, 0xFFFFF7FF, 0xFFFFFBFF, 0xFFFFFDFF, 0xFFFFFEFF,
    0xFFFFFF7F, 0xFFFFFFBF, 0xFFFFFFDF, 0xFFFFFFEF, 0xFFFFFFF7, 0xFFFFFFFB,
    0xFFFFFFFD, 0xFFFFFFFE,
};

 */
// 定义一些全局常量
const int kMaxNumVars = 512;

// 一个bitDom[x]的长度
extern __constant__ int kDeviceBitDomIntSize;
// 整个bitDom的长度
extern __constant__ int kDeviceBitDomsIntSize;
extern __constant__ int kDeviceMaxDomSize;
extern __constant__ int kDeviceNumVars;
extern __constant__ int kDeviceNumTabs;
extern __constant__ int* kDeviceDomSize;

extern __constant__ int kDeviceBitSupIntSize;
extern __constant__ int kDeviceBitSupsIntSize;
extern __constant__ int kDeviceBitSubDomsIntSize;
extern __constant__ inline int kDeg[kMaxNumVars];

// constexpr int num_threads = 32;
constexpr int U32_SIZE = sizeof(u32);  ///< 4
constexpr int U32_BIT = U32_SIZE * 8;  ///< 32
constexpr int U32_POS = 5;
constexpr int U32_MOD_MASK = 31;

constexpr int kAddressBitsPerWord = 5;
constexpr int kBitsPerWord = 1 << kAddressBitsPerWord;
constexpr int kBitIndexMask = kBitsPerWord - 1;

#define WORD_INDEX(bitIndex) ((bitIndex) >> kAddressBitsPerWord)
#define WORD_OFFSET(bitIndex) ((bitIndex) & kBitIndexMask)

#define BITSET_GET(words, bitIndex) \
  ((words[WORD_INDEX(bitIndex)] >> WORD_OFFSET(bitIndex)) & 1U)
#define BITSET_SET(words, bitIndex) \
  (words[WORD_INDEX(bitIndex)] |= (1U << WORD_OFFSET(bitIndex)))
#define BITSET_CLEAR(words, bitIndex) \
  (words[WORD_INDEX(bitIndex)] &= ~(1U << WORD_OFFSET(bitIndex)))

// 定义纹理内存描述符
// 纹理内存描述符
// extern cudaTextureObject_t texObject;
// extern cudaTextureObject_t texObject;
// // 一个bitDom[x]的长度
// __constant__ int D_BITDOM_INTSIZE;
// // 整个bitDom的长度
//
// __constant__ int D_BITDOMS_INTSIZE;

// bit支持， uint2 bitSup[c][a][idx].x = bitSup[c,x,a,idx]
// bit支持， uint2 bitSup[c][a][idx].y = bitSup[c,y,a,idx]
//__device__ uint2*** d_bitSup;
//__host__ uint2*** h_bitSup;
//__device__ u32** d_bitDom;
//__host__ u32** h_bitDom;
//__device__ u32**
// __constant__ int D_NUM_BD_BLOCK;
// __constant__ int D_NUM_CS_SIZE_BLOCKS;
//////////////////////////////////////////////////////////////////////////
//	一些GPU常量
//////////////////////////////////////////////////////////////////////////
// __managed__ int NUM_BD_BLOCK;
// __device__ __managed__ int NUM_CS_SIZE_BLOCKS;
// 一个变量论域的int长度
// extern __managed__ int BITDOM_INTSIZE;
// // 整个变量集合的论域的int长度
// extern __managed__ int BITDOMS_INTSIZE;
// // 一个约束的bitsup的int长度
// extern __managed__ int BITSUP_INTSIZE;
// // 整个约束集合的bitsup的int长度
// extern __managed__ int BITSUPS_INTSIZE;
// // 子问题论域总长度
// extern __managed__ int BITSUBDOMS_INTSIZE;
// // 变量个数
// extern __managed__ int VS_SIZE;
// // 约束个数
// extern __managed__ int CS_SIZE;
// // 约束最大元数，目前仅支持二元
// extern __managed__ int MAX_ARITY;
// // 主问题约束压缩BLOCK数
// extern __managed__ int MCC_BLOCK;
//////////////////////////////////////////////////////////////////////////
//	一些GPU变量
//////////////////////////////////////////////////////////////////////////
extern __managed__ int M_Qsize;

//////////////////////////////////////////////////////////////////////////
//  GPU约束记录信息，不可更改
//////////////////////////////////////////////////////////////////////////
// //	每个变量的int大小
// extern __managed__ int* vars_size;
// // 存储约束的scope，类型int3，scope.x: x.id; scope.y: y.id; scope.z: c.id
// extern __managed__ int3* scope;
// // 最大dom
// extern __managed__ int MAX_DOM_SIZE;
// // subCon长度
// extern __managed__ int SUBCON_SIZE;
extern __managed__ int GAC_success;

//    __managed__ int BITDOM_SIZE;
//    __managed__ int
//  主问题数据结构，使用UM
// //  表示约束网络论域
extern __managed__ u32* bitDom;
extern __managed__ u32* bitSubDom;
// // 表示约束，不可修改
// extern __managed__ uint2* bitSup;
// ////类似队列，存储约束id
// //    __managed__ int *mainCon;
// ////子问题数据结构
// // 表示子问题的约束网络论域。
// extern __managed__ u32* bitSubDom;
// ////类似队列，存储子问题约束id subCon.x: variable，subCon.y: value，subCon.z:
// /// c.id
// //    __managed__ ushort3* subCon;
// //  标记主问题变量域是否删减，初始化全部为1
// extern __managed__ int* M_VarPre;
// // 标记主问题约束是否需检查，初始化全部为1
// extern __managed__ int* M_ConPre;
// // 主问题约束传播队列(压缩版)
// extern __managed__ uint3* M_ConEvt;
// // 主问题约束传播队列
// extern __managed__ uint3* M_Con;
//  thrust::device_vector<uint3> M_Con;
//  thrust::device_vector<uint3> M_ConEvt;
//  thrust::device_vector<int> M_ConPre;
// 标记子问题变量域是否删减，初始化全部为1
extern __managed__ int* S_VarPre;
// 记录子问题变量域
extern __managed__ uint3* S_Var;
// 标记子问题约束是否需检查，初始化全部为1
extern __managed__ int* S_ConPre;
// 子问题约束传播队列(压缩版)
extern __managed__ int3* S_ConEvt;
// 子问题约束传播队列
extern __managed__ int3* S_Con;

__global__ void exampleKernel(uint3* MCon, int size);

__global__ void exampleKernelUMask();

__global__ void CsCheckMain(i32* mConPre, const u32x3* mCon, u32* bitDom,
                            const i32* dom_size, cudaTextureObject_t bitSup,
                            int num_ConEvt);

class CModel {
 public:
  const int kNumVars;
  const int kNumTabs;
  const int kDepth;
  const int kMaxDomSize;
  const int kBitDomIntSize;
  const int kBitDomsIntSize;
  const int kAllBitDomsIntSize;
  const int kBitSupIntSize;
  const int kBitSupsIntSize;
  const int kBitSubDomsIntSize;
  const int kSharedMemSize;

  // num_threads
  // 以后很有可能会放到纹理内存里
  // uint2* d_bitSup{};
  // 约束和约束的相邻关系
  i32* d_ConNeighbor{};
  // 长度具有搜索树深度，dom等于1的变量默认它已赋值
  i32* h_current_domain_size;
  i32* d_current_domain_size;
  u32* h_bitDom;
  u32* d_bitDom;

  // 变量在第几级被赋值了
  i32* d_assigned_at_level;

  thrust::device_vector<u32x3> d_subscription;
  thrust::host_vector<u32x3> h_subscription;
  // 长度kNumVars+1
  thrust::device_vector<int> d_subscription_offset;
  thrust::host_vector<int> h_subscription_offset;

  // 记录解：索引是变量，值是解
  i32* d_solution;
  i32x2* assigned;
  thrust::host_vector<int> h_Deg;
  thrust::device_vector<int> d_Deg;

  cudaArray_t cuArray_MCon{};
  cudaTextureObject_t texObj_MCon{};
  cudaResourceDesc resDesc_MCon{};
  cudaTextureDesc texDesc_MCon{};

  cudaArray_t cuArray3D{};
  cudaTextureObject_t texObj_BitSup{};
  cudaResourceDesc resDesc3D{};
  cudaTextureDesc texDesc3D{};

  thrust::device_vector<uint3> d_MCon;
  thrust::device_vector<uint3> d_MConEvt;
  thrust::device_vector<int> d_ConPre;
  thrust::host_vector<int> h_dom_size;
  thrust::host_vector<int> h_cur_dom_size;
  thrust::device_vector<int> d_cur_dom_size;
  thrust::device_vector<float> d_ratio;
  SearchStatistics statistics_;

  // thrust::host_vector<int> d_dom_size;
  // thrust::host_vector<int> dom_size;

  // i32* h_current_domain_size;
  // i32* d_current_domain_size;
  //
  //   __device__ __managed__ ushort4* subVar;
  ////标记子问题发生改动的变量id，初始化全部为0
  //   __device__ __managed__ unsigned short* subEvtVar;
  ////标记子问题发生改动的约束id，初始化全部为1
  //   __device__ __managed__ int* subEvtCon;

  // 根据落在最后文字的值的个数获取bit表示的偏移量
#define GetOffSet(x) (U32_BIT - (x & U32_MOD_MASK))
#define IsGtZero(x) (x > 0)
#define GetTopNum(num_elements, num_threads) \
  ((num_elements + (num_threads - 1)) / num_threads)
#define pow2i(e) (1 << e)

  // 获取常量值的模板函数
  template <typename T>
  __host__ __device__ static inline T GetConstantValue(const T& hostValue,
                                                       const T& deviceValue) {
#ifdef __CUDA_ARCH__
    return deviceValue;
#else
    return hostValue;
#endif
  }

  // 通过值(x,ith)拿到(x,ith)所在的word
  __host__ __device__ int GetBitDomByIndex(const int x, const int i) const {
    return x * GetConstantValue(kBitDomIntSize, kDeviceBitDomIntSize) + i;
  }

  // 通过值(x,ith)拿到(x,ith)所在的word
  __host__ __device__ int GetBitDomByIndexAndLevel(const int x, const int i,
                                                   const int level) const {
    return level * GetConstantValue(kBitDomsIntSize, kDeviceBitDomsIntSize) +
           x * GetConstantValue(kBitDomIntSize, kDeviceBitDomIntSize) + i;
  }

  // 通过值(x,a)拿到(x,a)所在的word
  __host__ __device__ int GetBitDomByValue(const int x, const int a) const {
    return x * GetConstantValue(kBitDomIntSize, kDeviceBitDomIntSize) + a &
           U32_MOD_MASK;
  }

  __host__ __device__ int GetBitSubDomStartIndex(int x, int a) {
    return (x * GetConstantValue(kMaxDomSize, kDeviceMaxDomSize) + a) *
           GetConstantValue(kBitDomsIntSize, kDeviceBitDomsIntSize);
  }

  __host__ __device__ int GetBitSubDomIndex(int x, int a, int y, int i) {
    return GetBitSubDomStartIndex(x, a) + GetBitDomByIndex(y, i);
  }

  __host__ __device__ int GetBitSupIndexByINTPrstn(int cid, int x_val,
                                                   int y_val) {
    return cid * GetConstantValue(kBitSupIntSize, kDeviceBitSupIntSize) +
           x_val * GetConstantValue(kBitDomIntSize, kDeviceBitDomIntSize) +
           y_val;
  }

  __host__ __device__ inline int GetBitSupIndexByCID(const int cid) {
    int bitSupIntSize = GetConstantValue(kBitSupIntSize, kDeviceBitSupIntSize);
    return cid * bitSupIntSize;
  }

  // // c, (x, a), (y, a)
  // // t.x = x, a
  // // t.y = y, a
  // // 若维度是[e][d][d/w],因此索引的计算公式如下:
  __host__ __device__ inline int2 GetBitSupIndexByTuple_C_MDS_MDINTS(
      const int cid, const int2 t) {
    int bitSupIntSize = GetConstantValue(kBitSupIntSize, kDeviceBitSupIntSize);
    int bitDomIntSize = GetConstantValue(kBitDomIntSize, kDeviceBitDomIntSize);
    return make_int2(
        cid * bitSupIntSize + t.x * bitDomIntSize + (t.y >> U32_POS),
        cid * bitSupIntSize + t.y * bitDomIntSize + (t.x >> U32_POS));
  }

  // // c, (x, a), (y, a)
  // // t.x = x, a
  // // t.y = y, a
  // // 若维度是[e][d/w][d],因此索引的计算公式如下:
  __host__ __device__ inline int2 GetBitSupIndexByTuple_C_MDINTS_MDS(
      const int cid, const int2 t) {
    int bitSupIntSize = GetConstantValue(kBitSupIntSize, kDeviceBitSupIntSize);
    int maxDomSize = GetConstantValue(kMaxDomSize, kDeviceMaxDomSize);
    return make_int2(cid * bitSupIntSize + (t.y >> U32_POS) * maxDomSize + t.x,
                     cid * bitSupIntSize + (t.x >> U32_POS) * maxDomSize + t.y);
  }

  explicit CModel(const HModel& xm);

  void bitDomCopy();

  int compress_Main();

  void BuildBitModel(const HModel& xm);

  int CreateNewLevel();
  int BackLevel() { return --current_level_; }

  void initialGPUConstant();
  bool enforceGAC();
  bool enforceGAC(int var, int type);
  void enforceSAC();
  SearchStatistics solve(float time_limits);
  int heuristic();
  void DelGPUModel() const;

  ~CModel();

 private:
  int current_level_ = 0;
};

}  // namespace cpim
#endif  // CUSAC_CUH
