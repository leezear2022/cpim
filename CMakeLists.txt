#cmake_minimum_required(VERSION 3.18)
#project(cpim LANGUAGES CXX CUDA)
#set(CMAKE_CUDA_COMPILER /usr/local/cuda/bin/nvcc)
#set(CMAKE_CUDA_STANDARD 20)
#
## 设置 CUDA 架构
#set(CMAKE_CUDA_ARCHITECTURES 60 70 80)
#
## 添加 CUDA Toolkit 的 include 路径
#include_directories(/usr/local/cuda/include)
#include_directories(${CMAKE_CUDA_TOOLKIT_INCLUDE_DIRECTORIES})
#
## 添加库文件路径
#link_directories(/usr/local/cuda/lib64)
#
#find_package(LibXml2 REQUIRED)
#include_directories(${LIBXML2_INCLUDE_DIR})
#
#find_package(XercesC REQUIRED)
#include_directories(${XercesC_INCLUDE_DIRS})
#
## 添加头文件目录
#include_directories(include)
#
## 添加子目录
#add_subdirectory(xcsp3parser)
#
#add_executable(cpim samples/main.cu
#        #        samples/main.cpp
#        src/HModel.cpp
#        src/XBuilder.cpp
#        include/NaiveBitSet.h
#        include/CPIMBase.h
#        src/Network.cpp
#        src/Solver.cpp
#        include/Timer.h
#        # include/cuSAC2.cuh
#        # src/cuSAC.cu
#        include/cuSAC.cuh
#)
#
#set_target_properties(cpim PROPERTIES
#        CUDA_SEPARABLE_COMPILATION ON)
#
## 链接 xcsp3parser 静态库
#target_link_libraries(cpim PRIVATE xcsp3parser ${XercesC_LIBRARIES} cuda cudart)


cmake_minimum_required(VERSION 3.18)
project(cpim LANGUAGES CXX CUDA)

#set(CMAKE_CUDA_COMPILER /usr/local/cuda/bin/nvcc)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 17)

# 设置 CUDA 架构
set(CMAKE_CUDA_ARCHITECTURES 80 87)

# 添加 CUDA Toolkit 的 include 路径
include_directories(/usr/local/cuda/include)
include_directories(${CMAKE_CUDA_TOOLKIT_INCLUDE_DIRECTORIES})

# 添加库文件路径
link_directories(/usr/local/cuda/lib64)

# ============================================================================
# Abseil 依赖配置 (使用 FetchContent)
# ============================================================================
include(FetchContent)

FetchContent_Declare(
  absl
  GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
  GIT_TAG        20240722.0  # LTS 20240722
  GIT_SHALLOW    TRUE
)

set(ABSL_PROPAGATE_CXX_STD ON)
set(ABSL_ENABLE_INSTALL ON)
set(BUILD_TESTING OFF)  # 不构建 Abseil 的测试

message(STATUS "Fetching Abseil C++...")
FetchContent_MakeAvailable(absl)
message(STATUS "Abseil C++ fetched successfully")

# ============================================================================
# 其他依赖库
# ============================================================================
find_package(LibXml2 REQUIRED)
include_directories(${LIBXML2_INCLUDE_DIR})

# Xerces-C 已弃用，使用 libxml2 替代
# find_package(XercesC REQUIRED)
# include_directories(${XercesC_INCLUDE_DIRS})

# 查找依赖库 (GTest 和 gflags 改为可选)
find_package(GTest)
find_package(gflags)
find_package(glog REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(Unwind REQUIRED libunwind)

# 包含目录
include_directories(${GTEST_INCLUDE_DIRS})
include_directories(${GFLAGS_INCLUDE_DIRS})
include_directories(${GLOG_INCLUDE_DIRS})
include_directories(${Unwind_INCLUDE_DIRS})

# 添加头文件目录
include_directories(include)

# 添加子目录
add_subdirectory(xcsp3parser)

# ============================================================================
# 新模块: cpim_model (XCSP3 解析和中间模型)
# ============================================================================
add_library(cpim_model STATIC
  src/model/types.cpp
  src/model/xcsp_parser.cpp
  src/model/libxml2_parser.cpp
  src/model/model_builder.cpp
  src/model/intermediate_model.cpp
  src/model/cmodel_adapter.cpp
  src/model/model_normalizer.cpp
)

target_include_directories(cpim_model PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${LIBXML2_INCLUDE_DIR}
)

target_link_libraries(cpim_model PUBLIC
  # Abseil 核心库
  absl::flat_hash_map
  absl::flat_hash_set
  absl::inlined_vector
  absl::span
  absl::status
  absl::statusor
  absl::strings
  absl::str_format
  absl::time
  absl::variant
  absl::optional

  # libxml2
  ${LIBXML2_LIBRARIES}

  # 日志
  glog::glog
)

target_compile_options(cpim_model PRIVATE
  -Wall -Wextra
  $<$<CONFIG:Release>:-O3 -march=native>
)



add_library(cpim_solver_cpu STATIC
  src/AC.cpp
  src/AC3.cpp
  src/AC3bit.cpp
  src/AC3rm.cpp
  src/FC.cpp
  src/MAC.cpp
  src/Network.cpp
  src/NSAC.cpp
  src/RNSQ.cpp
  src/RPC3.cpp
  src/SAC1.cpp
  src/SAC3.cpp
  src/Solver.cpp
  src/lMaxRPC.cpp
)

target_include_directories(cpim_solver_cpu PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_BINARY_DIR}/_deps/absl-src
)

target_link_libraries(cpim_solver_cpu PUBLIC
  absl::flat_hash_map
  absl::flat_hash_set
  absl::inlined_vector
  absl::span
  absl::status
  absl::statusor
  absl::strings
  absl::str_format
  absl::time
  absl::variant
  absl::optional
)

target_compile_options(cpim_solver_cpu PRIVATE
  -Wall -Wextra
  $<$<CONFIG:Release>:-O3 -march=native>
)

# ============================================================================
# 新解析器测试程序 (纯 C++ 程序，不含 CUDA 代码)
# ============================================================================
add_executable(cpim_test_parser samples/main_new_parser.cpp)

target_link_libraries(cpim_test_parser PRIVATE
  cpim_model
  cpim_solver_cpu
  glog::glog
  absl::flags
  absl::flags_parse
)

# ============================================================================
# GPU CModel dump utility (parses XML -> IntermediateModel -> CModel)
# ============================================================================
add_executable(cpim_dump
  samples/dump_cmodel.cpp
  src/cuSAC.cu
)

set_target_properties(cpim_dump PROPERTIES
  CUDA_SEPARABLE_COMPILATION ON
)

target_link_libraries(cpim_dump PRIVATE
  cpim_model
  cpim_solver_cpu
  glog::glog
  absl::flags
  absl::flags_parse
  cuda
  cudart
)

# ARM 平台上 NVCC 需要禁用 NEON intrinsics，因为 CUDA 编译器不支持 ARM NEON 扩展
# 使用 -U 来取消内置的宏定义
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64|arm")
  target_compile_options(cpim_dump PRIVATE
    $<$<COMPILE_LANGUAGE:CUDA>:--compiler-options=-U__ARM_NEON>
    $<$<COMPILE_LANGUAGE:CUDA>:--compiler-options=-U__ARM_NEON__>
    $<$<COMPILE_LANGUAGE:CUDA>:--compiler-options=-DABSL_HAVE_NEON=0>
  )
endif()

# ============================================================================
# GModel dump utility (simplified GPU model with unified memory)
# ============================================================================
add_executable(dump_gmodel
  samples/dump_gmodel.cpp
  src/GModel.cu
  src/model/gmodel_adapter.cu
  src/model/gmodel_validator.cu
)

set_target_properties(dump_gmodel PROPERTIES
  CUDA_SEPARABLE_COMPILATION ON
)

target_link_libraries(dump_gmodel PRIVATE
  cpim_model
  glog::glog
  absl::flags
  absl::flags_parse
  cuda
  cudart
)

# ARM 平台 NEON 禁用
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64|arm")
  target_compile_options(dump_gmodel PRIVATE
    $<$<COMPILE_LANGUAGE:CUDA>:--compiler-options=-U__ARM_NEON>
    $<$<COMPILE_LANGUAGE:CUDA>:--compiler-options=-U__ARM_NEON__>
    $<$<COMPILE_LANGUAGE:CUDA>:--compiler-options=-DABSL_HAVE_NEON=0>
  )
endif()

# ============================================================================
# CPU GAC baseline utility (serial event queue + propagation)
# ============================================================================
add_executable(cpim_gac_cpu
  samples/run_gac_cpu.cpp
  src/model/gac_cpu.cpp
)

target_link_libraries(cpim_gac_cpu PRIVATE
  cpim_model
  glog::glog
  absl::flags
  absl::flags_parse
)

# ============================================================================
# 原始主程序 (已弃用，依赖旧的 XBuilder)
# 请使用新的 cpim_test_parser 程序代替
# ============================================================================
# add_executable(cpim samples/main.cu
#         src/HModel.cpp
#         # src/XBuilder.cpp  # 已弃用，使用新的 LibXml2Parser 替代
#         include/NaiveBitSet.h
#         include/CPIMBase.h
#         src/Network.cpp
#         src/Solver.cpp
#         include/Timer.h
#         include/cuSAC.cuh
#         src/cuSAC.cu
#         src/MAC.cpp
#         src/AC.cpp
#         src/AC3.cpp
#         src/AC3bit.cpp
#         src/AC3rm.cpp
#         src/AC3rm.cpp
#         src/FC.cpp
#         src/lMaxRPC.cpp
#         src/NSAC.cpp
#         src/RPC3.cpp
#         src/SAC1.cpp
#         src/SAC3.cpp
# )
#
# set_target_properties(cpim PROPERTIES
#         CUDA_SEPARABLE_COMPILATION ON)
#
# # 链接库
# target_link_libraries(cpim PRIVATE
#         cpim_model          # 新的模型解析模块（已包含 libxml2）
#         xcsp3parser
#         # ${XercesC_LIBRARIES}  # 已弃用
#         ${LIBXML2_LIBRARIES}
#         cuda cudart cudadevrt cudart_static)
#
# # 确保 CUDA 链接器标志被设置
# set_target_properties(cpim PROPERTIES CUDA_SEPARABLE_COMPILATION ON)

# 添加 CUDA 相关的路径
include_directories(${CUDA_INCLUDE_DIRS})
link_directories(${CUDA_TOOLKIT_ROOT_DIR}/lib64)
