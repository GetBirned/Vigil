add_library(vigil_tracker_lib
  src/kalman.cpp
  src/track_manager.cpp)
add_library(vigil::tracker ALIAS vigil_tracker_lib)
target_include_directories(vigil_tracker_lib PUBLIC include)
target_link_libraries(vigil_tracker_lib PUBLIC vigil::common Eigen3::Eigen PRIVATE vigil_options)

add_executable(vigil-tracker src/main.cpp)
target_link_libraries(vigil-tracker PRIVATE vigil::tracker vigil::common vigil_options)
