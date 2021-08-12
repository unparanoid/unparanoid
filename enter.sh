#!/bin/bash

set -e

docker build --tag falsycat:upd .

exec docker run --rm -it  \
  --name upd  \
  --user 1000:1000  \
  --hostname upd  \
  --publish 8080:8080  \
  --env "DISPLAY"  \
  --volume "$PWD/../:/repos"  \
  --volume "$PWD/build:/build"  \
  --volume "/tmp/.X11-unix:/tmp/.X11-unix"  \
  falsycat:upd /bin/bash
