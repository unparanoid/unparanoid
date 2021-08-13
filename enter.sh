#!/bin/bash

set -e

if [[ $UID != 1000 ]]; then
  echo "WARNING: entering the environment as uid 1000"
fi

mkdir -p build

docker build --tag falsycat:upd .
exec docker run --rm -it  \
  --name upd  \
  --user 1000:1000  \
  --hostname upd  \
  --publish 8080:8080  \
  --env "DISPLAY"  \
  --volume "$PWD/:/repo/"  \
  --volume "$PWD/build:/build/"  \
  --volume "/tmp/.X11-unix:/tmp/.X11-unix"  \
  falsycat:upd /bin/bash
