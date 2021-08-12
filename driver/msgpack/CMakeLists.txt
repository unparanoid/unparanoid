add_driver(upd.msgpack)
target_sources(upd.msgpack
  PRIVATE msgpack.c
)
target_link_libraries(upd.msgpack
  PRIVATE
    utf8.h
    msgpackc
)
