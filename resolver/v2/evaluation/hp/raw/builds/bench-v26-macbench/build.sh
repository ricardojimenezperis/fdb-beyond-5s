#!/bin/bash
W=$1; O=$2
docker run --rm -v /data/fdb:/data/fdb -w $O foundationdb/build:rockylinux9-latest bash -c "cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=/usr/local/bin/clang -DCMAKE_CXX_COMPILER=/usr/local/bin/clang++ -DUSE_LIBCXX=1 -DUSE_LD=LLD $W > $O/../cmake-$(basename $O).log 2>&1 && ninja fdbserver > $O/../build-$(basename $O).log 2>&1; echo exit=\$? > $O/../exit-$(basename $O).txt; chown -R $(id -u):$(id -g) $O $O/.."
