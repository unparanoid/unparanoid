set(BUILD_TESTING OFF)

# ---- hedley (https://github.com/nemequ/hedley) ----
add_library(hedley INTERFACE)
target_include_directories(hedley SYSTEM
  INTERFACE hedley
)
target_sources(hedley
  INTERFACE hedley/hedley.h
)
