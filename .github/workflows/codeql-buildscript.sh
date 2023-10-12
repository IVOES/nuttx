#!/usr/bin/env bash

sudo apt-get install kconfig-frontends

git clone https://github.com/apache/nuttx-apps.git ../apps

TOPDIR=.
DUMPCFGS=""
configlist=`find ${TOPDIR}/boards/arm -name defconfig`
for defconfig in ${configlist}; do
  config=`dirname ${defconfig} | sed -e "s,${TOPDIR}/boards/,,g"`
  boardname=`echo ${config} | cut -d'/' -f3`
  configname=`echo ${config} | cut -d'/' -f5`
  DUMPCFGS="${DUMPCFGS} ${boardname}:${configname}"
done

for cfg in $(echo "$DUMPCFGS" | sed 's, ,\n,g' | sort -t ':' -k2 -u); do
  echo "Compiling $cfg"
  ./tools/configure.sh $cfg
  make CROSSDEV=arm-none-eabi- -j$(nproc)
  make distclean
done

rm -rf ../apps
