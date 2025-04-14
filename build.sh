#!/usr/bin/env bash
set -e

mkdir -p bin
mkdir -p text/tool
mkdir -p text/bin

if [[ "$1" == 'release' ]]; then
    clang -O3 -Wall -Werror \
        $(pkg-config --cflags --libs pango pangocairo fontconfig) \
        -o bin/textc main.c vendor/lodepng.c
else
    clang -O0 -g -Wall -Werror -Wno-unused-variable -Wno-unused-function \
        $(pkg-config --cflags --libs pango pangocairo fontconfig) \
        -o bin/textc main.c vendor/lodepng.c
fi

cp bin/textc text/tool/textc
cp msdfgen/build/msdfgen text/tool/msdfgen

if [[ -n "$1" && "$1" != 'release' ]]; then
    cd text
    rm -f .cache
    tool/textc "$1"
fi
