#!/bin/bash
# Bash "strict mode"
set -euo pipefail

# Setup the kernel driver of ConnectX-5
# Requirement: OS is ubuntu 18.04

# This is the only working version I found for moongen with support to Ubuntu 18.04
DRV_VERSION="4.7-1.0.0.1"

# Make sure we have make
sudo apt-get -y update
sudo apt-get -y install build-essential

# Install kernel driver & update firmware
# See: https://docs.mellanox.com/display/MLNXOFEDv471001/Installing+Mellanox+OFED
# The following procedure may not apply to later or earlier version of OFED, please
# check relevant OFED installation manual to see what has been changed.
# You may also need to install the upstream RDMA-core libs, see if this is required on
# DPDK's mlx5 page
if ! grep "$DRV_VERSION" /etc/apt/sources.list.d/mlnx_ofed.list >>/dev/null 2>&1; then
  # Uninstall previous version if exists
  # See: https://docs.mellanox.com/display/MLNXOFEDv471001/Uninstalling+Mellanox+OFED
  if [[ -f /usr/sbin/ofed_uninstall.sh ]]; then
    sudo /usr/sbin/ofed_uninstall.sh
  fi

  sudo rm -rf *OFED*
  wget "https://content.mellanox.com/ofed/MLNX_OFED-$DRV_VERSION/MLNX_OFED_LINUX-$DRV_VERSION-ubuntu18.04-x86_64.tgz"
  tar -xf "MLNX_OFED_LINUX-$DRV_VERSION-ubuntu18.04-x86_64.tgz"
  
  echo "deb file:$PWD/MLNX_OFED_LINUX-$DRV_VERSION-ubuntu18.04-x86_64/DEBS_UPSTREAM_LIBS ./" | sudo tee /etc/apt/sources.list.d/mlnx_ofed.list > /dev/null
  wget -qO - http://www.mellanox.com/downloads/ofed/RPM-GPG-KEY-Mellanox | sudo apt-key add -
  sudo apt-get -y update
  sudo apt-get -y install mlnx-ofed-dpdk-upstream-libs
  
  # Update firmware
  sudo apt-get -y install mlnx-fw-updater 
  
  echo "You may need to REBOOT to complete driver/firmware update!"
fi

