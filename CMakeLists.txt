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
project(cpim LANGUAGES CXX)

#set(CMAKE_CUDA_COMPILER /usr/local/cuda/bin/nvcc)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_TESTING_ENABLED)
  enable_testing()
endif()

set(CPIM_ENABLE_CUDA "AUTO" CACHE STRING "Enable CUDA targets: AUTO, ON, or OFF")
set_property(CACHE CPIM_ENABLE_CUDA PROPERTY STRINGS AUTO ON OFF)
set(CPIM_ENABLE_METAL "AUTO" CACHE STRING "Enable Metal targets: AUTO, ON, or OFF")
set_property(CACHE CPIM_ENABLE_METAL PROPERTY STRINGS AUTO ON OFF)

include(CheckLanguage)

set(CPIM_CUDA_ENABLED OFF)
if(CPIM_ENABLE_CUDA STREQUAL "ON")
  enable_language(CUDA)
  set(CPIM_CUDA_ENABLED ON)
elseif(CPIM_ENABLE_CUDA STREQUAL "AUTO")
  check_language(CUDA)
  if(CMAKE_CUDA_COMPILER)
    enable_language(CUDA)
    set(CPIM_CUDA_ENABLED ON)
  endif()
elseif(NOT CPIM_ENABLE_CUDA STREQUAL "OFF")
  message(FATAL_ERROR "CPIM_ENABLE_CUDA must be AUTO, ON, or OFF")
endif()

if(CPIM_CUDA_ENABLED)
  set(CMAKE_CUDA_STANDARD 17)
  # 设置 CUDA 架构
  set(CMAKE_CUDA_ARCHITECTURES 80 87)

  # 添加 CUDA Toolkit 的 include 路径
  include_directories(/usr/local/cuda/include)
  include_directories(${CMAKE_CUDA_TOOLKIT_INCLUDE_DIRECTORIES})

  # 添加库文件路径
  link_directories(/usr/local/cuda/lib64)
  message(STATUS "CPIM CUDA targets enabled")
else()
  message(STATUS "CPIM CUDA targets disabled")
endif()

set(CPIM_METAL_ENABLED OFF)
if(CPIM_ENABLE_METAL STREQUAL "ON" OR CPIM_ENABLE_METAL STREQUAL "AUTO")
  if(APPLE)
    execute_process(
      COMMAND xcrun --find metal
      RESULT_VARIABLE CPIM_METAL_FIND_RESULT
      OUTPUT_VARIABLE CPIM_METAL_COMPILER
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
    )
    execute_process(
      COMMAND xcrun --find metallib
      RESULT_VARIABLE CPIM_METALLIB_FIND_RESULT
      OUTPUT_VARIABLE CPIM_METALLIB_COMPILER
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
    )
    if(CPIM_METAL_FIND_RESULT EQUAL 0 AND CPIM_METALLIB_FIND_RESULT EQUAL 0)
      enable_language(OBJCXX)
      set(CPIM_METAL_ENABLED ON)
      message(STATUS "CPIM Metal targets enabled")
    elseif(CPIM_ENABLE_METAL STREQUAL "ON")
      message(FATAL_ERROR "CPIM_ENABLE_METAL=ON but metal/metallib tools were not found")
    endif()
  elseif(CPIM_ENABLE_METAL STREQUAL "ON")
    message(FATAL_ERROR "CPIM_ENABLE_METAL=ON requires an Apple platform")
  endif()
elseif(NOT CPIM_ENABLE_METAL STREQUAL "OFF")
  message(FATAL_ERROR "CPIM_ENABLE_METAL must be AUTO, ON, or OFF")
endif()

if(NOT CPIM_METAL_ENABLED)
  message(STATUS "CPIM Metal targets disabled")
endif()

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
find_package(GTest QUIET)
find_package(gflags QUIET)
find_package(glog QUIET)
if(NOT glog_FOUND)
  set(WITH_GFLAGS OFF CACHE BOOL "Build glog without gflags" FORCE)
  set(WITH_GTEST OFF CACHE BOOL "Build glog without tests" FORCE)
  FetchContent_Declare(
    glog
    GIT_REPOSITORY https://github.com/google/glog.git
    GIT_TAG        v0.7.1
    GIT_SHALLOW    TRUE
  )
  message(STATUS "Fetching glog...")
  FetchContent_MakeAvailable(glog)
endif()
find_package(PkgConfig REQUIRED)
pkg_check_modules(Unwind libunwind)

# 包含目录
include_directories(${GTEST_INCLUDE_DIRS})
include_directories(${GFLAGS_INCLUDE_DIRS})
include_directories(${GLOG_INCLUDE_DIRS})
if(Unwind_FOUND)
  include_directories(${Unwind_INCLUDE_DIRS})
endif()

# 添加头文件目录
include_directories(include)

# 添加子目录
add_subdirectory(third_party/xcsp3parser)

# ============================================================================
# 新模块: cpim_model (XCSP3 解析和中间模型)
# ============================================================================
add_library(cpim_model STATIC
  src/model/types.cpp
  src/model/xcsp_parser.cpp
  src/model/libxml2_parser.cpp
  src/model/model_builder.cpp
  src/model/intermediate_model.cpp
  src/model/device_layout.cpp
  src/model/model_normalizer.cpp
)

if(CPIM_CUDA_ENABLED)
  target_sources(cpim_model PRIVATE src/model/cmodel_adapter.cpp)
endif()

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
  # 基础组件
  src/base/unified_trail.cpp

  # CPU 算法
  src/solver/cpu/AC.cpp
  src/solver/cpu/AC3.cpp
  src/solver/cpu/AC3bit.cpp
  src/solver/cpu/AC3rm.cpp
  src/solver/cpu/FC.cpp
  src/solver/cpu/MAC.cpp
  src/solver/cpu/MSAC3bit.cpp
  src/solver/cpu/NSAC.cpp
  src/solver/cpu/RNSQ.cpp
  src/solver/cpu/RPC3.cpp
  src/solver/cpu/SAC1.cpp
  src/solver/cpu/SAC3.cpp
  src/solver/cpu/lMaxRPC.cpp

  # 共享代码
  src/solver/common/Network.cpp
  src/solver/common/Solver.cpp

  # Phase 1.5: 变量选择启发式
  src/solver/common/variable_selector.cpp
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
  glog::glog
)

target_compile_options(cpim_solver_cpu PRIVATE
  -Wall -Wextra
  $<$<CONFIG:Release>:-O3 -march=native>
)

# ============================================================================
# 新解析器测试程序 (纯 C++ 程序，不含 CUDA 代码)
# ============================================================================
add_executable(cpim_test_parser apps/cpim_test_parser.cpp)

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
if(CPIM_CUDA_ENABLED)
add_executable(cpim_dump
  apps/cpim_dump.cpp
  src/solver/gpu/cuSAC.cu
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
  apps/dump_gmodel.cpp
  src/solver/gpu/GModel.cu
  src/solver/gpu/batch_probe_manager.cu
  src/model/gmodel_adapter.cu
  src/model/gmodel_validator.cu
)

set_target_properties(dump_gmodel PROPERTIES
  CUDA_SEPARABLE_COMPILATION ON
)

target_link_libraries(dump_gmodel PRIVATE
  cpim_model
  cpim_solver_cpu  # Phase 1.2: 需要 UnifiedTrail + IntVar (用于条件编译的符号)
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
# test_multilevel: 测试 GModel 多层级功能
# ============================================================================
add_executable(test_multilevel
  tests/cpp/test_multilevel.cpp
  src/solver/gpu/GModel.cu
  src/solver/gpu/batch_probe_manager.cu
  src/model/gmodel_adapter.cu
  src/model/gmodel_validator.cu
)

set_target_properties(test_multilevel PROPERTIES
  CUDA_SEPARABLE_COMPILATION ON
)

target_link_libraries(test_multilevel PRIVATE
  cpim_model
  cpim_solver_cpu  # Phase 1.2: 需要 UnifiedTrail + IntVar
  glog::glog
  absl::flags
  absl::flags_parse
  cuda
  cudart
)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64|arm")
  target_compile_options(test_multilevel PRIVATE
    $<$<COMPILE_LANGUAGE:CUDA>:--compiler-options=-U__ARM_NEON>
    $<$<COMPILE_LANGUAGE:CUDA>:--compiler-options=-U__ARM_NEON__>
    $<$<COMPILE_LANGUAGE:CUDA>:--compiler-options=-DABSL_HAVE_NEON=0>
  )
endif()

# ============================================================================
# test_batch_probe_state: 测试 BatchProbeManager 状态一致性
# ============================================================================
add_executable(test_batch_probe_state
  tests/cpp/test_batch_probe_state.cpp
  src/solver/gpu/GModel.cu
  src/solver/gpu/batch_probe_manager.cu
  src/model/gmodel_adapter.cu
  src/model/gmodel_validator.cu
)

set_target_properties(test_batch_probe_state PROPERTIES
  CUDA_SEPARABLE_COMPILATION ON
)

target_link_libraries(test_batch_probe_state PRIVATE
  cpim_model
  cpim_solver_cpu
  glog::glog
  absl::flags
  absl::flags_parse
  cuda
  cudart
)

# 定义项目根路径宏（支持从任意目录运行测试）
target_compile_definitions(test_batch_probe_state PRIVATE
  CPIM_PROJECT_ROOT="${CMAKE_SOURCE_DIR}"
)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64|arm")
  target_compile_options(test_batch_probe_state PRIVATE
    $<$<COMPILE_LANGUAGE:CUDA>:--compiler-options=-U__ARM_NEON>
    $<$<COMPILE_LANGUAGE:CUDA>:--compiler-options=-U__ARM_NEON__>
    $<$<COMPILE_LANGUAGE:CUDA>:--compiler-options=-DABSL_HAVE_NEON=0>
  )
endif()

# 启用测试（如果尚未启用）
if(NOT CMAKE_TESTING_ENABLED)
  enable_testing()
endif()

# 注册到 ctest
add_test(
  NAME test_batch_probe_state
  COMMAND test_batch_probe_state --input=${CMAKE_SOURCE_DIR}/tests/data/bench/queens-4_ext.xml
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

set_tests_properties(test_batch_probe_state PROPERTIES
  LABELS "gpu;batch_ac"
  TIMEOUT 120
)

# ============================================================================
# test_batch2_probe: 比较 Batch-2 (Micro-Batch) 与 Batch-1 的结果一致性
# ============================================================================
add_executable(test_batch2_probe
  tests/cpp/test_batch2_probe.cpp
  src/solver/gpu/GModel.cu
  src/solver/gpu/batch_probe_manager.cu
  src/model/gmodel_adapter.cu
  src/model/gmodel_validator.cu
)

set_target_properties(test_batch2_probe PROPERTIES
  CUDA_SEPARABLE_COMPILATION ON
)

target_link_libraries(test_batch2_probe PRIVATE
  cpim_model
  cpim_solver_cpu
  glog::glog
  absl::flags
  absl::flags_parse
  cuda
  cudart
)

target_compile_definitions(test_batch2_probe PRIVATE
  CPIM_PROJECT_ROOT="${CMAKE_SOURCE_DIR}"
)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64|arm")
  target_compile_options(test_batch2_probe PRIVATE
    $<$<COMPILE_LANGUAGE:CUDA>:--compiler-options=-U__ARM_NEON>
    $<$<COMPILE_LANGUAGE:CUDA>:--compiler-options=-U__ARM_NEON__>
    $<$<COMPILE_LANGUAGE:CUDA>:--compiler-options=-DABSL_HAVE_NEON=0>
  )
endif()

add_test(
  NAME test_batch2_probe
  COMMAND test_batch2_probe --input=${CMAKE_SOURCE_DIR}/tests/data/bench/queens-4_ext.xml
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

set_tests_properties(test_batch2_probe PROPERTIES
  LABELS "gpu;batch_ac"
  TIMEOUT 120
)

# ============================================================================
# gmodel_solver: 简单的 GModel MAC 求解器
# ============================================================================
add_executable(gmodel_solver
  apps/gmodel_solver.cpp
  src/solver/gpu/GModel.cu
  src/solver/gpu/batch_probe_manager.cu
  src/model/gmodel_adapter.cu
  src/model/gmodel_validator.cu
)

set_target_properties(gmodel_solver PROPERTIES
  CUDA_SEPARABLE_COMPILATION ON
)

target_link_libraries(gmodel_solver PRIVATE
  cpim_model
  cpim_solver_cpu  # Phase 1.2: 需要 UnifiedTrail + IntVar
  glog::glog
  absl::flags
  absl::flags_parse
  cuda
  cudart
)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64|arm")
  target_compile_options(gmodel_solver PRIVATE
    $<$<COMPILE_LANGUAGE:CUDA>:--compiler-options=-U__ARM_NEON>
    $<$<COMPILE_LANGUAGE:CUDA>:--compiler-options=-U__ARM_NEON__>
    $<$<COMPILE_LANGUAGE:CUDA>:--compiler-options=-DABSL_HAVE_NEON=0>
  )
endif()

# ============================================================================
# Batch-1 vs Batch-2 Throughput Benchmark
# ============================================================================
add_executable(benchmark_probe_throughput
  apps/benchmark_probe_throughput.cpp
  src/solver/gpu/GModel.cu
  src/solver/gpu/batch_probe_manager.cu
  src/model/gmodel_adapter.cu
  src/model/gmodel_validator.cu
)

set_target_properties(benchmark_probe_throughput PROPERTIES
  CUDA_SEPARABLE_COMPILATION ON
)

target_link_libraries(benchmark_probe_throughput PRIVATE
  cpim_model
  cpim_solver_cpu
  glog::glog
  absl::flags
  absl::flags_parse
  cuda
  cudart
)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64|arm")
  target_compile_options(benchmark_probe_throughput PRIVATE
    $<$<COMPILE_LANGUAGE:CUDA>:--compiler-options=-U__ARM_NEON>
    $<$<COMPILE_LANGUAGE:CUDA>:--compiler-options=-U__ARM_NEON__>
    $<$<COMPILE_LANGUAGE:CUDA>:--compiler-options=-DABSL_HAVE_NEON=0>
  )
endif()

# ============================================================================
# Stage 1 vs Stage 2 (Persistent Blocks) correctness test
# ============================================================================
add_executable(test_stage2_persistent
  tests/cpp/test_stage2_persistent.cpp
  src/solver/gpu/GModel.cu
  src/solver/gpu/batch_probe_manager.cu
  src/model/gmodel_adapter.cu
  src/model/gmodel_validator.cu
  src/base/unified_trail.cpp
  src/solver/common/Network.cpp
)

target_link_libraries(test_stage2_persistent PRIVATE
  cpim_model
  glog::glog
  absl::flags
  absl::flags_parse
)

target_include_directories(test_stage2_persistent PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${CMAKE_CURRENT_SOURCE_DIR}/include/model
)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64|arm")
  target_compile_options(test_stage2_persistent PRIVATE
    $<$<COMPILE_LANGUAGE:CUDA>:--compiler-options=-U__ARM_NEON>
    $<$<COMPILE_LANGUAGE:CUDA>:--compiler-options=-U__ARM_NEON__>
    $<$<COMPILE_LANGUAGE:CUDA>:--compiler-options=-DABSL_HAVE_NEON=0>
  )
endif()

add_test(
  NAME test_stage2_persistent
  COMMAND test_stage2_persistent ${CMAKE_SOURCE_DIR}/tests/data/bench/queens-4_ext.xml
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

set_tests_properties(test_stage2_persistent PROPERTIES
  LABELS "gpu;batch_ac;stage2"
  TIMEOUT 120
)
endif()

# ============================================================================
# CPU GAC baseline utility (serial event queue + propagation)
# ============================================================================
add_executable(cpim_gac_cpu
  apps/cpim_gac_cpu.cpp
  src/model/gac_cpu.cpp
)

target_link_libraries(cpim_gac_cpu PRIVATE
  cpim_model
  glog::glog
  absl::flags
  absl::flags_parse
)

# ============================================================================
# compare_cpu_metal: CPU vs Metal GAC correctness MVP
# ============================================================================
if(CPIM_METAL_ENABLED)
  set(CPIM_METAL_AIR ${CMAKE_CURRENT_BINARY_DIR}/gac_kernels.air)
  set(CPIM_METAL_LIB ${CMAKE_CURRENT_BINARY_DIR}/metal_gac.metallib)
  set(CPIM_METAL_KERNELS ${CMAKE_CURRENT_SOURCE_DIR}/src/solver/metal/gac_kernels.metal)
  set(CPIM_METAL_RUNTIME_SOURCES
    src/solver/metal/metal_runtime.mm
    src/solver/metal/metal_gac_solver.mm
  )

  add_custom_command(
    OUTPUT ${CPIM_METAL_AIR}
    COMMAND ${CPIM_METAL_COMPILER} -c ${CPIM_METAL_KERNELS} -o ${CPIM_METAL_AIR}
    DEPENDS ${CPIM_METAL_KERNELS}
    COMMENT "Compiling Metal GAC kernels"
    VERBATIM
  )

  add_custom_command(
    OUTPUT ${CPIM_METAL_LIB}
    COMMAND ${CPIM_METALLIB_COMPILER} ${CPIM_METAL_AIR} -o ${CPIM_METAL_LIB}
    DEPENDS ${CPIM_METAL_AIR}
    COMMENT "Linking Metal GAC library"
    VERBATIM
  )

  add_custom_target(cpim_metal_kernels DEPENDS ${CPIM_METAL_LIB})

  add_executable(compare_cpu_metal
    apps/compare_cpu_metal.cpp
    src/model/gac_cpu.cpp
    ${CPIM_METAL_RUNTIME_SOURCES}
  )

  add_dependencies(compare_cpu_metal cpim_metal_kernels)

  target_link_libraries(compare_cpu_metal PRIVATE
    cpim_model
    glog::glog
    absl::flags
    absl::flags_parse
    "-framework Foundation"
    "-framework Metal"
  )

  target_compile_options(compare_cpu_metal PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
  )

  add_custom_command(
    TARGET compare_cpu_metal
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CPIM_METAL_LIB}
            $<TARGET_FILE_DIR:compare_cpu_metal>/metal_gac.metallib
    VERBATIM
  )

  add_executable(benchmark_metal_gac
    apps/benchmark_metal_gac.cpp
    src/model/gac_cpu.cpp
    ${CPIM_METAL_RUNTIME_SOURCES}
  )

  add_dependencies(benchmark_metal_gac cpim_metal_kernels)

  target_link_libraries(benchmark_metal_gac PRIVATE
    cpim_model
    glog::glog
    absl::flags
    absl::flags_parse
    "-framework Foundation"
    "-framework Metal"
  )

  target_compile_options(benchmark_metal_gac PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
  )

  add_custom_command(
    TARGET benchmark_metal_gac
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CPIM_METAL_LIB}
            $<TARGET_FILE_DIR:benchmark_metal_gac>/metal_gac.metallib
    VERBATIM
  )

  add_test(
    NAME compare_cpu_metal_queens4
    COMMAND $<TARGET_FILE:compare_cpu_metal>
            --input=${CMAKE_SOURCE_DIR}/tests/data/bench/queens-4_ext.xml
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  )
  add_test(
    NAME compare_cpu_metal_deletion
    COMMAND $<TARGET_FILE:compare_cpu_metal>
            --input=${CMAKE_SOURCE_DIR}/tests/data/bench/test.xml
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  )
  add_test(
    NAME compare_cpu_metal_inconsistent
    COMMAND $<TARGET_FILE:compare_cpu_metal>
            --input=${CMAKE_SOURCE_DIR}/tests/data/metal/gac_inconsistent.xml
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  )
  add_test(
    NAME compare_cpu_metal_bitwords2
    COMMAND $<TARGET_FILE:compare_cpu_metal>
            --input=${CMAKE_SOURCE_DIR}/tests/data/metal/gac_bitwords2.xml
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  )
  add_test(
    NAME compare_cpu_metal_manifest
    COMMAND $<TARGET_FILE:compare_cpu_metal>
            --input=${CMAKE_SOURCE_DIR}/tests/data/bench/BMPath.xml
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  )

  set_tests_properties(
    compare_cpu_metal_queens4
    compare_cpu_metal_deletion
    compare_cpu_metal_inconsistent
    compare_cpu_metal_bitwords2
    compare_cpu_metal_manifest
    PROPERTIES
      LABELS "metal;gac"
      TIMEOUT 120
  )

  add_test(
    NAME benchmark_metal_gac_smoke
    COMMAND $<TARGET_FILE:benchmark_metal_gac>
            --input=${CMAKE_SOURCE_DIR}/tests/data/metal/gac_bitwords2.xml
            --runs=2
            --warmup=1
            --verify=true
            --readonly_storage=private
            --frontier_mode=compact
            --csv=${CMAKE_BINARY_DIR}/metal_gac_benchmark_smoke.csv
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
  )

  set_tests_properties(benchmark_metal_gac_smoke PROPERTIES
    LABELS "metal;gac;benchmark"
    TIMEOUT 120
  )
endif()

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

if(CPIM_CUDA_ENABLED)
# 添加 CUDA 相关的路径
include_directories(${CUDA_INCLUDE_DIRS})
link_directories(${CUDA_TOOLKIT_ROOT_DIR}/lib64)

# ============================================================================
# compare_cpu_gpu: CPU vs GPU 求解器对比测试
# ============================================================================
add_executable(compare_cpu_gpu
  apps/compare_cpu_gpu.cpp
  src/solver/gpu/GModel.cu
  src/solver/gpu/batch_probe_manager.cu
  src/solver/gpu/GModelSolver.cu
  src/model/gmodel_adapter.cu
  src/model/gmodel_validator.cu
)

set_target_properties(compare_cpu_gpu PROPERTIES
  CUDA_SEPARABLE_COMPILATION ON
)

target_link_libraries(compare_cpu_gpu PRIVATE
  cpim_model
  cpim_solver_cpu
  glog::glog
  absl::flags
  absl::flags_parse
  cuda
  cudart
)

# ARM 平台 NEON 禁用
if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64|arm")
  target_compile_options(compare_cpu_gpu PRIVATE
    $<$<COMPILE_LANGUAGE:CUDA>:--compiler-options=-U__ARM_NEON>
    $<$<COMPILE_LANGUAGE:CUDA>:--compiler-options=-U__ARM_NEON__>
    $<$<COMPILE_LANGUAGE:CUDA>:--compiler-options=-DABSL_HAVE_NEON=0>
  )
endif()
endif()

# ============================================================================
# verify_gac: GAC 正确性验证工具 (穷举法对比 AC3bit)
# ============================================================================
add_executable(verify_gac
  apps/verify_gac.cpp
)

target_link_libraries(verify_gac PRIVATE
  cpim_model
  cpim_solver_cpu
  glog::glog
  absl::flags
  absl::flags_parse
)

# ============================================================================
# verify_search: MAC 搜索过程正确性验证工具
# ============================================================================
add_executable(verify_search
  apps/verify_search.cpp
)

target_link_libraries(verify_search PRIVATE
  cpim_model
  cpim_solver_cpu
  glog::glog
  absl::flags
  absl::flags_parse
)

# ============================================================================
# test_batch3a: Batch-3A (约束聚合) 测试
# ============================================================================
if(CPIM_CUDA_ENABLED)
add_executable(test_batch3a
  tests/cpp/test_batch3a.cpp
  src/solver/gpu/GModel.cu
  src/solver/gpu/batch_probe_manager.cu
  src/model/gmodel_adapter.cu
  src/model/gmodel_validator.cu
)

set_target_properties(test_batch3a PROPERTIES
  CUDA_SEPARABLE_COMPILATION ON
)

target_link_libraries(test_batch3a PRIVATE
  cpim_model
  cpim_solver_cpu
  glog::glog
  absl::flags
  absl::flags_parse
  cuda
  cudart
)

target_compile_definitions(test_batch3a PRIVATE
  CPIM_PROJECT_ROOT="${CMAKE_SOURCE_DIR}"
)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64|arm")
  target_compile_options(test_batch3a PRIVATE
    $<$<COMPILE_LANGUAGE:CUDA>:--compiler-options=-U__ARM_NEON>
    $<$<COMPILE_LANGUAGE:CUDA>:--compiler-options=-U__ARM_NEON__>
    $<$<COMPILE_LANGUAGE:CUDA>:--compiler-options=-DABSL_HAVE_NEON=0>
  )
endif()

add_test(
  NAME test_batch3a
  COMMAND test_batch3a --input=${CMAKE_SOURCE_DIR}/tests/data/bench/queens-4_ext.xml --num_probes=10
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

set_tests_properties(test_batch3a PROPERTIES
  LABELS "gpu;batch_ac;batch3a"
  TIMEOUT 120
)

# ============================================================================
# test_fqpt_baseline: FQ-PT (Flattened Queue Persistent Threads) 基线测试
# ============================================================================
add_executable(test_fqpt_baseline
  tests/cpp/test_fqpt_baseline.cpp
  src/solver/gpu/GModel.cu
  src/solver/gpu/batch_probe_manager.cu
  src/model/gmodel_adapter.cu
  src/model/gmodel_validator.cu
)

set_target_properties(test_fqpt_baseline PROPERTIES
  CUDA_SEPARABLE_COMPILATION ON
)

target_link_libraries(test_fqpt_baseline PRIVATE
  cpim_model
  cpim_solver_cpu
  glog::glog
  absl::flags
  absl::flags_parse
  cuda
  cudart
)

target_compile_definitions(test_fqpt_baseline PRIVATE
  CPIM_PROJECT_ROOT="${CMAKE_SOURCE_DIR}"
)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64|arm")
  target_compile_options(test_fqpt_baseline PRIVATE
    $<$<COMPILE_LANGUAGE:CUDA>:--compiler-options=-U__ARM_NEON>
    $<$<COMPILE_LANGUAGE:CUDA>:--compiler-options=-U__ARM_NEON__>
    $<$<COMPILE_LANGUAGE:CUDA>:--compiler-options=-DABSL_HAVE_NEON=0>
  )
endif()

add_test(
  NAME test_fqpt_baseline
  COMMAND test_fqpt_baseline --input=${CMAKE_SOURCE_DIR}/tests/data/bench/queens-4_ext.xml --num_probes=16
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

set_tests_properties(test_fqpt_baseline PROPERTIES
  LABELS "gpu;batch_ac;fqpt"
  TIMEOUT 120
)

# ============================================================================
# sac_benchmark: SAC-GPU 统一基准测试工具
# ============================================================================
add_executable(sac_benchmark
  apps/sac_benchmark.cpp
  src/solver/gpu/GModel.cu
  src/solver/gpu/GModelSolver.cu
  src/solver/gpu/batch_probe_manager.cu
  src/model/gmodel_adapter.cu
  src/model/gmodel_validator.cu
)

set_target_properties(sac_benchmark PROPERTIES
  CUDA_SEPARABLE_COMPILATION ON
)

target_link_libraries(sac_benchmark PRIVATE
  cpim_model
  cpim_solver_cpu
  glog::glog
  absl::flags
  absl::flags_parse
  cuda
  cudart
)

target_compile_definitions(sac_benchmark PRIVATE
  CPIM_PROJECT_ROOT="${CMAKE_SOURCE_DIR}"
)

if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|ARM64|arm")
  target_compile_options(sac_benchmark PRIVATE
    $<$<COMPILE_LANGUAGE:CUDA>:--compiler-options=-U__ARM_NEON>
    $<$<COMPILE_LANGUAGE:CUDA>:--compiler-options=-U__ARM_NEON__>
    $<$<COMPILE_LANGUAGE:CUDA>:--compiler-options=-DABSL_HAVE_NEON=0>
  )
endif()
endif()
