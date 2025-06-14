cmake_minimum_required(VERSION 3.24)

add_library(signalsmith-util INTERFACE)
set_target_properties(signalsmith-util PROPERTIES INTERFACE_INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR}/include)
