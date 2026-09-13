cmake_minimum_required(VERSION 3.21)
project(odyssey_mvc_probe LANGUAGES CXX)

if(NOT LAV_INCLUDE_DIR)
    message(FATAL_ERROR "Set LAV_INCLUDE_DIR to the x64 LAV Filters include directory")
endif()

add_executable(mvc_probe mvc_probe.cpp)
target_compile_features(mvc_probe PRIVATE cxx_std_17)
target_include_directories(mvc_probe PRIVATE "${LAV_INCLUDE_DIR}")
target_compile_definitions(mvc_probe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(mvc_probe PRIVATE ole32 oleaut32 strmiids)

if(MSVC)
    target_compile_options(mvc_probe PRIVATE /W4 /WX /permissive-)
endif()
