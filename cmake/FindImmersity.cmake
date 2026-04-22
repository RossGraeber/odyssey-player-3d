# FindImmersity.cmake
#
# Locates the Immersity / LeiaSR SDK and exposes:
#   Immersity_INCLUDE_DIRS   - headers (contains sr/)
#   Immersity_LIBRARIES      - import libs to link
#   Immersity_RUNTIME_DLLS   - list of DLLs to stage next to the exe
#   Immersity_FOUND          - set when everything resolves
#
# Search order:
#   1. IMMERSITY_SDK_ROOT env var
#   2. D:/Sources/LeiaSR-SDK-1.34.8-RC1-win64  (canonical path on this machine,
#      learned from D:/Sources/Immersity-Demos/CMakeLists.txt)
#   3. C:/Program Files/Immersity SDK
#
# Runtime DLLs ship with the LeiaSR Platform installer (not in the SDK archive)
# so we additionally probe C:/Program Files/LeiaSR/Platform/bin for them.

set(_immersity_search_paths "")
if(DEFINED ENV{IMMERSITY_SDK_ROOT})
    list(APPEND _immersity_search_paths "$ENV{IMMERSITY_SDK_ROOT}")
endif()
list(APPEND _immersity_search_paths
    "D:/Sources/LeiaSR-SDK-1.34.8-RC1-win64"
    "C:/Program Files/Immersity SDK"
)

set(Immersity_ROOT "")
foreach(_p IN LISTS _immersity_search_paths)
    if(EXISTS "${_p}/include/sr" AND EXISTS "${_p}/lib")
        set(Immersity_ROOT "${_p}")
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
    # sr/sense/core/transformation.h #includes <opencv2/opencv.hpp>; the SDK
    # ships the matching OpenCV under third_party, so pull its include dir in.
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

# The SDK's transformation.h pulls OpenCV inlines that emit references to
# cv::String::deallocate — the SDK ships opencv_world343 to resolve them.
set(_immersity_opencv_lib "${Immersity_ROOT}/third_party/OpenCV/lib/x64/opencv_world343.lib")
if(EXISTS "${_immersity_opencv_lib}")
    list(APPEND Immersity_LIBRARIES "${_immersity_opencv_lib}")
endif()

# Runtime DLLs — canonical location on this machine is the LeiaSR Platform
# install. Fall back to searching the SDK root in case a future SDK drop ships
# DLLs alongside the libs.
set(_immersity_dll_dirs
    "C:/Program Files/LeiaSR/Platform/bin"
    "${Immersity_ROOT}/bin"
    "${Immersity_ROOT}/lib"
)

set(_immersity_dll_names
    simulatedreality
    SimulatedRealityCore
    SimulatedRealityDirectX
    SimulatedRealityDisplays
    SimulatedRealityCameras
    SimulatedRealityFaceTrackers
    SimulatedRealityHandTrackers
    SimulatedRealityUserModelers
    DimencoWeaving
    libserialport
    glog
    opencv_world343
)

set(Immersity_RUNTIME_DLLS "")
foreach(_dll IN LISTS _immersity_dll_names)
    set(_found "")
    foreach(_dir IN LISTS _immersity_dll_dirs)
        if(EXISTS "${_dir}/${_dll}.dll")
            set(_found "${_dir}/${_dll}.dll")
            break()
        endif()
    endforeach()
    if(_found)
        list(APPEND Immersity_RUNTIME_DLLS "${_found}")
    endif()
endforeach()

if(NOT Immersity_RUNTIME_DLLS)
    message(WARNING
        "Immersity SDK found at ${Immersity_ROOT} but no runtime DLLs were "
        "located. Expected them in C:/Program Files/LeiaSR/Platform/bin. "
        "The build will still link, but the exe will not run until the "
        "LeiaSR Platform is installed.")
endif()

set(Immersity_FOUND TRUE)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(Immersity
    REQUIRED_VARS Immersity_INCLUDE_DIRS Immersity_LIBRARIES)
