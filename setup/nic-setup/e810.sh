#!/bin/bash
# Bash "strict mode"
set -euo pipefail

# Setup the kernel driver & update firmware of E810
# See: https://doc.dpdk.org/guides-20.11/nics/ice.html

DRV_VERSION=1.3.2
FIRMWARE_VERSION=2_32

# Make sure we have make
sudo apt-get -y update
sudo apt-get -y install build-essential

# Install kernel driver
if [[ (! -f ice/.version) || ("$(cat ice/.version)" != "$DRV_VERSION") ]]; then
  wget "https://downloadmirror.intel.com/30303/eng/ice-$DRV_VERSION.tar.gz"
  tar -xf "ice-$DRV_VERSION.tar.gz"
  rm "ice-$DRV_VERSION.tar.gz"
  sudo rm -rf ice
  mv "ice-$DRV_VERSION" ice
  
  pushd  "ice/src"
    sudo make install
  popd
  sudo rmmod ice
  sudo modprobe ice

  echo "$DRV_VERSION" > ice/.version
fi

# Update firmware
if [[ (! -f E810/.version) || ("$(cat E810/.version)" != "$FIRMWARE_VERSION") ]]; then
  # Make sure the NICs are bound to ice before doing firmware update
  DPDK_BIND_SCRIPT=$(find $HOME -wholename "*dpdk*/usertools/dpdk-devbind.py")
  if [[ "$DPDK_BIND_SCRIPT" != "" ]]; then
    E810_PORTS=$(sudo $DPDK_BIND_SCRIPT --status | grep E810 | cut -d' '  -f1 | tr '\n' ' ')
    sudo $DPDK_BIND_SCRIPT --bind=ice $E810_PORTS
  fi

  sudo rm -rf E810
  wget "https://downloadmirror.intel.com/30297/eng/E810_NVMUpdatePackage_v${FIRMWARE_VERSION}_Linux.tar.gz"
  tar -xf "E810_NVMUpdatePackage_v${FIRMWARE_VERSION}_Linux.tar.gz"
  rm "E810_NVMUpdatePackage_v${FIRMWARE_VERSION}_Linux.tar.gz"

  pushd E810/Linux_x64
    sudo ./nvmupdate64e -u -l -o update.xml -b -c nvmupdate.cfg
  popd
  
  echo "You may need to REBOOT to complete firmware update!"
  echo "$FIRMWARE_VERSION" > E810/.version
fi

