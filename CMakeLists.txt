cmake_minimum_required(VERSION 3.18)

project(unparanoid C)


# ---- include external deps ----
include(CTest)
include(TestBigEndian)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LUA  REQUIRED IMPORTED_TARGET luajit)
pkg_check_modules(UV   REQUIRED IMPORTED_TARGET libuv)
pkg_check_modules(YAML REQUIRED IMPORTED_TARGET yaml-0.1)

pkg_check_modules(VALGRIND valgrind)

add_subdirectory(thirdparty)


# ---- env check ----
TEST_BIG_ENDIAN(IS_BIG_ENDIAN)
if(IS_BIG_ENDIAN)
  message(ERROR "big endian is not supported")
endif()


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
    "${VALGRIND_INCLUDE_DIRS}"
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
    src/buf.h
    src/cli.c
    src/cli.h
    src/common.h
    src/config.c
    src/config.h
    src/driver.c
    src/driver.h
    src/file.c
    src/file.h
    src/iso.c
    src/memory.h
    src/path.h
    src/req.c
    src/req.h
    src/srv.c
    src/srv.h

    src/driver/dir.c
    src/driver/lua.c
    src/driver/syncdir.c

    src/driver/prog/http.c
    src/driver/prog/parallelism.c
)
target_link_libraries(libupd
  PUBLIC
    crypto-algorithms
    cwalk
    hedley
    picohttpparser
    PkgConfig::LUA
    PkgConfig::UV
    PkgConfig::YAML
    utf8.h
    wsock.h

    m
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
