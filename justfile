default:
    @just --list

check:
    plenty check

build:
    mkdir -p build/compressor.lv2
    plenty --shared-library src/lib.plenty --library-name compressor -o build/compressor.lv2/libcompressor.so
    cc -std=c11 -O2 -Wall -Wextra -Werror -fPIC -shared lv2/plugin.c -Ibuild/compressor.lv2 -Lbuild/compressor.lv2 -lcompressor -lm -Wl,-rpath,'$ORIGIN' -o build/compressor.lv2/plugin.so
    cp lv2/*.ttl build/compressor.lv2/

test: build
    python3 tests/smoke.py

clean:
    rm -rf build target
