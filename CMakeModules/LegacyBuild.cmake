find_package(Git)

set(GIT_REV "0")
set(GIT_HASH "N/A")

if (GIT_FOUND)
	exec_program("${GIT_EXECUTABLE}" "${PROJECT_SOURCE_DIR}"
			ARGS rev-list --all --count
			OUTPUT_VARIABLE GIT_REV)
	exec_program("${GIT_EXECUTABLE}" "${PROJECT_SOURCE_DIR}"
			ARGS rev-parse --verify --short HEAD
			OUTPUT_VARIABLE GIT_HASH)
endif ()

configure_file("${PROJECT_SOURCE_DIR}/version.tmpl" "${PROJECT_SOURCE_DIR}/version.h")

set(CASPARCG_MODULE_INCLUDE_STATEMENTS "" CACHE INTERNAL "")
set(CASPARCG_MODULE_INIT_STATEMENTS "" CACHE INTERNAL "")
set(CASPARCG_MODULE_UNINIT_STATEMENTS "" CACHE INTERNAL "")
set(CASPARCG_MODULE_COMMAND_LINE_ARG_INTERCEPTORS_STATEMENTS "" CACHE INTERNAL "")
set(CASPARCG_MODULE_PROJECTS "" CACHE INTERNAL "")
set(CASPARCG_RUNTIME_DEPENDENCIES "" CACHE INTERNAL "")

function(casparcg_add_include_statement HEADER_FILE_TO_INCLUDE)
	set(CASPARCG_MODULE_INCLUDE_STATEMENTS "${CASPARCG_MODULE_INCLUDE_STATEMENTS}"
			"#include <${HEADER_FILE_TO_INCLUDE}>"
			CACHE INTERNAL "")
endfunction()

function(casparcg_add_init_statement INIT_FUNCTION_NAME NAME_TO_LOG)
	set(CASPARCG_MODULE_INIT_STATEMENTS "${CASPARCG_MODULE_INIT_STATEMENTS}"
			"	${INIT_FUNCTION_NAME}(dependencies)\;"
			"	CASPAR_LOG(info) << L\"Initialized ${NAME_TO_LOG} module.\"\;"
			""
			CACHE INTERNAL "")
endfunction()

function(casparcg_add_uninit_statement UNINIT_FUNCTION_NAME)
	set(CASPARCG_MODULE_UNINIT_STATEMENTS
			"	${UNINIT_FUNCTION_NAME}()\;"
			"${CASPARCG_MODULE_UNINIT_STATEMENTS}"
			CACHE INTERNAL "")
endfunction()

function(casparcg_add_command_line_arg_interceptor INTERCEPTOR_FUNCTION_NAME)
	set(CASPARCG_MODULE_COMMAND_LINE_ARG_INTERCEPTORS_STATEMENTS "${CASPARCG_MODULE_COMMAND_LINE_ARG_INTERCEPTORS_STATEMENTS}"
			"	if (${INTERCEPTOR_FUNCTION_NAME}(argc, argv))"
			"		return true\;"
			""
			CACHE INTERNAL "")
endfunction()

function(casparcg_add_module_project PROJECT)
	set(CASPARCG_MODULE_PROJECTS "${CASPARCG_MODULE_PROJECTS}" "${PROJECT}" CACHE INTERNAL "")
endfunction()

# http://stackoverflow.com/questions/7172670/best-shortest-way-to-join-a-list-in-cmake
function(join_list VALUES GLUE OUTPUT)
	string (REGEX REPLACE "([^\\]|^);" "\\1${GLUE}" _TMP_STR "${VALUES}")
	string (REGEX REPLACE "[\\](.)" "\\1" _TMP_STR "${_TMP_STR}") #fixes escaping
	set (${OUTPUT} "${_TMP_STR}" PARENT_SCOPE)
endfunction()

function(casparcg_add_runtime_dependency FILE_TO_COPY)
	set(CASPARCG_RUNTIME_DEPENDENCIES "${CASPARCG_RUNTIME_DEPENDENCIES}" "${FILE_TO_COPY}" CACHE INTERNAL "")
endfunction()

set(PACKAGES_FOLDER "${PROJECT_SOURCE_DIR}/packages")

# BOOST
set(BOOST_INCLUDE_PATH "${PACKAGES_FOLDER}/boost.1.66.0.0/lib/native/include")
link_directories("${PACKAGES_FOLDER}/boost_atomic-vc141.1.66.0.0/lib/native")
link_directories("${PACKAGES_FOLDER}/boost_chrono-vc141.1.66.0.0/lib/native")
link_directories("${PACKAGES_FOLDER}/boost_context-vc141.1.66.0.0/lib/native")
link_directories("${PACKAGES_FOLDER}/boost_coroutine-vc141.1.66.0.0/lib/native")
link_directories("${PACKAGES_FOLDER}/boost_date_time-vc141.1.66.0.0/lib/native")
link_directories("${PACKAGES_FOLDER}/boost_filesystem-vc141.1.66.0.0/lib/native")
link_directories("${PACKAGES_FOLDER}/boost_locale-vc141.1.66.0.0/lib/native")
link_directories("${PACKAGES_FOLDER}/boost_log-vc141.1.66.0.0/lib/native")
link_directories("${PACKAGES_FOLDER}/boost_regex-vc141.1.66.0.0/lib/native")
link_directories("${PACKAGES_FOLDER}/boost_system-vc141.1.66.0.0/lib/native")
link_directories("${PACKAGES_FOLDER}/boost_thread-vc141.1.66.0.0/lib/native")
add_definitions( -DBOOST_CONFIG_SUPPRESS_OUTDATED_MESSAGE )
add_definitions( -DBOOST_COROUTINES_NO_DEPRECATION_WARNING )

# FFMPEG
set(FFMPEG_INCLUDE_PATH "${PACKAGES_FOLDER}/FFmpeg.Nightly.20180130.1.0/build/native/include")
set(FFMPEG_BIN_PATH "${PACKAGES_FOLDER}/FFmpeg.Nightly.20180130.1.0/build/native/bin/x64")
link_directories("${PACKAGES_FOLDER}/FFmpeg.Nightly.20180130.1.0/build/native/lib/x64")
casparcg_add_runtime_dependency("${FFMPEG_BIN_PATH}/avcodec-58.dll")
casparcg_add_runtime_dependency("${FFMPEG_BIN_PATH}/avdevice-58.dll")
casparcg_add_runtime_dependency("${FFMPEG_BIN_PATH}/avfilter-7.dll")
casparcg_add_runtime_dependency("${FFMPEG_BIN_PATH}/avformat-58.dll")
casparcg_add_runtime_dependency("${FFMPEG_BIN_PATH}/avutil-56.dll")
casparcg_add_runtime_dependency("${FFMPEG_BIN_PATH}/postproc-55.dll")
casparcg_add_runtime_dependency("${FFMPEG_BIN_PATH}/swresample-3.dll")
casparcg_add_runtime_dependency("${FFMPEG_BIN_PATH}/swscale-5.dll")

# TBB
set(TBB_INCLUDE_PATH "${PACKAGES_FOLDER}/tbb/include")
set(TBB_BIN_PATH "${PACKAGES_FOLDER}/tbb/bin/win32")
link_directories("${PACKAGES_FOLDER}/tbb/lib/win32")
casparcg_add_runtime_dependency("${TBB_BIN_PATH}/tbb.dll")
casparcg_add_runtime_dependency("${TBB_BIN_PATH}/tbb_debug.dll")
casparcg_add_runtime_dependency("${TBB_BIN_PATH}/tbbmalloc.dll")
casparcg_add_runtime_dependency("${TBB_BIN_PATH}/tbbmalloc_debug.dll")
casparcg_add_runtime_dependency("${TBB_BIN_PATH}/tbbmalloc_proxy.dll")
casparcg_add_runtime_dependency("${TBB_BIN_PATH}/tbbmalloc_proxy_debug.dll")

# GLEW
set(GLEW_INCLUDE_PATH "${PACKAGES_FOLDER}/glew/include")
set(GLEW_BIN_PATH "${PACKAGES_FOLDER}/glew/bin/win32")
link_directories("${PACKAGES_FOLDER}/glew/lib/win32")
add_definitions( -DGLEW_NO_GLU )
casparcg_add_runtime_dependency("${GLEW_BIN_PATH}/glew32.dll")

# SFML
set(SFML_INCLUDE_PATH "${PACKAGES_FOLDER}/sfml-2.4.2/include")

link_directories("${PACKAGES_FOLDER}/sfml-graphics.2.4.2.0/build/native/lib/x64/v140/Debug/dynamic")
link_directories("${PACKAGES_FOLDER}/sfml-graphics.2.4.2.0/build/native/lib/x64/v140/Release/dynamic")
link_directories("${PACKAGES_FOLDER}/sfml-window.2.4.2.0/build/native/lib/x64/v140/Debug/dynamic")
link_directories("${PACKAGES_FOLDER}/sfml-window.2.4.2.0/build/native/lib/x64/v140/Release/dynamic")
link_directories("${PACKAGES_FOLDER}/sfml-system.2.4.2.0/build/native/lib/x64/v140/Debug/dynamic")
link_directories("${PACKAGES_FOLDER}/sfml-system.2.4.2.0/build/native/lib/x64/v140/Release/dynamic")
casparcg_add_runtime_dependency("${PACKAGES_FOLDER}/sfml-graphics.redist.2.4.2.0/build/native/bin/x64/v140/Debug/dynamic/sfml-graphics-d-2.dll")
casparcg_add_runtime_dependency("${PACKAGES_FOLDER}/sfml-graphics.redist.2.4.2.0/build/native/bin/x64/v140/Release/dynamic/sfml-graphics-2.dll")
casparcg_add_runtime_dependency("${PACKAGES_FOLDER}/sfml-window.redist.2.4.2.0/build/native/bin/x64/v140/Debug/dynamic/sfml-window-d-2.dll")
casparcg_add_runtime_dependency("${PACKAGES_FOLDER}/sfml-window.redist.2.4.2.0/build/native/bin/x64/v140/Release/dynamic/sfml-window-2.dll")
casparcg_add_runtime_dependency("${PACKAGES_FOLDER}/sfml-system.redist.2.4.2.0/build/native/bin/x64/v140/Debug/dynamic/sfml-system-d-2.dll")
casparcg_add_runtime_dependency("${PACKAGES_FOLDER}/sfml-system.redist.2.4.2.0/build/native/bin/x64/v140/Release/dynamic/sfml-system-2.dll")

# FREEIMAGE
set(FREEIMAGE_INCLUDE_PATH "${PACKAGES_FOLDER}/freeimage/include")
set(FREEIMAGE_BIN_PATH "${PACKAGES_FOLDER}/freeimage/bin/win32")
link_directories("${PACKAGES_FOLDER}/freeimage/lib/win32")
link_directories("${PACKAGES_FOLDER}/zlib/lib")
casparcg_add_runtime_dependency("${FREEIMAGE_BIN_PATH}/FreeImage.dll")
casparcg_add_runtime_dependency("${FREEIMAGE_BIN_PATH}/FreeImaged.dll")

# OPENAL
set(OPENAL_INCLUDE_PATH "${PACKAGES_FOLDER}/openal/include")
set(OPENAL_BIN_PATH "${PACKAGES_FOLDER}/openal/bin/win32")
link_directories("${PACKAGES_FOLDER}/openal/lib/win32")
casparcg_add_runtime_dependency("${OPENAL_BIN_PATH}/OpenAL32.dll")

# LIBERATION_FONTS
set(LIBERATION_FONTS_BIN_PATH "${PACKAGES_FOLDER}/liberation-fonts")

# CEF
set(CEF_INCLUDE_PATH "${PACKAGES_FOLDER}/cef/win32/include")
set(CEF_PATH "${PACKAGES_FOLDER}/cef/win32")
set(CEF_BIN_PATH "${PACKAGES_FOLDER}/cef/win32/Release")
set(CEF_RESOURCE_PATH "${PACKAGES_FOLDER}/cef/win32/Resources")
link_directories("${CEF_BIN_PATH}")

casparcg_add_runtime_dependency("${CEF_RESOURCE_PATH}/locales")
casparcg_add_runtime_dependency("${CEF_RESOURCE_PATH}/cef.pak")
casparcg_add_runtime_dependency("${CEF_RESOURCE_PATH}/cef_100_percent.pak")
casparcg_add_runtime_dependency("${CEF_RESOURCE_PATH}/cef_200_percent.pak")
casparcg_add_runtime_dependency("${CEF_RESOURCE_PATH}/cef_extensions.pak")
casparcg_add_runtime_dependency("${CEF_RESOURCE_PATH}/devtools_resources.pak")
casparcg_add_runtime_dependency("${CEF_RESOURCE_PATH}/icudtl.dat")
casparcg_add_runtime_dependency("${CEF_BIN_PATH}/natives_blob.bin")
casparcg_add_runtime_dependency("${CEF_BIN_PATH}/snapshot_blob.bin")
casparcg_add_runtime_dependency("${CEF_BIN_PATH}/libcef.dll")
casparcg_add_runtime_dependency("${CEF_BIN_PATH}/chrome_elf.dll")
casparcg_add_runtime_dependency("${CEF_BIN_PATH}/d3dcompiler_43.dll")
casparcg_add_runtime_dependency("${CEF_BIN_PATH}/d3dcompiler_47.dll")
casparcg_add_runtime_dependency("${CEF_BIN_PATH}/libEGL.dll")
casparcg_add_runtime_dependency("${CEF_BIN_PATH}/libGLESv2.dll")

set_property(GLOBAL PROPERTY USE_FOLDERS ON)

add_definitions(-DUNICODE)
add_definitions(-D_UNICODE)
add_definitions(-DCASPAR_SOURCE_PREFIX="${CMAKE_CURRENT_SOURCE_DIR}")
add_definitions(-D_WIN32_WINNT=0x601)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHa /Zi /W4 /WX /MP /fp:fast /Zm192 /FIcommon/compiler/vs/disable_silly_warnings.h")
set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}	/D TBB_USE_ASSERT=1 /D TBB_USE_DEBUG /bigobj")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}	/Oi /Ot /Gy /bigobj")

if (POLICY CMP0045)
	cmake_policy(SET CMP0045 OLD)
endif ()

include(CMakeModules/PrecompiledHeader.cmake)

add_subdirectory(accelerator)
add_subdirectory(common)
add_subdirectory(core)
add_subdirectory(modules)
add_subdirectory(protocol)
add_subdirectory(shell)