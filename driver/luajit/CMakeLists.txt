find_program(XXD xxd REQUIRED)

set(XXD_PWD "${CMAKE_CURRENT_SOURCE_DIR}")
set(XXD_SRC "${CMAKE_CURRENT_SOURCE_DIR}/std.lua")
set(XXD_DST "${CMAKE_CURRENT_BINARY_DIR}/std.lua.h")

add_custom_command(
  OUTPUT  "${XXD_DST}"
  DEPENDS "${XXD_SRC}"
  COMMAND  ${XXD} -i std.lua "${XXD_DST}"

  WORKING_DIRECTORY "${XXD_PWD}"
  COMMENT           "converting std.lua into C header..."
  VERBATIM
)


add_driver(upd.luajit
  FILES
    lib/lambda.lua
    lib/trait.lua
)
target_sources(upd.luajit
  PRIVATE
  "${XXD_DST}"

    common.h
    ctx.c
    ctx_req.h
    dev.c
    main.c
    prog.c
    std.c
    std_lua.h
    std_mpk.h
    std_path.h
    stream.c
)
target_link_libraries(upd.luajit
  PRIVATE
    hedley
    luajit
    msgpackc
    utf8.h
)
target_include_directories(upd.luajit
  PRIVATE "${CMAKE_CURRENT_BINARY_DIR}"
)
