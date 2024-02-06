#!/usr/bin/env bash

sudo apt-get install kconfig-frontends gcc-arm-none-eabi
make
tools/configure.sh -L
