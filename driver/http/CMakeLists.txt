add_driver(upd.http)
target_sources(upd.http
  PRIVATE
    http.c
)
target_link_libraries(upd.http
  PRIVATE
    crypto-algorithms
    hedley
    picohttpparser
    utf8.h
    wsock.h
)
