find_package(benchmark QUIET)
if(NOT benchmark_FOUND)
  message(STATUS "Google Benchmark not found; skipping benchmarks")
  return()
endif()

add_executable(vigil_bench bench_tracker.cpp bench_queue.cpp)
target_link_libraries(vigil_bench PRIVATE vigil::tracker vigil::sim benchmark::benchmark_main
                                        vigil_options)
