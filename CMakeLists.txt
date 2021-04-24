cmake_minimum_required(VERSION 3.18)

project(unparanoid C)

# ---- include external deps ----
include(CTest)

find_package(PkgConfig REQUIRED)
pkg_check_modules(UV   REQUIRED IMPORTED_TARGET libuv)
pkg_check_modules(YAML REQUIRED IMPORTED_TARGET yaml-0.1)

add_subdirectory(thirdparty)


# ---- core library ----
add_library(libupd)
set_target_properties(libupd
  PROPERTIES
    PREFIX ""
)
target_compile_options(libupd
  PUBLIC
    -Wall -Wextra -Werror -pedantic -Wno-missing-field-initializers
)
target_include_directories(libupd SYSTEM
  PUBLIC
    include
)
target_include_directories(libupd
  PUBLIC
    src
)
target_sources(libupd
  PUBLIC
    include/libupd.h
  PRIVATE
    src/array.h
    src/iso.c
    src/memory.h
)
target_link_libraries(libupd
  PUBLIC
    hedley
    PkgConfig::UV
    PkgConfig::YAML
    utf8.h
)


# ---- main app ----
add_executable(upd)
target_link_libraries(upd
  PRIVATE
    libupd
)
target_sources(upd
  PRIVATE
    src/main.c
)


# ---- test app ----
if(BUILD_TESTING)
  add_executable(updtest)
  target_link_libraries(updtest
    PRIVATE
      libupd
  )
  target_sources(updtest
    PRIVATE
      src/test.c
  )

  add_test(
    NAME    updtest
    COMMAND $<TARGET_FILE:updtest>
  )
endif()
