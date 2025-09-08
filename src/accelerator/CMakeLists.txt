cmake_minimum_required (VERSION 3.16)
project (accelerator)

set(SOURCES
	vulkan/image/image_kernel.cpp
	vulkan/image/image_mixer.cpp
	vulkan/image/image_shader.cpp

	vulkan/util/buffer.cpp
	vulkan/util/device.cpp
	vulkan/util/shader.cpp
	vulkan/util/texture.cpp
	vulkan/util/matrix.cpp
	vulkan/util/transforms.cpp

	accelerator.cpp
)
set(HEADERS
	vulkan/image/image_kernel.h
	vulkan/image/image_mixer.h
	vulkan/image/image_shader.h

	vulkan/util/buffer.h
	vulkan/util/context.h
	vulkan/util/device.h
	vulkan/util/shader.h
	vulkan/util/texture.h
	vulkan/util/matrix.h
	vulkan/util/transforms.h

	vulkan_image_vertex.h
	vulkan_image_fragment.h

	accelerator.h
	StdAfx.h
)

if (MSVC)
	list(APPEND SOURCES
		vulkan/util/context_sfml.cpp
	)
elseif (APPLE)
	list(APPEND SOURCES
		vulkan/util/context_sfml.cpp
	)
else()
	list(APPEND SOURCES
		vulkan/util/context_egl.cpp
	)
endif()

bin2c("vulkan/image/shader.vert" "vulkan_image_vertex.h" "caspar::accelerator::vulkan" "vertex_shader")
bin2c("vulkan/image/shader.frag" "vulkan_image_fragment.h" "caspar::accelerator::vulkan" "fragment_shader")

casparcg_add_library(accelerator SOURCES ${SOURCES} ${HEADERS})
target_include_directories(accelerator PRIVATE .. ${CMAKE_CURRENT_BINARY_DIR} ${SFML_INCLUDE_PATH})
target_precompile_headers(accelerator PRIVATE StdAfx.h)
target_link_libraries(accelerator PRIVATE common core Vulkan::Vulkan)

if (MSVC)
target_link_libraries(accelerator PRIVATE sfml-window)
elseif (APPLE)
target_link_libraries(accelerator PRIVATE sfml-window)
endif()

source_group(sources ./.*)
source_group(sources\\cpu\\image cpu/image/.*)
source_group(sources\\cpu\\util cpu/util/.*)
source_group(sources\\vulkan\\image vulkan/image/.*)
source_group(sources\\vulkan\\util vulkan/util/.*)
