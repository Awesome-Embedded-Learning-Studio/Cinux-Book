message("Start Configuring the Base library of Cinux")

# Currently, it is just a interface library, for it is header only :)
add_library(cinux_base_include_only INTERFACE)
add_library(cinux_base STATIC src/format/entry.cpp src/format/radix.cpp)

target_include_directories(cinux_base_include_only INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(cinux_base PUBLIC cinux_base_include_only)
target_link_libraries(cinux_base PRIVATE cinux_warnings)