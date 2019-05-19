#!/bin/bash

git submodule init
# Remove or increase --depth if server errors
git submodule update --depth 50
meson build --prefix=/usr
(cd build; ninja)

LIBDIR=/usr/lib32
if [ -d /usr/lib/i386-linux-gnu ]; then
  LIBDIR=/usr/lib/i386-linux-gnu
fi

#meson build32 --cross-file build32.txt --prefix=/usr --libdir=$LIBDIR
#(cd build32; ninja)
