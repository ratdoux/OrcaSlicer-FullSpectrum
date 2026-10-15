option(ORCA_ENABLE_VULKAN_COMPUTE "Build the opt-in Vulkan compute experiments" OFF)
add_library(orca_vulkan_compute STATIC SeamVisibility.cpp SeamVisibility.hpp LocalZClipDecision.cpp LocalZClipDecision.hpp
    VulkanBatch.cpp VulkanBatch.hpp SeamNeighbors.cpp SeamNeighbors.hpp)
target_compile_features(orca_vulkan_compute PRIVATE cxx_std_17)
set_target_properties(orca_vulkan_compute PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(ORCA_ENABLE_VULKAN_COMPUTE)
    find_path(ORCA_VULKAN_INCLUDE_DIR vulkan/vulkan_core.h HINTS "$ENV{VULKAN_SDK}/include")
    find_program(ORCA_GLSLANG_VALIDATOR NAMES glslangValidator glslang HINTS "$ENV{VULKAN_SDK}/bin" "$ENV{VULKAN_SDK}/Bin")
    if(NOT ORCA_VULKAN_INCLUDE_DIR OR NOT ORCA_GLSLANG_VALIDATOR)
        message(FATAL_ERROR "Vulkan compute requires Vulkan headers and glslangValidator/glslang. Set VULKAN_SDK or ORCA_VULKAN_INCLUDE_DIR and ORCA_GLSLANG_VALIDATOR.")
    endif()
    set(_spirv "${CMAKE_CURRENT_BINARY_DIR}/seam_visibility.spv")
    set(_header "${CMAKE_CURRENT_BINARY_DIR}/seam_visibility_spirv.hpp")
    add_custom_command(OUTPUT "${_header}"
        COMMAND "${ORCA_GLSLANG_VALIDATOR}" -V --target-env vulkan1.0 -o "${_spirv}" "${CMAKE_CURRENT_SOURCE_DIR}/seam_visibility.comp"
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=${_spirv}" "-DOUTPUT=${_header}" -P "${CMAKE_CURRENT_LIST_DIR}/../../../cmake/EmbedSpirv.cmake"
        DEPENDS seam_visibility.comp "${CMAKE_CURRENT_LIST_DIR}/../../../cmake/EmbedSpirv.cmake"
        VERBATIM)
    target_sources(orca_vulkan_compute PRIVATE "${_header}")
    set(_clip_spirv "${CMAKE_CURRENT_BINARY_DIR}/local_z_clip_decision.spv")
    set(_clip_header "${CMAKE_CURRENT_BINARY_DIR}/local_z_clip_decision_spirv.hpp")
    add_custom_command(OUTPUT "${_clip_header}"
        COMMAND "${ORCA_GLSLANG_VALIDATOR}" -V --target-env vulkan1.0 -o "${_clip_spirv}" "${CMAKE_CURRENT_SOURCE_DIR}/local_z_clip_decision.comp"
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=${_clip_spirv}" "-DOUTPUT=${_clip_header}" -DSYMBOL=LOCAL_Z_CLIP_DECISION_SPIRV -P "${CMAKE_CURRENT_LIST_DIR}/../../../cmake/EmbedSpirv.cmake"
        DEPENDS local_z_clip_decision.comp "${CMAKE_CURRENT_LIST_DIR}/../../../cmake/EmbedSpirv.cmake"
        VERBATIM)
    target_sources(orca_vulkan_compute PRIVATE "${_clip_header}")
    set(_neighbors_header "${CMAKE_CURRENT_BINARY_DIR}/seam_neighbors_spirv.hpp")
    add_custom_command(OUTPUT "${_neighbors_header}"
        COMMAND "${ORCA_GLSLANG_VALIDATOR}" -V --target-env vulkan1.0 -o "${CMAKE_CURRENT_BINARY_DIR}/seam_neighbors.spv" "${CMAKE_CURRENT_SOURCE_DIR}/seam_neighbors.comp"
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=${CMAKE_CURRENT_BINARY_DIR}/seam_neighbors.spv" "-DOUTPUT=${_neighbors_header}" -DSYMBOL=SEAM_NEIGHBORS_SPIRV -P "${CMAKE_CURRENT_LIST_DIR}/../../../cmake/EmbedSpirv.cmake"
        DEPENDS seam_neighbors.comp "${CMAKE_CURRENT_LIST_DIR}/../../../cmake/EmbedSpirv.cmake" VERBATIM)
    target_sources(orca_vulkan_compute PRIVATE "${_neighbors_header}")
    target_include_directories(orca_vulkan_compute PRIVATE "${ORCA_VULKAN_INCLUDE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
    target_compile_definitions(orca_vulkan_compute PRIVATE ORCA_ENABLE_VULKAN_COMPUTE)
    target_link_libraries(orca_vulkan_compute PRIVATE ${CMAKE_DL_LIBS})
endif()
