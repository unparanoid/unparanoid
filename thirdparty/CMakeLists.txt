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


# ---- luajit (https://github.com/LuaJIT/LuaJIT) ----
function (include_luajit)
  file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/luajit" DESTINATION .)

  if (WIN32)
    set(LUAJIT_TYPE  STATIC)
    set(LUAJIT_LIB   lua51.lib)
    add_custom_target(luajit-build
      WORKING_DIRECTORY luajit/src
      COMMAND           msvcbuild.bat static
      VERBATIM
    )

  else()
    set(LUAJIT_TYPE STATIC)
    set(LUAJIT_LIB  libluajit.a)
    add_custom_target(luajit-build
      WORKING_DIRECTORY luajit
      COMMAND           make -j BUILDMODE=static CFLAGS=-fPIC
      VERBATIM
    )
  endif()

  add_library(luajit-imported ${LUAJIT_TYPE} IMPORTED)
  set_target_properties(luajit-imported PROPERTIES
    IMPORTED_LOCATION "${CMAKE_CURRENT_BINARY_DIR}/luajit/src/${LUAJIT_LIB}"
    IMPORTED_IMPLIB   "${CMAKE_CURRENT_BINARY_DIR}/luajit/src/${LUAJIT_IMPLIB}"
# INTERFACE_INCLUDE_DIRECTORIES doesn't work well ;(
  )
  add_dependencies(luajit-imported luajit-build)

  add_library(luajit INTERFACE)
  target_link_libraries(luajit
    INTERFACE luajit-imported $<$<PLATFORM_ID:Linux>:m>
  )
  target_include_directories(luajit SYSTEM BEFORE
    INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/luajit/src"
  )
endfunction()
include_luajit()


# ---- mimetype.h (https://github.com/falsycat/mimetype.h) ----
add_subdirectory(mimetype.h)


# ---- picohttpparser (https://github.com/h2o/picohttpparser) ----
add_library(picohttpparser)
target_include_directories(picohttpparser SYSTEM BEFORE
  PUBLIC picohttpparser
)
target_sources(picohttpparser
  PRIVATE picohttpparser/picohttpparser.c
  PUBLIC  picohttpparser/picohttpparser.h
)


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
