cmake_minimum_required(VERSION 3.20)

add_custom_target(pack ALL)


function (add_driver name)
  cmake_parse_arguments(AD "" "" "FILES")
  set(suffix ".x86_64${CMAKE_SHARED_LIBRARY_SUFFIX}")

  add_library(${name} SHARED)
  set_target_properties(${name}
    PROPERTIES
      PREFIX ""
      SUFFIX "${suffix}"
  )
  target_link_libraries(${name}
    PRIVATE libupd
  )
  target_compile_options(${name}
    PRIVATE ${UPD_C_FLAGS}
  )

  set(dir_src "${CMAKE_CURRENT_SOURCE_DIR}")
  set(dir_dst "${CMAKE_CURRENT_BINARY_DIR}/dst")

  file(MAKE_DIRECTORY "${dir_dst}")
  list(APPEND AD_FILES "upd.yml")

  set(src "${CMAKE_CURRENT_BINARY_DIR}/${name}${suffix}")
  set(dst "${dir_dst}/${name}${suffix}")
  add_custom_command(
    OUTPUT  "${dst}"
    DEPENDS ${name}

    COMMAND ${CMAKE_COMMAND} -E copy "${src}" "${dst}"

    COMMENT "${name}: copying ${name}${suffix}..."
    VERBATIM
  )
  set(deps "${dst}")

  foreach (f ${AD_FILES})
    set(src "${dir_src}/${f}")
    set(dst "${dir_dst}/${f}")

    get_filename_component(p "${dst}" DIRECTORY)
    file(MAKE_DIRECTORY "${p}")

    add_custom_command(
      OUTPUT  "${dst}"
      DEPENDS "${src}"

      COMMAND ${CMAKE_COMMAND} -E copy "${src}" "${dst}"

      COMMENT "${name}: copying ${f}..."
      VERBATIM
    )
    list(APPEND deps "${dst}")
  endforeach()

  add_custom_target(pack-${name} DEPENDS ${deps})
  add_dependencies(pack pack-${name})
endfunction()


add_subdirectory(graphics)
add_subdirectory(http)
add_subdirectory(luajit)
add_subdirectory(msgpack)
add_subdirectory(png)
add_subdirectory(yaml)
