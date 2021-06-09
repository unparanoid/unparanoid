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
  set(CURL_DISABLE_DICT ON)
  set(CURL_DISABLE_FTP ON)
  set(CURL_DISABLE_GOPHER ON)
  set(CURL_DISABLE_IMAP ON)
  set(CURL_DISABLE_LDAP ON)
  set(CURL_DISABLE_LDAPS ON)
  set(CURL_DISABLE_MQTT ON)
  set(CURL_DISABLE_POP3 ON)
  set(CURL_DISABLE_RTSP ON)
  set(CURL_DISABLE_SMB ON)
  set(CURL_DISABLE_SMTP ON)
  set(CURL_DISABLE_TELNET ON)
  set(CURL_DISABLE_TFTP ON)
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


# ---- luajit (https://github.com/LuaJIT/LuaJIT)
function (include_luajit)
  file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/luajit" DESTINATION .)

  if (WIN32)
    if (BUILD_SHARED_LIBS)
      set(LUAJIT_TYPE   SHARED)
      set(LUAJIT_LIB    lua51.dll)
      set(LUAJIT_IMPLIB lua51.lib)
      add_custom_target(luajit-build
        WORKING_DIRECTORY luajit/src
        COMMAND           msvcbuild.bat debug
        VERBATIM
      )
    else()
      set(LUAJIT_TYPE  STATIC)
      set(LUAJIT_LIB   lua51.lib)
      add_custom_target(luajit-build
        WORKING_DIRECTORY luajit/src
        COMMAND           msvcbuild.bat static
        VERBATIM
      )
    endif()

  else()
    if (BUILD_SHARED_LIBS)
      set(LUAJIT_TYPE SHARED)
      set(LUAJIT_LIB  libluajit-5.1.so.2)
      add_custom_target(luajit-build
        WORKING_DIRECTORY luajit
        COMMAND           make -j BUILDMODE=dynamic
        COMMAND           cp src/libluajit.so src/libluajit-5.1.so.2
        VERBATIM
      )
    else()
      set(LUAJIT_TYPE STATIC)
      set(LUAJIT_LIB  libluajit.a)
      add_custom_target(luajit-build
        WORKING_DIRECTORY luajit
        COMMAND           make -j BUILDMODE=static
        VERBATIM
      )
    endif()
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
add_subdirectory(mimetype.h EXCLUDE_FROM_ALL)


# ---- picohttpparser (https://github.com/h2o/picohttpparser) ----
add_library(picohttpparser EXCLUDE_FROM_ALL)
target_include_directories(picohttpparser SYSTEM BEFORE
  PUBLIC picohttpparser
)
target_sources(picohttpparser
  PRIVATE picohttpparser/picohttpparser.c
  PUBLIC  picohttpparser/picohttpparser.h
)


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
