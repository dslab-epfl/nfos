#!/bin/bash

NFOS_PATH=$PWD

# build sys of mv-rlu complains with gcc 9 (default on ubuntu 20.04),
# use gcc 8 instead
sudo apt-get install gcc-8
pushd deps/mv-rlu/lib
    make libmvrlu-ordo.a CC=gcc-8
popd

# build DPDK
pushd $HOME
. $NFOS_PATH/deps/vigor/setup.sh no-verify dpdk-only
popd
