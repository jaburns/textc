#!/usr/bin/env bash
set -e

mkdir -p text/tool
mkdir -p text/bin

clang -Wall -Werror -Wno-unused-variable -Wno-unused-function \
    $(pkg-config --cflags --libs pango pangocairo fontconfig) \
    -O0 -g -o bin/textc main.c vendor/lodepng.c

cp bin/textc text/tool/textc
cp msdfgen/build/msdfgen text/tool/msdfgen

if [[ -n "$1" ]]; then
    cd text
    tool/textc "$1"
fi
