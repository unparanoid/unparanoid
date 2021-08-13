FROM    debian:stable-slim
WORKDIR /build
EXPOSE  8080

RUN  \
  apt update &&  \
  DEBIAN_FRONTEND=nointeractive  \
  apt install -y --no-install-recommends  \
    bash  \
    curl  \
    gcc  \
    gcc-multilib  \
    gdb  \
    git  \
    g++  \
    libgl1-mesa-dev  \
    libglu1-mesa-dev  \
    libssl-dev  \
    make  \
    mesa-common-dev  \
    python3-pip  \
    python3-setuptools  \
    pkgconf  \
    valgrind  \
    vim  \
    xorg-dev  \
    xvfb &&  \
  pip3 install htmlark

RUN  \
  curl -kL https://github.com/Kitware/CMake/releases/download/v3.20.3/cmake-3.20.3-Linux-x86_64.sh > /build/cmake-install &&  \
  /bin/sh /build/cmake-install --skip-license --prefix=/ &&  \
  rm /build/cmake-install

RUN  \
  apt purge -y curl &&  \
  apt clean &&  \
  rm -rf /var/lib/apt/lists/* &&  \
  useradd  \
    -d /repos  \
    -u 1000  \
    -s /usr/bin/bash user
