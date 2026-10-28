find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(vigil_tests
  test_messages.cpp
  test_udp.cpp
  test_scenario.cpp
  test_kalman.cpp
  test_track_manager.cpp
  test_publish.cpp
  test_spatial_grid.cpp
  test_spsc_queue.cpp
  test_latency_histogram.cpp)
target_link_libraries(vigil_tests PRIVATE vigil::common vigil::sim vigil::tracker GTest::gtest_main
                                        vigil_options)

gtest_discover_tests(vigil_tests)
