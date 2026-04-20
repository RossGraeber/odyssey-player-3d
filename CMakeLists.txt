cmake_minimum_required(VERSION 3.21)
project(odyssey-player-3d VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

find_package(Immersity REQUIRED)

add_executable(odyssey WIN32
    src/main.cpp
    src/app/Win32Window.h
    src/app/Win32Window.cpp
    src/app/D3D11Device.h
    src/app/D3D11Device.cpp
    src/app/AppShell.h
    src/app/AppShell.cpp
)

set_target_properties(odyssey PROPERTIES OUTPUT_NAME "odyssey")

target_include_directories(odyssey PRIVATE
    src
    ${Immersity_INCLUDE_DIRS}
)

target_compile_definitions(odyssey PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
)

if(MSVC)
    target_compile_options(odyssey PRIVATE /W4 /WX /permissive-)
    target_link_options(odyssey PRIVATE /SUBSYSTEM:WINDOWS)
endif()

target_link_libraries(odyssey PRIVATE
    ${Immersity_LIBRARIES}
    d3d11
    dxgi
    dxguid
    d3dcompiler
)

if(Immersity_RUNTIME_DLLS)
    add_custom_command(TARGET odyssey POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:odyssey>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${Immersity_RUNTIME_DLLS}
            "$<TARGET_FILE_DIR:odyssey>"
        COMMENT "Staging Immersity runtime DLLs next to odyssey.exe"
        VERBATIM
    )
endif()

enable_testing()
add_test(NAME odyssey-smoke COMMAND odyssey --smoke-test)
set_tests_properties(odyssey-smoke PROPERTIES TIMEOUT 30)
