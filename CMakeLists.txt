cmake_minimum_required(VERSION 3.21)
project(odyssey-player-3d VERSION 0.0.1 LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

find_package(Immersity REQUIRED)
find_package(FFMPEG REQUIRED)

set(ODYSSEY_LAV_ROOT
    "${CMAKE_SOURCE_DIR}/build/mvc_probe/deps/lav-0.83"
    CACHE PATH "Absolute path to the approved private LAV Filters 0.83 package"
)
get_filename_component(ODYSSEY_LAV_ROOT "${ODYSSEY_LAV_ROOT}" ABSOLUTE)
set(_odyssey_lav_runtime_files
    LAVSplitter.ax LAVVideo.ax LAVAudio.ax
    avcodec-lav-63.dll avfilter-lav-12.dll avformat-lav-63.dll
    avutil-lav-61.dll swresample-lav-7.dll swscale-lav-10.dll
    libbluray.dll IntelQuickSyncDecoder.dll libmfxsw64.dll
    LAVFilters.Dependencies.manifest COPYING README.md CHANGELOG.txt
)
foreach(_lav_file IN LISTS _odyssey_lav_runtime_files)
    if(NOT EXISTS "${ODYSSEY_LAV_ROOT}/${_lav_file}")
        message(FATAL_ERROR "ODYSSEY_LAV_ROOT is missing ${_lav_file}: ${ODYSSEY_LAV_ROOT}")
    endif()
endforeach()
foreach(_lav_header IN ITEMS IMediaSample3D.h LAVSplitterSettings.h LAVVideoSettings.h LAVAudioSettings.h)
    if(NOT EXISTS "${ODYSSEY_LAV_ROOT}/include/${_lav_header}")
        message(FATAL_ERROR "ODYSSEY_LAV_ROOT is missing include/${_lav_header}: ${ODYSSEY_LAV_ROOT}")
    endif()
endforeach()

add_executable(odyssey WIN32
    src/main.cpp
    src/app/Odyssey.rc
    src/app/Win32Window.h
    src/app/Win32Window.cpp
    src/app/D3D11Device.h
    src/app/D3D11Device.cpp
    src/app/AppShell.h
    src/app/AppShell.cpp
    src/app/AudioOutput.h
    src/app/AudioOutput.cpp
    src/app/BluRayTitles.h
    src/app/BluRayTitles.cpp
    src/app/IsoMount.h
    src/app/IsoMount.cpp
    src/app/MvcPipeline.h
    src/app/MvcPipeline.cpp
    src/app/MvcSubtitleTiming.h
    src/app/MvcSubtitleTiming.cpp
    src/app/PlaybackSession.h
    src/app/PlaybackSession.cpp
    src/app/PlayerControls.h
    src/app/PlayerControls.cpp
    src/app/ImmersityRuntime.h
    src/app/ImmersityRuntime.cpp
    src/app/ImmersityWeaver.h
    src/app/ImmersityWeaver.cpp
    src/app/VideoPipeline.h
    src/app/VideoPipeline.cpp
    src/app/SubtitleCue.h
    src/app/SubtitleDecoder.h
    src/app/SubtitleDecoder.cpp
    src/app/SubtitleRenderer.h
    src/app/SubtitleRenderer.cpp
    src/app/Nv12ToRgba.h
    src/app/Nv12ToRgba.cpp
    src/app/StereoCompositor.h
    src/app/StereoCompositor.cpp
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
    "${ODYSSEY_LAV_ROOT}/include"
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
    foreach(_dll IN LISTS Immersity_DELAYLOAD_DLLS)
        target_link_options(odyssey PRIVATE "/DELAYLOAD:${_dll}")
    endforeach()
endif()

target_link_libraries(odyssey PRIVATE
    ${Immersity_LIBRARIES}
    ${FFMPEG_LIBRARIES}
    d3d11
    dxgi
    dxguid
    d3dcompiler
    delayimp
    gdi32
    windowscodecs
    ole32
    shell32
    VirtDisk
    Strmiids
    Quartz
)

set(_odyssey_lav_copy_commands)
foreach(_lav_file IN LISTS _odyssey_lav_runtime_files)
    list(APPEND _odyssey_lav_copy_commands
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ODYSSEY_LAV_ROOT}/${_lav_file}"
            "$<TARGET_FILE_DIR:odyssey>/lav/${_lav_file}"
    )
endforeach()
add_custom_command(TARGET odyssey POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:odyssey>/lav"
    ${_odyssey_lav_copy_commands}
    COMMENT "Staging private LAV Filters runtime under lav/"
    VERBATIM
)

install(TARGETS odyssey RUNTIME DESTINATION .)
install(FILES "${CMAKE_SOURCE_DIR}/README.md" DESTINATION .)
foreach(_lav_file IN LISTS _odyssey_lav_runtime_files)
    install(FILES "${ODYSSEY_LAV_ROOT}/${_lav_file}" DESTINATION lav)
endforeach()

if(WIN32)
    set(CPACK_GENERATOR "NSIS;WIX" CACHE STRING "Generators to use for CPack packaging on Windows")
    set(CPACK_PACKAGE_NAME "Odyssey Player 3D")
    set(CPACK_PACKAGE_VENDOR "Odyssey Player 3D")
    set(CPACK_PACKAGE_INSTALL_DIRECTORY "Odyssey Player 3D")
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    configure_file("${CMAKE_SOURCE_DIR}/LICENSE" "${CMAKE_BINARY_DIR}/LICENSE.txt" COPYONLY)
    set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_BINARY_DIR}/LICENSE.txt")
    set(CPACK_PACKAGE_ICON "${CMAKE_SOURCE_DIR}/src/app/odyssey.ico")
    set(CPACK_NSIS_MUI_ICON "${CMAKE_SOURCE_DIR}/src/app/odyssey.ico")
    set(CPACK_NSIS_MUI_UNIICON "${CMAKE_SOURCE_DIR}/src/app/odyssey.ico")
    set(CPACK_NSIS_INSTALLED_ICON_NAME "odyssey.exe")
    set(CPACK_WIX_PRODUCT_ICON "${CMAKE_SOURCE_DIR}/src/app/odyssey.ico")
    set(CPACK_WIX_VERSION 4)
    include(CPack)
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

# Unit tests — behavior over coverage. Production units are listed explicitly.
find_package(GTest CONFIG REQUIRED)
add_executable(odyssey_tests
    tests/FrameMailboxTests.cpp
    src/app/BluRayTitles.cpp
    tests/BluRayTitlesTests.cpp
    tests/FrameQueueTests.cpp
    src/app/ImmersityRuntime.cpp
    tests/ImmersityRuntimeTests.cpp
    src/app/Nv12ToRgba.cpp
    tests/Nv12ToRgbaTests.cpp
    tests/PtsMathTests.cpp
    src/app/PlayerControls.cpp
    tests/PlayerControlsTests.cpp
    src/app/StereoCompositor.cpp
    tests/StereoCompositorTests.cpp
    tests/StereoLayoutTests.cpp
    src/app/SubtitleDecoder.cpp
    tests/SubtitleDecoderTests.cpp
    src/app/SubtitleRenderer.cpp
    tests/SubtitleRendererTests.cpp
    src/app/MvcSubtitleTiming.cpp
    tests/MvcSubtitleTimingTests.cpp
    tests/VideoTimingTests.cpp
)
target_include_directories(odyssey_tests PRIVATE src)
target_include_directories(odyssey_tests SYSTEM PRIVATE ${FFMPEG_INCLUDE_DIRS})
target_link_libraries(odyssey_tests PRIVATE
    ${FFMPEG_LIBRARIES}
    GTest::gtest
    GTest::gtest_main
    d3d11
    d3dcompiler
    gdi32
    ole32
    shell32
)
if(MSVC)
    target_compile_options(odyssey_tests PRIVATE /W4 /permissive-)
endif()
add_test(NAME odyssey-unit COMMAND odyssey_tests)
set_tests_properties(odyssey-unit PROPERTIES TIMEOUT 30)
