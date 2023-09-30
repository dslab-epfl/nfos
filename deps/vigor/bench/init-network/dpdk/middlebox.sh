#!/bin/bash
. ./config.sh

echo "[init] Binding middlebox interfaces to DPDK..."
LOADED_DPDK=0
if [ $MB_DPDK_NIC_DRIVER == "Mellanox" ]; then
  . ./util/dpdk-functions.sh
  #set_numa_pages
  sudo modprobe -a ib_uverbs mlx5_core mlx5_ib
  LOADED_DPDK=1
else 
  for pci in "$MB_PCI_INTERNAL" "$MB_PCI_EXTERNAL"; do
    if ! sudo "$RTE_SDK/usertools/dpdk-devbind.py" --status | grep -F "$pci" | grep -q "drv=$MB_DPDK_NIC_DRIVER"; then
      if [ $LOADED_DPDK -eq 0 ]; then
        echo "[init] Initializing DPDK on middlebox..."
        . ./util/dpdk-functions.sh
        set_numa_pages
        load_igb_uio_module
        LOADED_DPDK=1
      fi
  
      sudo "$RTE_SDK/usertools/dpdk-devbind.py" --force --bind "$MB_DPDK_NIC_DRIVER" "$pci"
    fi
  done
fi
