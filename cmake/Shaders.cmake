find_package(Vulkan REQUIRED COMPONENTS glslc)

# retropark_embed_shader(<glsl> <stage vert|frag> <out_header_abs> <symbol>)
function(retropark_embed_shader GLSL STAGE OUT_HEADER SYMBOL)
  set(SPV "${OUT_HEADER}.spv")
  add_custom_command(
    OUTPUT "${OUT_HEADER}"
    COMMAND Vulkan::glslc -fshader-stage=${STAGE} "${GLSL}" -o "${SPV}"
    COMMAND ${CMAKE_COMMAND}
            -DSPV=${SPV} -DHEADER=${OUT_HEADER} -DSYMBOL=${SYMBOL}
            -P "${CMAKE_SOURCE_DIR}/cmake/EmbedSpv.cmake"
    DEPENDS "${GLSL}" "${CMAKE_SOURCE_DIR}/cmake/EmbedSpv.cmake"
    VERBATIM)
endfunction()
