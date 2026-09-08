cmake_minimum_required (VERSION 3.16)

include(ExternalProject)
include(FetchContent)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
# Prefer the new boost helper
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

# macOS-specific cache options
set(ENABLE_HTML ON CACHE BOOL "Enable CEF and HTML producer")
set(USE_STATIC_BOOST OFF CACHE BOOL "Use shared library version of Boost")
set(CASPARCG_BINARY_NAME "casparcg" CACHE STRING "Custom name of the binary to build")
set(ENABLE_AVX2 ON CACHE BOOL "Enable the AVX2 instruction set (requires a CPU that supports it)")
set(ENABLE_VULKAN ON CACHE BOOL "Use Vulkan backend (required on macOS)")

# Determine build (target) platform
SET (PLATFORM_FOLDER_NAME "macos")

IF (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    MESSAGE (STATUS "Setting build type to 'Release' as none was specified.")
    SET (CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
    SET_PROPERTY (CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
ENDIF ()
MARK_AS_ADVANCED (CMAKE_INSTALL_PREFIX)

# Minimum macOS version
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Minimum macOS deployment target")

message(STATUS "=== CasparCG macOS Build Configuration ===")
message(STATUS "macOS Deployment Target: ${CMAKE_OSX_DEPLOYMENT_TARGET}")

# ============================================================================
# Boost
# ============================================================================
if (USE_STATIC_BOOST)
    SET (Boost_USE_STATIC_LIBS ON)
endif()
find_package(Boost 1.74.0 COMPONENTS thread filesystem log_setup log locale regex date_time coroutine REQUIRED)
SET (BOOST_INCLUDE_PATH "${Boost_INCLUDE_DIRS}")

# ============================================================================
# FFmpeg
# ============================================================================
find_package(FFmpeg REQUIRED COMPONENTS AVCODEC AVFORMAT AVUTIL AVDEVICE AVFILTER SWSCALE SWRESAMPLE)
SET (FFMPEG_INCLUDE_PATH "${FFMPEG_INCLUDE_DIRS}")
LINK_DIRECTORIES("${FFMPEG_LIBRARY_DIRS}")

# ============================================================================
# TBB (Threading Building Blocks)
# ============================================================================
find_package(TBB REQUIRED)

# ============================================================================
# OpenAL
# ============================================================================
find_package(OpenAL REQUIRED)

# Support for different OpenAL package configurations
if (NOT TARGET OpenAL::OpenAL)
    add_library(OpenAL::OpenAL INTERFACE IMPORTED)
    target_include_directories(OpenAL::OpenAL INTERFACE ${OPENAL_INCLUDE_DIR})
    target_link_libraries(OpenAL::OpenAL INTERFACE ${OPENAL_LIBRARY})
endif()

# ============================================================================
# Vulkan SDK / MoltenVK
# ============================================================================
find_package(Vulkan REQUIRED)

if (Vulkan_FOUND)
    message(STATUS "Vulkan found: ${Vulkan_LIBRARY}")
    message(STATUS "Vulkan include: ${Vulkan_INCLUDE_DIRS}")
else()
    message(FATAL_ERROR "Vulkan SDK not found. Please install from https://vulkan.lunarg.com/sdk/home")
endif()

if (NOT TARGET Vulkan::Vulkan)
    add_library(Vulkan::Vulkan INTERFACE IMPORTED)
    target_include_directories(Vulkan::Vulkan INTERFACE ${Vulkan_INCLUDE_DIRS})
    target_link_libraries(Vulkan::Vulkan INTERFACE ${Vulkan_LIBRARY})
endif()

# Add Vulkan loader library path to rpath so vk-bootstrap's dlopen can find it
get_filename_component(VULKAN_LOADER_DIR "${Vulkan_LIBRARY}" DIRECTORY)
list(APPEND CMAKE_BUILD_RPATH "${VULKAN_LOADER_DIR}")
list(APPEND CMAKE_INSTALL_RPATH "${VULKAN_LOADER_DIR}")

# ============================================================================
# vk-bootstrap and VulkanMemoryAllocator (matching Linux/Windows versions)
# ============================================================================
FetchContent_Declare(
    fetch_vk_bootstrap
    GIT_REPOSITORY https://github.com/charles-lunarg/vk-bootstrap
    GIT_TAG        v1.4.328
)
FetchContent_MakeAvailable(fetch_vk_bootstrap)

FetchContent_Declare(
    fetch_vma
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator
    GIT_TAG        v3.3.0
)
FetchContent_MakeAvailable(fetch_vma)

# ============================================================================
# GLFW (for window management, replacing SFML on macOS)
# ============================================================================
find_package(glfw3 3.3 QUIET)
if (NOT glfw3_FOUND)
    message(STATUS "GLFW not found via find_package, will use FetchContent")
    FetchContent_Declare(glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG 3.3.8
    )
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(glfw)
endif()

# ============================================================================
# GLEW stub (macOS uses Vulkan, not OpenGL, but some targets link GLEW::glew)
# ============================================================================
if (NOT TARGET GLEW::glew)
    add_library(GLEW::glew INTERFACE IMPORTED)
endif()

# ============================================================================
# Apple Frameworks
# ============================================================================
find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
find_library(METAL_FRAMEWORK Metal REQUIRED)
find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
find_library(IOKIT_FRAMEWORK IOKit REQUIRED)
find_library(COREVIDEO_FRAMEWORK CoreVideo REQUIRED)
find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)

message(STATUS "Apple frameworks found:")
message(STATUS "  Cocoa: ${COCOA_FRAMEWORK}")
message(STATUS "  Metal: ${METAL_FRAMEWORK}")
message(STATUS "  QuartzCore: ${QUARTZCORE_FRAMEWORK}")

add_library(Apple::Frameworks INTERFACE IMPORTED)
target_link_libraries(Apple::Frameworks INTERFACE
    ${COCOA_FRAMEWORK}
    ${METAL_FRAMEWORK}
    ${QUARTZCORE_FRAMEWORK}
    ${IOKIT_FRAMEWORK}
    ${COREVIDEO_FRAMEWORK}
    ${COREFOUNDATION_FRAMEWORK}
)

# ============================================================================
# CEF (Chromium Embedded Framework)
# ============================================================================
if (ENABLE_HTML)
    casparcg_add_external_project(cef)

    if (CMAKE_SYSTEM_PROCESSOR MATCHES "(arm64|aarch64)")
        set(CEF_PLATFORM "macosarm64")
        set(CEF_URL "https://cef-builds.spotifycdn.com/cef_binary_131.4.1%2Bg437feba%2Bchromium-131.0.6778.265_macosarm64_minimal.tar.bz2")
    else()
        set(CEF_PLATFORM "macosx64")
        set(CEF_URL "https://cef-builds.spotifycdn.com/cef_binary_131.4.1%2Bg437feba%2Bchromium-131.0.6778.265_macosx64_minimal.tar.bz2")
    endif()

    message(STATUS "CEF Platform: ${CEF_PLATFORM}")
    message(STATUS "CEF URL: ${CEF_URL}")

    ExternalProject_Add(cef
        URL ${CEF_URL}
        DOWNLOAD_DIR ${CASPARCG_DOWNLOAD_CACHE}
        CMAKE_ARGS
            -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
            -DUSE_SANDBOX=OFF
            -DCEF_RUNTIME_LIBRARY_FLAG=
            -DPROJECT_ARCH=${CMAKE_SYSTEM_PROCESSOR}
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS
            "<SOURCE_DIR>/Release/Chromium Embedded Framework.framework/Chromium Embedded Framework"
            "<BINARY_DIR>/libcef_dll_wrapper/libcef_dll_wrapper.a"
    )
    ExternalProject_Get_Property(cef SOURCE_DIR)
    ExternalProject_Get_Property(cef BINARY_DIR)

    add_library(CEF::CEF INTERFACE IMPORTED)
    add_dependencies(CEF::CEF cef)
    target_include_directories(CEF::CEF INTERFACE
        "${SOURCE_DIR}"
    )

    set(CEF_FRAMEWORK_PATH "${SOURCE_DIR}/Release/Chromium Embedded Framework.framework" CACHE PATH "CEF Framework path")
    set(CEF_RESOURCE_PATH "${CEF_FRAMEWORK_PATH}/Resources" CACHE PATH "CEF Resources path")

    target_link_libraries(CEF::CEF INTERFACE
        "${CEF_FRAMEWORK_PATH}/Chromium Embedded Framework"
        "${BINARY_DIR}/libcef_dll_wrapper/libcef_dll_wrapper.a"
    )

    target_link_options(CEF::CEF INTERFACE
        "-F${SOURCE_DIR}/Release"
    )

    install(DIRECTORY "${CEF_FRAMEWORK_PATH}" DESTINATION Frameworks
        USE_SOURCE_PERMISSIONS)

    set(CMAKE_INSTALL_RPATH "@executable_path/../Frameworks" CACHE STRING "Install RPATH" FORCE)
    set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE CACHE BOOL "Build with install RPATH" FORCE)

    message(STATUS "CEF enabled for macOS (${CEF_PLATFORM})")
endif()

# ============================================================================
# Global Properties
# ============================================================================
SET_PROPERTY (GLOBAL PROPERTY USE_FOLDERS ON)

# ============================================================================
# Preprocessor Definitions
# ============================================================================
ADD_DEFINITIONS (-DUNICODE)
ADD_DEFINITIONS (-D_UNICODE)
ADD_DEFINITIONS (-D__NO_INLINE__)
ADD_DEFINITIONS (-DBOOST_NO_SWPRINTF)
ADD_DEFINITIONS (-DTBB_USE_CAPTURED_EXCEPTION=1)
ADD_DEFINITIONS (-DNDEBUG)
ADD_DEFINITIONS (-DBOOST_LOCALE_HIDE_AUTO_PTR)
ADD_DEFINITIONS (-DBOOST_STACKTRACE_GNU_SOURCE_NOT_REQUIRED)

if (NOT USE_STATIC_BOOST)
    ADD_DEFINITIONS (-DBOOST_ALL_DYN_LINK)
endif()

# ============================================================================
# Compiler Flags
# ============================================================================
IF (NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
    ADD_COMPILE_OPTIONS (-O3)
endif()

# Architecture-specific optimizations
IF (CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|amd64)")
    ADD_COMPILE_OPTIONS (-msse3)
    ADD_COMPILE_OPTIONS (-mssse3)
    ADD_COMPILE_OPTIONS (-msse4.1)
    IF (ENABLE_AVX2)
        ADD_COMPILE_OPTIONS (-mavx)
        ADD_COMPILE_OPTIONS (-mavx2)
    ENDIF ()
ELSEIF (CMAKE_SYSTEM_PROCESSOR MATCHES "(arm64|aarch64)")
    message(STATUS "Building for Apple Silicon (arm64)")
ENDIF ()

# SIMD support
ADD_COMPILE_DEFINITIONS (USE_SIMDE)
ADD_COMPILE_DEFINITIONS (SIMDE_ENABLE_OPENMP)
ADD_COMPILE_OPTIONS (-fopenmp-simd)
ADD_COMPILE_OPTIONS (-fnon-call-exceptions)

# Warning flags
ADD_COMPILE_OPTIONS (-Wno-deprecated-declarations -Wno-multichar -Werror)
ADD_COMPILE_OPTIONS (-Wno-nonnull -Wno-nullability-completeness)

# Clang-specific settings (Apple Clang)
IF (CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID MATCHES "AppleClang")
    ADD_COMPILE_OPTIONS (-fvisibility=hidden)
    ADD_COMPILE_OPTIONS (-fvisibility-inlines-hidden)

    string(REPLACE "." "0" TBB_USE_GLIBCXX_VERSION ${CMAKE_CXX_COMPILER_VERSION})
    message(STATUS "ADDING: -DTBB_USE_GLIBCXX_VERSION=${TBB_USE_GLIBCXX_VERSION}")
    add_definitions(-DTBB_USE_GLIBCXX_VERSION=${TBB_USE_GLIBCXX_VERSION})
ENDIF ()

message(STATUS "=== End macOS Build Configuration ===")
