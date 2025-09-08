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

set(ENABLE_HTML OFF CACHE BOOL "Enable CEF and HTML producer")
set(USE_STATIC_BOOST ON CACHE BOOL "Use shared library version of Boost")
set(CASPARCG_BINARY_NAME "casparcg" CACHE STRING "Custom name of the binary to build (this disables some install files)")
set(ENABLE_AVX2 OFF CACHE BOOL "Enable the AVX2 instruction set (requires a CPU that supports it)")

# Determine build (target) platform
SET (PLATFORM_FOLDER_NAME "Mac")

IF (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	MESSAGE (STATUS "Setting build type to 'Release' as none was specified.")
	SET (CMAKE_BUILD_TYPE "Release" CACHE STRING "Choose the type of build." FORCE)
	SET_PROPERTY (CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
ENDIF ()
MARK_AS_ADVANCED (CMAKE_INSTALL_PREFIX)

if (USE_STATIC_BOOST)
	SET (Boost_USE_STATIC_LIBS ON)
endif()
find_package(Boost 1.74.0 REQUIRED)
find_package(FFmpeg REQUIRED)
find_package(GLEW REQUIRED)
find_package(TBB REQUIRED)
find_package(OpenAL REQUIRED)
find_package(SFML 2 COMPONENTS graphics window REQUIRED PATHS /opt/homebrew/opt/sfml@2)

# FetchContent_Declare(
#     fetch_vk_bootstrap
#     GIT_REPOSITORY https://github.com/charles-lunarg/vk-bootstrap
#     GIT_TAG        vv1.4.315 #suggest using a tag so the library doesn't update whenever new commits are pushed to a branch
#     )
# FetchContent_MakeAvailable(fetch_vk_bootstrap)
find_package(Vulkan REQUIRED)

FetchContent_Declare(
    fetch_vma
    GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator
    GIT_TAG        v3.3.0 #suggest using a tag so the library doesn't update whenever new commits are pushed to a branch
)
FetchContent_MakeAvailable(fetch_vma)


# support for Ubuntu 22.04
# if (NOT TARGET OpenAL::OpenAL)
    # add_library(OpenAL::OpenAL INTERFACE IMPORTED)
    # target_include_directories(OpenAL::OpenAL INTERFACE ${OPENAL_INCLUDE_DIR})
    # target_link_libraries(OpenAL::OpenAL INTERFACE ${OPENAL_LIBRARY})
# endif()

if (ENABLE_HTML)
    casparcg_add_external_project(cef)
    ExternalProject_Add(cef
        URL ${CASPARCG_DOWNLOAD_MIRROR}/cef/cef_binary_131.4.1%2Bg437feba%2Bchromium-131.0.6778.265_linux64_minimal.tar.bz2
        URL_HASH SHA1=cbe52ac3c39ef93fdc5021588e12c466e801d9af
        DOWNLOAD_DIR ${CASPARCG_DOWNLOAD_CACHE}
        CMAKE_ARGS -DUSE_SANDBOX=Off
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS
            "<SOURCE_DIR>/Release/libcef.so"
            "<BINARY_DIR>/libcef_dll_wrapper/libcef_dll_wrapper.a"
    )
    ExternalProject_Get_Property(cef SOURCE_DIR)
    ExternalProject_Get_Property(cef BINARY_DIR)

    add_library(CEF::CEF INTERFACE IMPORTED)
    target_include_directories(CEF::CEF INTERFACE
        "${SOURCE_DIR}"
    )
    target_link_libraries(CEF::CEF INTERFACE
        # Note: All of these must be referenced in the BUILD_BYPRODUCTS above, to satisfy ninja
        "${SOURCE_DIR}/Release/libcef.so"
        "${BINARY_DIR}/libcef_dll_wrapper/libcef_dll_wrapper.a"
    )

    install(DIRECTORY ${SOURCE_DIR}/Resources/locales TYPE LIB)
    install(FILES ${SOURCE_DIR}/Resources/chrome_100_percent.pak TYPE LIB)
    install(FILES ${SOURCE_DIR}/Resources/chrome_200_percent.pak TYPE LIB)
    install(FILES ${SOURCE_DIR}/Resources/icudtl.dat TYPE LIB)
    install(FILES ${SOURCE_DIR}/Resources/resources.pak TYPE LIB)

    install(FILES ${SOURCE_DIR}/Release/chrome-sandbox TYPE LIB)
    install(FILES ${SOURCE_DIR}/Release/libcef.so TYPE LIB)
    install(FILES ${SOURCE_DIR}/Release/libEGL.so TYPE LIB)
    install(FILES ${SOURCE_DIR}/Release/libGLESv2.so TYPE LIB)
    install(FILES ${SOURCE_DIR}/Release/libvk_swiftshader.so TYPE LIB)
    install(FILES ${SOURCE_DIR}/Release/libvulkan.so.1 TYPE LIB)
    install(FILES ${SOURCE_DIR}/Release/snapshot_blob.bin TYPE LIB)
    install(FILES ${SOURCE_DIR}/Release/v8_context_snapshot.bin TYPE LIB)
    install(FILES ${SOURCE_DIR}/Release/vk_swiftshader_icd.json TYPE LIB)
endif ()

SET (BOOST_INCLUDE_PATH "${Boost_INCLUDE_DIRS}")
SET (FFMPEG_INCLUDE_PATH "${FFMPEG_INCLUDE_DIRS}")
SET (SFML_INCLUDE_PATH "${SFML_INCLUDE_DIRS}")

LINK_DIRECTORIES("${FFMPEG_LIBRARY_DIRS}")

SET_PROPERTY (GLOBAL PROPERTY USE_FOLDERS ON)

ADD_DEFINITIONS (-DSFML_STATIC)
ADD_DEFINITIONS (-DUNICODE)
ADD_DEFINITIONS (-D_UNICODE)
ADD_DEFINITIONS (-D__NO_INLINE__) # Needed for precompiled headers to work
ADD_DEFINITIONS (-DBOOST_NO_SWPRINTF) # swprintf on Linux seems to always use , as decimal point regardless of C-locale or C++-locale
ADD_DEFINITIONS (-DTBB_USE_CAPTURED_EXCEPTION=1)
ADD_DEFINITIONS (-DNDEBUG) # Needed for precompiled headers to work
ADD_DEFINITIONS (-DBOOST_LOCALE_HIDE_AUTO_PTR) # Needed for C++17 in boost 1.67+
ADD_DEFINITIONS (-DNO_OGL) 


if (NOT USE_STATIC_BOOST)
	ADD_DEFINITIONS (-DBOOST_ALL_DYN_LINK)
endif()

IF (NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
	ADD_COMPILE_OPTIONS (-O3) # Needed for precompiled headers to work
endif()

ADD_COMPILE_DEFINITIONS (_GNU_SOURCE)
#ADD_COMPILE_DEFINITIONS (USE_SIMDE) 
# ADD_COMPILE_DEFINITIONS (SIMDE_ENABLE_OPENMP) # Enable OpenMP support in simde
# ADD_COMPILE_OPTIONS (-fopenmp-simd) # Enable OpenMP SIMD support
ADD_COMPILE_OPTIONS (-fnon-call-exceptions) # Allow signal handler to throw exception

ADD_COMPILE_OPTIONS (-Wno-deprecated-declarations -Wno-write-strings -Wno-multichar -Wno-cpp -Werror)
IF (CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    ADD_COMPILE_OPTIONS (-Wno-terminate)
ELSEIF (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # Help TBB figure out what compiler support for c++11 features
    # https://github.com/01org/tbb/issues/22
    string(REPLACE "." "0" TBB_USE_GLIBCXX_VERSION ${CMAKE_CXX_COMPILER_VERSION})
    message(STATUS "ADDING: -DTBB_USE_GLIBCXX_VERSION=${TBB_USE_GLIBCXX_VERSION}")
    add_definitions(-DTBB_USE_GLIBCXX_VERSION=${TBB_USE_GLIBCXX_VERSION})
ENDIF ()
