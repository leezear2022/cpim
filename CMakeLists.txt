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
set(CMAKE_CUDA_STANDARD 20)

# 设置 CUDA 架构
set(CMAKE_CUDA_ARCHITECTURES 60 70 80)

# 添加 CUDA Toolkit 的 include 路径
include_directories(/usr/local/cuda/include)
include_directories(${CMAKE_CUDA_TOOLKIT_INCLUDE_DIRECTORIES})

# 添加库文件路径
link_directories(/usr/local/cuda/lib64)

find_package(LibXml2 REQUIRED)
include_directories(${LIBXML2_INCLUDE_DIR})

find_package(XercesC REQUIRED)
include_directories(${XercesC_INCLUDE_DIRS})

# 添加头文件目录
include_directories(include)

# 添加子目录
add_subdirectory(xcsp3parser)

add_executable(cpim samples/main.cu
        src/HModel.cpp
        src/XBuilder.cpp
        include/NaiveBitSet.h
        include/CPIMBase.h
        src/Network.cpp
        src/Solver.cpp
        include/Timer.h
        include/cuSAC.cuh
)

set_target_properties(cpim PROPERTIES
        CUDA_SEPARABLE_COMPILATION ON)

# 链接 xcsp3parser 静态库和 CUDA 相关库
target_link_libraries(cpim PRIVATE xcsp3parser ${XercesC_LIBRARIES} ${LIBXML2_LIBRARIES}
        cuda cudart cudadevrt cudart_static)

# 确保 CUDA 链接器标志被设置
set_target_properties(cpim PROPERTIES CUDA_SEPARABLE_COMPILATION ON)

# 添加 CUDA 相关的路径
include_directories(${CUDA_INCLUDE_DIRS})
link_directories(${CUDA_TOOLKIT_ROOT_DIR}/lib64)

