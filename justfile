plenty := env_var_or_default("PLENTY", "plenty")

default:
    @just --list

check:
    "{{plenty}}" --check-module src/lib.plenty

build:
    mkdir -p build/compressor.lv2
    "{{plenty}}" --shared-library src/lib.plenty --library-name compressor -o build/compressor.lv2/plugin.so
    cp lv2/*.ttl build/compressor.lv2/

test: build
    python3 tests/smoke.py

clean:
    rm -rf build target
