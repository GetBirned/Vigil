add_library(vigil_common
  src/messages.cpp
  src/udp.cpp)
add_library(vigil::common ALIAS vigil_common)

target_include_directories(vigil_common PUBLIC include)
target_link_libraries(vigil_common PRIVATE vigil_options)
