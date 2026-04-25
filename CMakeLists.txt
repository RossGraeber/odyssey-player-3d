cmake_minimum_required(VERSION 3.21)
project(odyssey-player-3d VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

find_package(Immersity REQUIRED)
find_package(FFMPEG REQUIRED)

add_executable(odyssey WIN32
    src/main.cpp
    src/app/Win32Window.h
    src/app/Win32Window.cpp
    src/app/D3D11Device.h
    src/app/D3D11Device.cpp
    src/app/AppShell.h
    src/app/AppShell.cpp
    src/app/ImmersityWeaver.h
    src/app/ImmersityWeaver.cpp
    src/app/VideoPipeline.h
    src/app/VideoPipeline.cpp
    src/app/Nv12ToRgba.h
    src/app/Nv12ToRgba.cpp
    src/app/FrameMailbox.h
    src/app/PtsMath.h
)

set_target_properties(odyssey PROPERTIES OUTPUT_NAME "odyssey")

target_include_directories(odyssey PRIVATE src)

# Immersity/OpenCV/FFmpeg headers trip /W4 /WX (C4864, C4201, C4005, C4505,
# ffmpeg deprecation). Treat them as system headers so those warnings are
# suppressed for SDK code only — our own code is still held to the full bar.
target_include_directories(odyssey SYSTEM PRIVATE
    ${Immersity_INCLUDE_DIRS}
    ${FFMPEG_INCLUDE_DIRS}
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
    ${FFMPEG_LIBRARIES}
    d3d11
    dxgi
    dxguid
    d3dcompiler
    windowscodecs
    ole32
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

add_test(NAME odyssey-m1-spike-smoke
    COMMAND odyssey --spike-smoke "${CMAKE_SOURCE_DIR}/assets/m1_sbs_fellowship.png"
)
set_tests_properties(odyssey-m1-spike-smoke PROPERTIES
    TIMEOUT 30
    SKIP_RETURN_CODE 77
)

# M2: --play-smoke reads the MKV path from ODYSSEY_TEST_M2_FSBS_MKV when no
# positional path is passed; if the env var is unset or the file is absent,
# AppShell exits 77 and CTest marks the test SKIPPED. That keeps the registered
# test runnable on any dev box while still failing loudly when the corpus is
# supposed to be present.
add_test(NAME odyssey-m2-play-smoke COMMAND odyssey --play-smoke)
set_tests_properties(odyssey-m2-play-smoke PROPERTIES
    TIMEOUT 60
    SKIP_RETURN_CODE 77
)

# Unit tests — behavior over coverage. Linked against gtest only (no app
# sources needed; the tested units are header-only).
find_package(GTest CONFIG REQUIRED)
add_executable(odyssey_tests
    tests/FrameMailboxTests.cpp
    tests/PtsMathTests.cpp
)
target_include_directories(odyssey_tests PRIVATE src)
target_link_libraries(odyssey_tests PRIVATE GTest::gtest GTest::gtest_main)
if(MSVC)
    target_compile_options(odyssey_tests PRIVATE /W4 /permissive-)
endif()
add_test(NAME odyssey-unit COMMAND odyssey_tests)
set_tests_properties(odyssey-unit PROPERTIES TIMEOUT 30)
