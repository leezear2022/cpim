# Vitis HLS entry point for the first Zynq-7020 profile.
#
# Usage:
#   vitis_hls -f fpga_cpim/hls/run_hls.tcl
# Optional:
#   export PROFILE=z7020_probe2
#   export RUN_COSIM=1

set script_dir [file dirname [file normalize [info script]]]
set repo_root [file normalize [file join $script_dir .. ..]]
set hls_dir [file join $repo_root fpga_cpim hls]

set project_name "fpga_cpim_hls"
set profile_name "z7020_small"
if {[info exists ::env(PROFILE)] && $::env(PROFILE) ne ""} {
  set profile_name $::env(PROFILE)
}

if {$profile_name eq "z7020_small"} {
  set profile_macro "FPGA_CPIM_HLS_PROFILE_Z7020_SMALL"
} elseif {$profile_name eq "z7020_probe2"} {
  set profile_macro "FPGA_CPIM_HLS_PROFILE_Z7020_PROBE2"
} elseif {$profile_name eq "stress128"} {
  set profile_macro "FPGA_CPIM_HLS_PROFILE_STRESS128"
} else {
  error "unsupported PROFILE=$profile_name"
}

set solution_name $profile_name
set top_name "cpim_top_hls"
set common_cflags "-std=c++17 -I$hls_dir -DFPGA_CPIM_HLS_PROFILE=$profile_macro"

open_project -reset $project_name
set_top $top_name

add_files [file join $hls_dir cpim_hls_types.hpp] -cflags $common_cflags
add_files [file join $hls_dir cpim_top_hls.cpp] -cflags $common_cflags
add_files [file join $hls_dir event_router_hls.cpp] -cflags $common_cflags
add_files [file join $hls_dir revise_tile_hls.cpp] -cflags $common_cflags
add_files [file join $hls_dir variable_owner_hls.cpp] -cflags $common_cflags
add_files -tb [file join $hls_dir testbench_hls.cpp] -cflags $common_cflags

open_solution -reset $solution_name
set_part {xc7z020clg400-1}
create_clock -period 10 -name default

csim_design
csynth_design

if {[info exists ::env(RUN_COSIM)] && $::env(RUN_COSIM) eq "1"} {
  cosim_design
}

exit
