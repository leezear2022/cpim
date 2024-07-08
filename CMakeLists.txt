cmake_minimum_required(VERSION 3.18)

set(CMAKE_CUDA_COMPILER /usr/local/cuda/bin/nvcc)

project(cpim CUDA CXX)

set(CMAKE_CUDA_STANDARD 23)

# 设置 CUDA 架构
set(CMAKE_CUDA_ARCHITECTURES 60 70 80)
find_package(LibXml2 REQUIRED)
include_directories(${LIBXML2_INCLUDE_DIR})
# 添加头文件目录
include_directories(include)

# 添加子目录
add_subdirectory(xcsp3parser)

add_executable(cpim samples/main.cu
        samples/main.cpp
        src/HModel.cpp
        )

set_target_properties(cpim PROPERTIES
        CUDA_SEPARABLE_COMPILATION ON)

# 链接 xcsp3parser 静态库
target_link_libraries(cpim PRIVATE xcsp3parser)
