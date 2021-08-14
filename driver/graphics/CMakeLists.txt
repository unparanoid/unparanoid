find_program(XXD xxd)

include(FindOpenGL)
if (NOT OPENGL_FOUND)
  message(ERROR "OpenGL not found")
endif()


add_driver(upd.graphics)
target_sources(upd.graphics
  PRIVATE
    common.h
    gl3.h
    gl3_buf.c
    gl3_dev.c
    gl3_enum.h
    gl3_glsl.c
    gl3_pl.h
    gl3_pl_def.c
    gl3_pl_lk.c
    gl3_tex.c
    gl3_view.c
    glfw.h
    glfw_dev.c
    main.c
)
target_link_libraries(upd.graphics
  PRIVATE
    OpenGL::GL

    glew
    glfw
    hedley
    for-msvc  # affects only when using MSVC
)


set(shaders
  gl3_view.vsh
  gl3_view.fsh
)

set(dst_dir "${CMAKE_CURRENT_BINARY_DIR}/shader")
file(MAKE_DIRECTORY "${dst_dir}")

target_include_directories(upd.graphics PRIVATE "${dst_dir}")

foreach (f ${shaders})
  set(dst "${dst_dir}/${f}.h")

  add_custom_command(
    OUTPUT  "${dst}"
    DEPENDS "${f}"
    COMMAND  ${XXD} -i "${f}" "${dst}"

    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT           "converting ${f} to C header..."
    VERBATIM
  )
  target_sources(upd.graphics PRIVATE "${dst}")
endforeach()
