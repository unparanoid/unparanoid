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
add_subdirectory(cwalk)


# ---- hedley (https://github.com/nemequ/hedley) ----
add_library(hedley INTERFACE)
target_include_directories(hedley SYSTEM BEFORE
  INTERFACE hedley
)
target_sources(hedley
  INTERFACE hedley/hedley.h
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
add_subdirectory(wsock.h)
