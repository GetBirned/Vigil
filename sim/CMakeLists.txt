add_library(vigil_sim_lib src/scenario.cpp)
add_library(vigil::sim ALIAS vigil_sim_lib)
target_include_directories(vigil_sim_lib PUBLIC include)
target_link_libraries(vigil_sim_lib PUBLIC vigil::common PRIVATE vigil_options)

add_executable(vigil-sim src/main.cpp)
target_link_libraries(vigil-sim PRIVATE vigil::sim vigil::common vigil_options)
