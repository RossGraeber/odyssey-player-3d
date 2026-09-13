# FindImmersity.cmake
#
# Locates the Immersity / LeiaSR SDK and exposes:
#   Immersity_INCLUDE_DIRS   - headers (contains sr/)
#   Immersity_LIBRARIES      - import libs to link
#   Immersity_DELAYLOAD_DLLS - direct runtime imports loaded at startup
#   Immersity_FOUND          - set when the compile-time SDK resolves

set(_immersity_search_paths "")
if(DEFINED ENV{IMMERSITY_SDK_ROOT})
    list(APPEND _immersity_search_paths "$ENV{IMMERSITY_SDK_ROOT}")
endif()
list(APPEND _immersity_search_paths
    "D:/Sources/LeiaSR-SDK-1.34.8-RC1-win64"
    "C:/Program Files/Immersity SDK"
)

set(Immersity_ROOT "")
foreach(_path IN LISTS _immersity_search_paths)
    if(EXISTS "${_path}/include/sr" AND EXISTS "${_path}/lib")
        set(Immersity_ROOT "${_path}")
        break()
    endif()
endforeach()

if(NOT Immersity_ROOT)
    message(FATAL_ERROR
        "Immersity SDK not found. Set IMMERSITY_SDK_ROOT to the SDK root "
        "(must contain include/sr/ and lib/). Searched: ${_immersity_search_paths}")
endif()

set(Immersity_INCLUDE_DIRS
    "${Immersity_ROOT}/include"
    "${Immersity_ROOT}/third_party/OpenCV/include"
)

set(_immersity_lib_names
    simulatedreality
    SimulatedRealityCore
    SimulatedRealityDirectX
    SimulatedRealityDisplays
    SimulatedRealityCameras
    SimulatedRealityFaceTrackers
    SimulatedRealityHandTrackers
    SimulatedRealityUserModelers
    DimencoWeaving
)

set(Immersity_LIBRARIES "")
foreach(_lib IN LISTS _immersity_lib_names)
    set(_full "${Immersity_ROOT}/lib/${_lib}.lib")
    if(NOT EXISTS "${_full}")
        message(FATAL_ERROR "Immersity SDK missing import lib: ${_full}")
    endif()
    list(APPEND Immersity_LIBRARIES "${_full}")
endforeach()

set(_immersity_opencv_lib "${Immersity_ROOT}/third_party/OpenCV/lib/x64/opencv_world343.lib")
if(EXISTS "${_immersity_opencv_lib}")
    list(APPEND Immersity_LIBRARIES "${_immersity_opencv_lib}")
endif()

# These are the application's direct imports. Their transitive dependencies
# are resolved from the installed Platform directory by ImmersityRuntime.
set(Immersity_DELAYLOAD_DLLS
    opencv_world343.dll
    SimulatedRealityCore.dll
    SimulatedRealityDisplays.dll
    SimulatedRealityDirectX.dll
)

set(Immersity_FOUND TRUE)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Immersity
    REQUIRED_VARS Immersity_INCLUDE_DIRS Immersity_LIBRARIES)
