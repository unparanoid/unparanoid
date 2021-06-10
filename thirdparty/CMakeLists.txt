set(BUILD_TESTING OFF)

if (CMAKE_BUILD_TYPE STREQUAL "Debug")
  set(BUILD_SHARED_LIBS ON)
else()
  set(BUILD_SHARED_LIBS OFF)
endif()


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


# ---- curl (https://github.com/curl/curl) ----
function(include_curl)
  set(HTTP_ONLY ON)
  if (WIN32)
    set(CMAKE_USE_SCHANNEL  ON)
  endif()
  add_subdirectory(curl EXCLUDE_FROM_ALL)
endfunction()
include_curl()


# ---- cwalk (https://github.com/likle/cwalk) ----
set(IGNORE_WARNINGS ON)
add_subdirectory(cwalk EXCLUDE_FROM_ALL)


# ---- libupd (https://github.com/falsycat/libupd)
add_subdirectory(libupd EXCLUDE_FROM_ALL)


# ---- libuv (https://github.com/libuv/libuv)
add_subdirectory(libuv EXCLUDE_FROM_ALL)
if (BUILD_SHARED_LIBS)
  add_library(libuv ALIAS uv)
else()
  add_library(libuv ALIAS uv_a)
endif()


# ---- libyaml (https://github.com/yaml/libyaml)
add_subdirectory(libyaml EXCLUDE_FROM_ALL)
add_library(libyaml ALIAS yaml)


# ---- mimetype.h (https://github.com/falsycat/mimetype.h) ----
add_subdirectory(mimetype.h EXCLUDE_FROM_ALL)


# ---- wsock.h (https://github.com/falsycat/wsock.h) ----
add_subdirectory(wsock.h EXCLUDE_FROM_ALL)


# ---- zlib (https://github.com/madler/zlib) ----
add_subdirectory(zlib EXCLUDE_FROM_ALL)
target_include_directories(zlib SYSTEM BEFORE
  PUBLIC zlib
)
target_include_directories(zlibstatic SYSTEM BEFORE
  PUBLIC zlib
)
if (BUILD_SHARED_LIBS)
  add_library(zlib_ ALIAS zlib)
else()
  add_library(zlib_ ALIAS zlibstatic)
endif()
