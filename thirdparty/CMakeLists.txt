set(BUILD_TESTING OFF)


# ---- crypto-algorithms (https://github.com/B-Con/crypto-algorithms) ----
add_library(crypto-algorithms EXCLUDE_FROM_ALL)
target_include_directories(crypto-algorithms SYSTEM BEFORE
  PUBLIC crypto-algorithms
)
target_sources(crypto-algorithms
  PUBLIC
    crypto-algorithms/base64.h
    crypto-algorithms/sha1.h
  PRIVATE
    crypto-algorithms/base64.c
    crypto-algorithms/sha1.c
)


# ---- cwalk (https://github.com/likle/cwalk) ----
add_subdirectory(cwalk EXCLUDE_FROM_ALL)


# ---- hedley (https://github.com/nemequ/hedley) ----
add_library(hedley INTERFACE)
target_include_directories(hedley SYSTEM BEFORE
  INTERFACE hedley
)
target_sources(hedley
  INTERFACE hedley/hedley.h
)


# ---- libuv (https://github.com/libuv/libuv)
add_subdirectory(libuv EXCLUDE_FROM_ALL)
if (CMAKE_BUILD_TYPE STREQUAL "Debug")
  add_library(libuv ALIAS uv)
else()
  add_library(libuv ALIAS uv_a)
endif()


# ---- libyaml (https://github.com/yaml/libyaml)
add_subdirectory(libyaml EXCLUDE_FROM_ALL)
add_library(libyaml ALIAS yaml)


# ---- mimetype.h (https://github.com/falsycat/mimetype.h) ----
add_subdirectory(mimetype.h EXCLUDE_FROM_ALL)


# ---- luajit (https://github.com/LuaJIT/LuaJIT) ----
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/luajit" DESTINATION .)
set_directory_properties(
  PROPERTIES
    ADDITIONAL_CLEAN_FILES luajit/*
)

set(LUAJIT_MAKE make -j)

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(LUAJIT_LIB "${CMAKE_CURRENT_BINARY_DIR}/luajit/src/libluajit-5.1.so.2")
  set(LUAJIT_MAKE ${LUAJIT_MAKE} BUILDMODE=dynamic)
  set(LUAJIT_MAKE ${LUAJIT_MAKE} && cp src/libluajit.so "${LUAJIT_LIB}")
else()
  set(LUAJIT_LIB "${CMAKE_CURRENT_BINARY_DIR}/luajit/src/libluajit.a")
  set(LUAJIT_MAKE ${LUAJIT_MAKE} BUILDMODE=static)
endif()

add_custom_target(luajit-build
  WORKING_DIRECTORY luajit
  COMMAND           ${LUAJIT_MAKE}
)

add_library(luajit INTERFACE)
target_include_directories(luajit SYSTEM
  INTERFACE luajit/src
)
target_link_libraries(luajit
  INTERFACE "${LUAJIT_LIB}"
)
add_dependencies(luajit
  luajit-build
)


# ---- picohttpparser (https://github.com/h2o/picohttpparser) ----
add_library(picohttpparser EXCLUDE_FROM_ALL)
target_include_directories(picohttpparser SYSTEM BEFORE
  PUBLIC picohttpparser
)
target_sources(picohttpparser
  PRIVATE picohttpparser/picohttpparser.c
  PUBLIC  picohttpparser/picohttpparser.h
)


# ---- utf8.h (https://github.com/sheredom/utf8.h) ----
add_library(utf8.h INTERFACE)
target_include_directories(utf8.h SYSTEM BEFORE
  INTERFACE utf8.h
)
target_sources(utf8.h
  INTERFACE utf8.h/utf8.h
)


# ---- wsock.h (https://github.com/falsycat/wsock.h) ----
add_subdirectory(wsock.h EXCLUDE_FROM_ALL)
