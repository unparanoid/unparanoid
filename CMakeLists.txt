cmake_minimum_required(VERSION 3.20)

project(unparanoid C)


# ---- include external deps ----
include(CTest)
include(TestBigEndian)

find_package(PkgConfig)
if (PKG_CONFIG_FOUND)
  pkg_check_modules(VALGRIND valgrind)
endif()

add_subdirectory(thirdparty)


# ---- env check ----
TEST_BIG_ENDIAN(IS_BIG_ENDIAN)
if(IS_BIG_ENDIAN)
  message(ERROR "big endian is not supported")
endif()

if (CMAKE_C_COMPILER_ID STREQUAL "GNU")
  set(UPD_C_FLAGS -Wall -Wextra -Werror -pedantic -Wno-missing-field-initializers)
elseif (CMAKE_C_COMPILER_ID STREQUAL "MSVC")
  set(UPD_C_FLAGS /W2 /std:c11 /Zc:preprocessor-)
else()
  message(ERROR "unknown compiler")
endif()


# ---- core library ----
add_library(libupd)
set_target_properties(libupd
  PROPERTIES
    PREFIX     ""
    C_STANDARD 11
)
target_compile_options(libupd
  PUBLIC ${UPD_C_FLAGS}
)
target_compile_definitions(libupd
  PUBLIC
    UPD_USE_VALGRIND=$<BOOL:${VALGRIND_FOUND}>
)
target_include_directories(libupd SYSTEM
  PUBLIC
    "${VALGRIND_INCLUDE_DIRS}"
    .
)
target_include_directories(libupd
  PUBLIC
    src
)
target_sources(libupd
  PUBLIC
    libupd.h
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

    src/driver/bin.c
    src/driver/dir.c
    src/driver/lua.c
    src/driver/syncdir.c
    src/driver/tensor.c

    src/driver/prog/http.c
)
target_link_libraries(libupd
  PUBLIC
    crypto-algorithms
    cwalk
    hedley
    libcurl
    libuv
    libyaml
    luajit
    mimetype.h
    picohttpparser
    utf8.h
    wsock.h
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


# ---- external driver example ----
add_library(extdrv-hello SHARED)
set_target_properties(extdrv-hello
  PROPERTIES
    PREFIX      ""
    OUTPUT_NAME hello.x86_64
    C_STANDARD  11
)
target_compile_options(extdrv-hello
  PUBLIC ${UPD_C_FLAGS}
)
target_sources(extdrv-hello
  PRIVATE
    hello.c
    libupd.h
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
