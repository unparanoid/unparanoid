set(BUILD_TESTING OFF)

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(BUILD_SHARED_LIBS ON)
else()
  set(BUILD_SHARED_LIBS OFF)
endif()


# ---- crypto-algorithms (https://github.com/B-Con/crypto-algorithms) ----
add_library(crypto-algorithms)
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


# ---- curl (https://github.com/curl/curl) ----
function(include_curl)
  set(HTTP_ONLY ON)
  if (WIN32)
    set(CMAKE_USE_SCHANNEL  ON)
  endif()
  add_subdirectory(curl)
endfunction()
include_curl()


# ---- cwalk (https://github.com/likle/cwalk) ----
set(IGNORE_WARNINGS ON)
add_subdirectory(cwalk)


# ---- libupd (https://github.com/falsycat/libupd)
add_subdirectory(libupd)


# ---- libuv (https://github.com/libuv/libuv)
add_subdirectory(libuv)
if (BUILD_SHARED_LIBS)
  add_library(libuv ALIAS uv)
else()
  add_library(libuv ALIAS uv_a)
endif()


# ---- libyaml (https://github.com/yaml/libyaml)
add_subdirectory(libyaml)
add_library(libyaml ALIAS yaml)


# ---- mimetype.h (https://github.com/falsycat/mimetype.h) ----
add_subdirectory(mimetype.h)


# ---- tiny-regex-c (https://github.com/kokke/tiny-regex-c) ----
add_library(tiny-regex-c)
target_sources(tiny-regex-c
  PRIVATE tiny-regex-c/re.c
  PUBLIC  tiny-regex-c/re.h
)
target_include_directories(tiny-regex-c SYSTEM BEFORE
  PUBLIC tiny-regex-c
)


# ---- wsock.h (https://github.com/falsycat/wsock.h) ----
add_subdirectory(wsock.h)


# ---- zlib (https://github.com/madler/zlib) ----
add_subdirectory(zlib-ng)
