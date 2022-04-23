#!/bin/bash

# make the build
git submodule init && git submodule update
make BUILD_TLS=yes -j2 KEYDB_CFLAGS='-Werror' KEYDB_CXXFLAGS='-Werror'

# gen-cert
./utils/gen-test-certs.sh