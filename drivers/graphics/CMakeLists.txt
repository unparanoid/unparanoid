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
    gl3_glsl.c
    gl3_pl.h
    gl3_pl_def.c
    gl3_pl_lk.c
    gl3_tex.c
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
