project(Chapter1)

add_executable(VulkanRenderer
    graphics/command_buffer.cpp
    graphics/command_buffer.hpp
    graphics/gpu_device.cpp
    graphics/gpu_device.hpp
    graphics/gpu_enum.hpp
    graphics/gpu_profiler.cpp
    graphics/gpu_profiler.hpp
    graphics/gpu_resources.cpp
    graphics/gpu_resources.hpp
    graphics/renderer.cpp
    graphics/renderer.hpp
    graphics/spirv_parser.cpp
    graphics/spirv_parser.hpp

    graphics/raptor_imgui.cpp
    graphics/raptor_imgui.hpp

    main.cpp
)

set_property(TARGET VulkanRenderer PROPERTY CXX_STANDARD 20)

if (WIN32)
    target_compile_definitions(VulkanRenderer PRIVATE
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
        NOMINMAX)
endif()

target_compile_definitions(VulkanRenderer PRIVATE
    TRACY_ENABLE
    TRACY_ON_DEMAND
    TRACY_NO_SYSTEM_TRACING
)

target_include_directories(VulkanRenderer PRIVATE
    .
    ..
    raptor
    ${Vulkan_INCLUDE_DIRS}
)

if (WIN32)
    target_include_directories(VulkanRenderer PRIVATE
        ../binaries/SDL2-2.0.18/include)

    target_link_directories(VulkanRenderer PRIVATE
        ../binaries/SDL2-2.0.18/lib/x64
    )
else()
    target_include_directories(VulkanRenderer PRIVATE
        ${SDL2_INCLUDE_DIRS})

endif()

target_link_libraries(VulkanRenderer PRIVATE
    RaptorFoundation
    RaptorExternal
    RaptorApp
    ${Vulkan_LIBRARIES}
)

if (WIN32)
    target_link_libraries(VulkanRenderer PRIVATE
        SDL2)
else()
    target_link_libraries(VulkanRenderer PRIVATE
        dl
        pthread
        SDL2::SDL2)
endif()

if (WIN32)
    set(DLLS_TO_COPY
        ${CMAKE_CURRENT_SOURCE_DIR}/../binaries/SDL2-2.0.18/lib/x64/SDL2.dll
    )

    foreach(DLL ${DLLS_TO_COPY})
        add_custom_command(TARGET VulkanRenderer POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy ${DLL} "${CMAKE_CURRENT_BINARY_DIR}"
            VERBATIM
        )
    endforeach()
endif()

file(GLOB GLSL_SHADERS
    shaders/*.vert
    shaders/*.frag)

add_custom_target(ShadersSpv
    SOURCES ${GLSL_SHADERS}
)

foreach (GLSL_SHADER ${GLSL_SHADERS})
    add_custom_command(TARGET ShadersSpv
        COMMAND $ENV{VULKAN_SDK}/bin/glslc ${GLSL_SHADER}
        DEPENDS ${GLSL_SHADER}
    )
endforeach()

add_dependencies(VulkanRenderer
    ShadersSpv
)
