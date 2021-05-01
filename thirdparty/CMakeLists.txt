set(BUILD_TESTING OFF)


# ---- cwalk (https://github.com/likle/cwalk) ----
add_subdirectory(cwalk)


# ---- hedley (https://github.com/nemequ/hedley) ----
add_library(hedley INTERFACE)
target_include_directories(hedley SYSTEM
  INTERFACE hedley
)
target_sources(hedley
  INTERFACE hedley/hedley.h
)


# ---- utf8.h (https://github.com/sheredom/utf8.h) ----
add_library(utf8.h INTERFACE)
target_include_directories(utf8.h SYSTEM
  INTERFACE utf8.h
)
target_sources(utf8.h
  INTERFACE utf8.h/utf8.h
)
