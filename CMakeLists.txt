cmake_minimum_required(VERSION 3.20)

project(unparanoid C)


# ---- include external deps ----
include(CTest)
include(TestBigEndian)

find_package(PkgConfig)
if (PKG_CONFIG_FOUND)
  pkg_check_modules(VALGRIND valgrind)
endif()

add_subdirectory(thirdparty EXCLUDE_FROM_ALL)


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
add_library(updcore)
set_target_properties(updcore
  PROPERTIES
    C_STANDARD 11
)
target_compile_options(updcore
  PUBLIC ${UPD_C_FLAGS}
)
target_compile_definitions(updcore
  PUBLIC
    UPD_USE_VALGRIND=$<BOOL:${VALGRIND_FOUND}>
)
target_include_directories(updcore SYSTEM
  PUBLIC
    "${VALGRIND_INCLUDE_DIRS}"
)
target_include_directories(updcore
  PUBLIC
    src
)
target_sources(updcore
  PRIVATE
    src/common.h
    src/config.c
    src/config.h
    src/driver.c
    src/driver.h
    src/file.c
    src/file.h
    src/iso.c
    src/pkg.c
    src/pkg.h

    src/driver/bin.c
    src/driver/dir.c
    src/driver/factory.c
    src/driver/syncdir.c
    src/driver/srv_tcp.c
    src/driver/tensor.c
)
target_link_libraries(updcore
  PUBLIC
    crypto-algorithms
    cwalk
    hedley
    libcurl
    libupd
    libuv
    libyaml
    mimetype.h
    tiny-regex-c
    utf8.h
    wsock.h
    zlib
)


# ---- main app ----
add_executable(upd)
target_link_libraries(upd
  PRIVATE
    updcore
)
target_sources(upd
  PRIVATE
    src/main.c
)


# ---- official drivers ----
add_subdirectory(drivers)
