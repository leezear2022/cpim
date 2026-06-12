cmake_minimum_required(VERSION 3.18)
project(fpga_cpim LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)

set(FPGA_CPIM_HEADERS
  include/fpga_cpim/bitset.hpp
  include/fpga_cpim/engine.hpp
  include/fpga_cpim/event_router.hpp
  include/fpga_cpim/golden.hpp
  include/fpga_cpim/model.hpp
  include/fpga_cpim/node_command.hpp
  include/fpga_cpim/partition.hpp
  include/fpga_cpim/revise_tile.hpp
  include/fpga_cpim/sim_config.hpp
  include/fpga_cpim/sim_stats.hpp
  include/fpga_cpim/support_oracle.hpp
  include/fpga_cpim/termination.hpp
  include/fpga_cpim/trace.hpp
  include/fpga_cpim/variable_owner.hpp
  include/fpga_cpim/world.hpp
)

set(FPGA_CPIM_SOURCES
  src/bitset.cpp
  src/engine.cpp
  src/event_router.cpp
  src/golden.cpp
  src/model.cpp
  src/node_command.cpp
  src/partition.cpp
  src/revise_tile.cpp
  src/sim_stats.cpp
  src/support_oracle.cpp
  src/termination.cpp
  src/trace.cpp
  src/variable_owner.cpp
  src/world.cpp
)

add_library(fpga_cpim_core STATIC ${FPGA_CPIM_HEADERS} ${FPGA_CPIM_SOURCES})
target_include_directories(fpga_cpim_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(fpga_cpim_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(fpga_cpim_sim src/main.cpp)
target_link_libraries(fpga_cpim_sim PRIVATE fpga_cpim_core)
target_compile_options(fpga_cpim_sim PRIVATE -Wall -Wextra -Wpedantic)

function(add_fpga_cpim_test name)
  add_executable(${name} tests/${name}.cpp)
  target_link_libraries(${name} PRIVATE fpga_cpim_core)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests)
  target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

if(BUILD_TESTING)
  add_fpga_cpim_test(test_bitset)
  add_fpga_cpim_test(test_support_oracle)
  add_fpga_cpim_test(test_revise_tile)
  add_fpga_cpim_test(test_variable_owner)
  add_fpga_cpim_test(test_event_router)
  add_fpga_cpim_test(test_termination)
  add_fpga_cpim_test(test_engine_ac)
  add_fpga_cpim_test(test_engine_sacq)
  add_fpga_cpim_test(test_engine_nsacq)
  add_fpga_cpim_test(test_overflow_unknown)
  add_fpga_cpim_test(test_partition)
  add_fpga_cpim_test(test_node_command)

  set(FPGA_CPIM_HLS_SOURCES
    hls/testbench_hls.cpp
    hls/cpim_top_hls.cpp
    hls/event_router_hls.cpp
    hls/revise_tile_hls.cpp
    hls/variable_owner_hls.cpp
  )

  function(add_hls_tb_profile name profile_macro)
    add_executable(${name} ${FPGA_CPIM_HLS_SOURCES})
    target_include_directories(${name} PRIVATE hls)
    target_compile_definitions(${name} PRIVATE
      FPGA_CPIM_HLS_PROFILE=${profile_macro})
    target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic)
  endfunction()

  add_hls_tb_profile(hls_tb FPGA_CPIM_HLS_PROFILE_STRESS128)
  add_test(NAME hls_tb COMMAND hls_tb)

  add_hls_tb_profile(hls_tb_z7020_small
                     FPGA_CPIM_HLS_PROFILE_Z7020_SMALL)
  add_test(NAME hls_tb_z7020_small
           COMMAND hls_tb_z7020_small --profile=z7020_small
                   --tiles=1 --capacity-sweep=512
                   --fixtures=chain,random,hub)

  add_hls_tb_profile(hls_tb_z7020_probe2
                     FPGA_CPIM_HLS_PROFILE_Z7020_PROBE2)
  add_test(NAME hls_tb_z7020_probe2
           COMMAND hls_tb_z7020_probe2 --profile=z7020_probe2
                   --tiles=1 --capacity-sweep=512
                   --fixtures=chain,random,hub)
endif()
