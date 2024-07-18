//
// Created by lee on 24-7-13.
//

#ifndef CUSAC_CUH
#define CUSAC_CUH
// CUDA Runtime
#include <cuda_runtime.h>
// #include <device_functions.h>
#include <device_launch_parameters.h>
// Utilities and system includes
#include <thrust/device_vector.h>
#include <thrust/host_vector.h>
#include <thrust/scan.h>

#include <cmath>
#include <iostream>

// #include "cuda_runtime_api.h"
#include "xcsp3model/HModel.h"
namespace cpim {

using u32x2 = uint2;
using u32x3 = uint3;
using u32x4 = uint4;

#ifndef MIN
#define MIN(x, y) ((x < y) ? x : y)
#endif

struct int_predicate {
  __host__ __device__ bool operator()(const int x) { return x > 0; }
};

//__forceinline__ int  GetBitDomIndex(int var_id)
//{
//	return var_id * BITDOM_INTSIZE;
//}

// __inline__ __device__ __host__ int GetTopNum(int num_elements,
//                                              int num_threads) {
//   return (num_elements + (num_threads - 1)) / num_threads;
// }

// inline int intsizeof(const int x) { return (int)ceil((float)x / U32_BIT); }
// #define intsizeof(x) ((nbits + BITS_PER_WORD - 1) / BITS_PER_WORD)
inline int intsizeof(int nbits);
//  {
//   return ((nbits + BITS_PER_WORD - 1) / BITS_PER_WORD);
// }

// #define GetBitSupIndexByTuple(cid, t)(make_int2(
// cid * BITSUP_INTSIZE + t.x * BITDOM_INTSIZE + (t.y >> U32_POS),
// cid * BITSUP_INTSIZE + t.y * BITDOM_INTSIZE + (t.x >> U32_POS)))
extern __constant__ u32 kU32Mask1[32];

extern __constant__ u32 kU32Mask0[32];

const u32 U32_MASK1[32] = {
    0x80000000, 0x40000000, 0x20000000, 0x10000000, 0x08000000, 0x04000000,
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
// 一个bitDom[x]的长度
extern __constant__ int D_BITDOM_INTSIZE;
// 整个bitDom的长度
extern __constant__ int D_BITDOMS_INTSIZE;

const int num_threads = 32;
const int U32_SIZE = sizeof(u32);  ///< 4
const int U32_BIT = U32_SIZE * 8;  ///< 32
const int U32_POS = 5;
const int U32_MOD_MASK = 31;

constexpr int ADDRESS_BITS_PER_WORD = 5;
constexpr int BITS_PER_WORD = 1 << ADDRESS_BITS_PER_WORD;
constexpr int BIT_INDEX_MASK = BITS_PER_WORD - 1;

// 定义纹理内存描述符
// 纹理内存描述符
extern cudaTextureObject_t texObject;
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
extern __managed__ int BITDOM_INTSIZE;
// 整个变量集合的论域的int长度
extern __managed__ int BITDOMS_INTSIZE;
// 一个约束的bitsup的int长度
extern __managed__ int BITSUP_INTSIZE;
// 整个约束集合的bitsup的int长度
extern __managed__ int BITSUPS_INTSIZE;
// 子问题论域总长度
extern __managed__ int BITSUBDOMS_INTSIZE;
// 变量个数
extern __managed__ int VS_SIZE;
// 约束个数
extern __managed__ int CS_SIZE;
// 约束最大元数，目前仅支持二元
extern __managed__ int MAX_ARITY;
// 主问题约束压缩BLOCK数
extern __managed__ int MCC_BLOCK;
//////////////////////////////////////////////////////////////////////////
//	一些GPU变量
//////////////////////////////////////////////////////////////////////////
extern __managed__ int M_Qsize;

//////////////////////////////////////////////////////////////////////////
//  GPU约束记录信息，不可更改
//////////////////////////////////////////////////////////////////////////
//	每个变量的int大小
extern __managed__ int* vars_size;
// 存储约束的scope，类型int3，scope.x: x.id; scope.y: y.id; scope.z: c.id
extern __managed__ int3* scope;
// 最大dom
extern __managed__ int MAX_DOM_SIZE;
// subCon长度
extern __managed__ int SUBCON_SIZE;

//    __managed__ int BITDOM_SIZE;
//    __managed__ int
//  主问题数据结构，使用UM
//  表示约束网络论域
extern __managed__ u32* bitDom;
// 表示约束，不可修改
extern __managed__ uint2* bitSup;
////类似队列，存储约束id
//    __managed__ int *mainCon;
////子问题数据结构
// 表示子问题的约束网络论域。
extern __managed__ u32* bitSubDom;
////类似队列，存储子问题约束id subCon.x: variable，subCon.y: value，subCon.z:
/// c.id
//    __managed__ ushort3* subCon;
//  标记主问题变量域是否删减，初始化全部为1
extern __managed__ int* M_VarPre;
// 标记主问题约束是否需检查，初始化全部为1
extern __managed__ int* M_ConPre;
// 主问题约束传播队列(压缩版)
extern __managed__ uint3* M_ConEvt;
// 主问题约束传播队列
extern __managed__ uint3* M_Con;
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
__global__ void CsCheckMain(int* mConEvt, int* mVarPre, int3* scope,
                            u32* bitDom, uint2* bitSup);

class CModel {
 public:
  const int kNumVars;
  const int kNumTabs;
  const int kMaxDomSize;
  const int kBitDomIntSize;
  // num_threads
  // 以后很有可能会放到纹理内存里
  // uint2* d_bitSup{};
  // 约束和约束的相邻关系
  i32* d_ConNeighbor{};

  cudaArray_t cuArray_MCon{};
  cudaTextureObject_t texObj_MCon{};
  cudaResourceDesc resDesc_MCon{};
  cudaTextureDesc texDesc_MCon{};

  cudaArray_t cuArray3D{};
  cudaTextureObject_t texObj3D{};
  cudaResourceDesc resDesc3D{};
  cudaTextureDesc texDesc3D{};

  thrust::device_vector<uint3> d_MCon;
  thrust::device_vector<uint3> d_MConEvt;
  thrust::device_vector<int> d_ConPre;
  //
  //   __device__ __managed__ ushort4* subVar;
  ////标记子问题发生改动的变量id，初始化全部为0
  //   __device__ __managed__ unsigned short* subEvtVar;
  ////标记子问题发生改动的约束id，初始化全部为1
  //   __device__ __managed__ int* subEvtCon;

  // 根据x和index获得bitDom位置
#define GetBitDomIndex(x, i) (x * BITDOM_INTSIZE + i)
  // 根据落在最后文字的值的个数获取bit表示的偏移量
#define GetOffSet(x) (U32_BIT - (x & U32_MOD_MASK))

#define GetBitSubDomStartIndex(x, a) ((x * MAX_DOM_SIZE + a) * BITDOMS_INTSIZE)
#define GetBitSubDomIndex(x, a, y, i) \
  (GetBitSubDomStartIndex(x, a) + GetBitDomIndex(y, i))

  // __device__ bool IsGtZero(int x) { return x > 0; }
#define IsGtZero(x) (x > 0)
#define GetTopNum(num_elements, num_threads) \
  ((num_elements + (num_threads - 1)) / num_threads)

#define GetBitSupIndexByINTPrstn(cid, x_val, y_val) \
  (cid * BITSUP_INTSIZE + x_val * BITDOM_INTSIZE + y_val)

#define pow2i(e) (1 << e)
  // __device__ __inline__ int pow2i(int e) { return 1 << e; }

// #define GetBitSupIndexByINTPrstn(cid, x_val, y_val) \
//   (cid * BITSUP_INTSIZE + x_val * BITDOM_INTSIZE + y_val)

  __device__ __host__  int GetBitSupIndexById_C_MDS_MDINTS(int cid) ;
  __device__ __host__  int2 GetBitSupIndexByTuple_C_MDS_MDINTS(int cid, int2 t);
  __device__ __host__  int2 GetBitSupIndexByTuple_C_MDINTS_MDS( int cid,  int2 t);
  // __device__ __host__  int2 GetBitSupIndexByINTPrstn_C_MDINTS_MDS( int cid,  int2 t);

  explicit CModel(const HModel& xm);

  void BuildBitModel(const HModel& xm);

  void initialCPUConstant();
  void DelGPUModel() const;

  ~CModel();
};

}  // namespace cpim
#endif  // CUSAC_CUH
