add_driver(upd.png)
target_sources(upd.png
  PRIVATE
    png.c
    png.h
    reader.h
)
target_link_libraries(upd.png
  PRIVATE
    hedley
    utf8.h
    zlib
)


if (BUILD_TESTING)
  add_executable(test-upd.png)
  target_sources(test-upd.png
    PRIVATE
      test.c
      png.h
      reader.h
  )
  target_link_libraries(test-upd.png
    PRIVATE
      hedley
      libupd
      zlib
  )
  target_compile_options(test-upd.png
    PRIVATE ${UPD_C_FLAGS}
  )
endif()
