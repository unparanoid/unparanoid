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


# ---- additional headers for MSVC
#   - https://github.com/cdschreiber/c11
#   - https://www.khronos.org/registry/OpenGL/api/GL/glext.h
add_library(for-msvc INTERFACE)
if (CMAKE_C_COMPILER_ID STREQUAL "MSVC")
  target_sources(for-msvc
    INTERFACE
      for-msvc/stdatomic.h
  )
  target_include_directories(for-msvc
    INTERFACE for-msvc
  )
endif()


# ---- glew (https://github.com/Perlmint/glew-cmake) ----
add_subdirectory(glew)
add_library(glew ALIAS libglew_static)


# ---- glfw (https://github.com/glfw/glfw) ----
set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(glfw)


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


# ---- miniaudio (https://github.com/mackron/miniaudio) ----
add_library(miniaudio)
target_include_directories(miniaudio SYSTEM BEFORE
  PUBLIC miniaudio
)
target_sources(miniaudio
  PUBLIC miniaudio/miniaudio.h
)
set_source_files_properties(miniaudio/miniaudio.h
  PROPERTIES
    LANGUAGE C
)
target_compile_definitions(miniaudio
  PRIVATE MINIAUDIO_IMPLEMENTATION
)
if (CMAKE_SYSTEM_NAME STREQUAL Linux)
  target_link_libraries(miniaudio
    PRIVATE m
  )
endif()


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
