#!/usr/bin/env bash

mkdir -p bin
mkdir -p output

clang -Wall -Werror -Wno-unused-variable -Wno-unused-function \
    $(pkg-config --cflags --libs pango pangocairo fontconfig) \
    -O0 -g -o bin/textc main.c vendor/lodepng.c