#!/bin/sh

git submodule init
git submodule update
meson build
(cd build; ninja)
