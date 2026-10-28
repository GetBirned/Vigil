add_library(vigil_common
  src/messages.cpp
  src/udp.cpp
  src/latency_histogram.cpp)
add_library(vigil::common ALIAS vigil_common)

target_include_directories(vigil_common PUBLIC include)
target_link_libraries(vigil_common PUBLIC Threads::Threads PRIVATE vigil_options)
