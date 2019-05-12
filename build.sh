#!/bin/sh

git submodule init
# Remove or increase --depth if server errors
git submodule update --depth 50
meson build
(cd build; ninja)
