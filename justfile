set positional-arguments

plenty := env_var_or_default("PLENTY", "plenty")

default:
    @just --list

check:
    "{{plenty}}" --check-module src/lib.plenty
    "{{plenty}}" --check-module src/clap.plenty

build:
    mkdir -p build/compressor.lv2
    "{{plenty}}" --shared-library src/lib.plenty --library-name compressor -o build/compressor.lv2/plugin.so
    cp lv2/*.ttl build/compressor.lv2/

test: build
    python3 tests/smoke.py

# Launch a DAW with the local LV2 bundle on its search path.
daw program *args: build
    #!/usr/bin/env bash
    set -eu
    export LV2_PATH="$PWD/build:${LV2_PATH:-$HOME/.lv2:/usr/local/lib/lv2:/usr/lib/lv2}"
    exec "$@"

clean:
    rm -rf build target

# Linux GNU x86_64; requires the CLAP interop compiler and libatomic.
build-clap:
    mkdir -p build
    "{{plenty}}" --shared-library src/clap.plenty --library-name compressor --unloadable-library --link-arg -latomic -o build/plenty-compressor.clap

test-clap: build-clap
    cc -std=c11 -O2 -Wall -Wextra -Werror -Itests/vendor/clap/include tests/clap_host.c -o build/clap-host -ldl -lpthread -lm
    ./build/clap-host "$PWD/build/plenty-compressor.clap"

test-all: test test-clap
