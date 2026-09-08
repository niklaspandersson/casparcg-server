cmake_minimum_required (VERSION 3.28)
project (screen)

if (CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    # macOS: Use Vulkan screen consumer with local vk_util library
    set(SOURCES
        consumer/screen_consumer_vk.mm
        consumer/screen_consumer_vk.h

        consumer/vk_util/device.cpp
        consumer/vk_util/device.h
        consumer/vk_util/swapchain.cpp
        consumer/vk_util/swapchain.h
        consumer/vk_util/render_pipeline.cpp
        consumer/vk_util/render_pipeline.h
        consumer/vk_util/texture.cpp
        consumer/vk_util/texture.h
        consumer/vk_util/buffer.cpp
        consumer/vk_util/buffer.h
        consumer/vk_util/vk_check.h

        screen.cpp
        screen.h
    )

    # Compile screen shaders to SPIR-V
    find_program(glslc_executable NAMES glslc HINTS Vulkan::glslc)
    if (glslc_executable)
        set(SCREEN_SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/consumer/shaders)
        set(SCREEN_SPV_DIR ${CMAKE_BINARY_DIR}/staging/shaders)
        file(MAKE_DIRECTORY ${SCREEN_SPV_DIR})

        add_custom_command(
            COMMAND ${glslc_executable} -o ${SCREEN_SPV_DIR}/screen_vert.spv ${SCREEN_SHADER_DIR}/screen.vert
            OUTPUT ${SCREEN_SPV_DIR}/screen_vert.spv
            DEPENDS ${SCREEN_SHADER_DIR}/screen.vert
            COMMENT "Compiling screen vertex shader"
        )
        add_custom_command(
            COMMAND ${glslc_executable} -o ${SCREEN_SPV_DIR}/screen_frag.spv ${SCREEN_SHADER_DIR}/screen.frag
            OUTPUT ${SCREEN_SPV_DIR}/screen_frag.spv
            DEPENDS ${SCREEN_SHADER_DIR}/screen.frag
            COMMENT "Compiling screen fragment shader"
        )
        add_custom_target(screen_shaders ALL DEPENDS
            ${SCREEN_SPV_DIR}/screen_vert.spv
            ${SCREEN_SPV_DIR}/screen_frag.spv
        )

        # Copy compiled shaders next to the executable for runtime loading
        add_custom_command(TARGET screen_shaders POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/shell/shaders
            COMMAND ${CMAKE_COMMAND} -E copy
                ${SCREEN_SPV_DIR}/screen_vert.spv
                ${SCREEN_SPV_DIR}/screen_frag.spv
                ${CMAKE_BINARY_DIR}/shell/shaders/
            COMMENT "Copying compiled screen shaders to output directory"
        )
    else()
        message(WARNING "glslc not found - screen shaders will not be compiled. Install Vulkan SDK.")
    endif()

    casparcg_add_module_project(screen
        SOURCES ${SOURCES}
        INIT_FUNCTION "screen::init"
    )
    target_include_directories(screen PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/consumer
        ${FFMPEG_INCLUDE_PATH}
    )
    target_link_libraries(screen PRIVATE accelerator ffmpeg glfw)
    target_link_libraries(screen PRIVATE Vulkan::Vulkan)
    target_link_libraries(screen PRIVATE Apple::Frameworks)
else()
    # Windows/Linux: Use OpenGL screen consumer
    set(SOURCES
        consumer/screen_consumer.cpp
        consumer/screen_consumer.h

        consumer_screen_vertex.h
        consumer_screen_fragment.h

        screen.cpp
        screen.h
    )

    bin2c("consumer/screen.vert" "consumer_screen_vertex.h" "caspar::screen" "vertex_shader")
    bin2c("consumer/screen.frag" "consumer_screen_fragment.h" "caspar::screen" "fragment_shader")

    if (MSVC)
    else ()
        list(APPEND SOURCES
            util/x11_util.cpp
            util/x11_util.h
        )
    endif ()

    casparcg_add_module_project(screen
        SOURCES ${SOURCES}
        INIT_FUNCTION "screen::init"
    )
    target_include_directories(screen PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}
        ${FFMPEG_INCLUDE_PATH}
    )
    target_link_libraries(screen PRIVATE accelerator ffmpeg GLEW::glew sfml-system sfml-window)
endif()

set_target_properties(screen PROPERTIES FOLDER modules)
source_group(sources\\consumer consumer/*)
source_group(sources ./*)
