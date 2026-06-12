# Vitis HLS entry point for the first Zynq-7020 profile.
#
# Usage:
#   vitis_hls -f fpga_cpim/hls/run_hls.tcl
# Optional:
#   export RUN_COSIM=1

set script_dir [file dirname [file normalize [info script]]]
set repo_root [file normalize [file join $script_dir .. ..]]
set hls_dir [file join $repo_root fpga_cpim hls]

set project_name "fpga_cpim_hls"
set solution_name "z7020_small"
set top_name "cpim_top_hls"

open_project -reset $project_name
set_top $top_name

add_files [file join $hls_dir cpim_hls_types.hpp] -cflags "-std=c++17 -I$hls_dir"
add_files [file join $hls_dir cpim_top_hls.cpp] -cflags "-std=c++17 -I$hls_dir"
add_files [file join $hls_dir event_router_hls.cpp] -cflags "-std=c++17 -I$hls_dir"
add_files [file join $hls_dir revise_tile_hls.cpp] -cflags "-std=c++17 -I$hls_dir"
add_files [file join $hls_dir variable_owner_hls.cpp] -cflags "-std=c++17 -I$hls_dir"
add_files -tb [file join $hls_dir testbench_hls.cpp] -cflags "-std=c++17 -I$hls_dir"

open_solution -reset $solution_name
set_part {xc7z020clg400-1}
create_clock -period 10 -name default

csim_design
csynth_design

if {[info exists ::env(RUN_COSIM)] && $::env(RUN_COSIM) eq "1"} {
  cosim_design
}

exit
