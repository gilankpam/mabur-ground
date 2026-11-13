# linkbench-rx -- the GS side of mabur's linkbench (iperf-like link bench).
# The pipeline (linkbench_core: bench_wire.h, rx_pipeline.h) lives in mabur's
# bench/linkbench next to linkbench-tx; this is only the radio-facing binary.
if(TARGET devourer AND TARGET mabur_gs_radio AND TARGET linkbench_core)
  add_executable(linkbench-rx rx_main.cpp)
  target_link_libraries(linkbench-rx PRIVATE linkbench_core mabur_gs_radio devourer pthread)
endif()
