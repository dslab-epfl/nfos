# Change this to Mellanox for Mellanox NICs and Intel for Intel NICs
# Currently supporting: Intel 82599ES, Mellanox ConnectX-5, Intel E810
export MB_NIC_TYPE="Intel"
export TESTER_NIC_TYPE="Mellanox"

# --------- #
# Middlebox #
# --------- #

export MB_CPU=$CPUS
export MB_HOST=mkirk1
export MB_PCI_INTERNAL=0000:02:00.0
export MB_PCI_EXTERNAL=0000:81:00.0

if [ "$MB_NIC_TYPE" = "Intel" ]; then
  export MB_DPDK_NIC_DRIVER=igb_uio
elif [ "$MB_NIC_TYPE" = "Mellanox" ]; then
  export MB_DPDK_NIC_DRIVER=Mellanox
else
  echo "No MB NIC type specified, abort"
  exit
fi

# ------ #
# Tester #
# ------ #

export TESTER_HOST=kirk5.maas
export TESTER_PCI_INTERNAL=0000:02:00.0
export TESTER_PCI_EXTERNAL=0000:81:00.0

if [ "$TESTER_NIC_TYPE" = "Intel" ]; then
  export TESTER_DPDK_NIC_DRIVER=igb_uio
elif [ "$TESTER_NIC_TYPE" = "Mellanox" ]; then
  export TESTER_DPDK_NIC_DRIVER=Mellanox
else
  echo "No TESTER NIC type specified, abort"
  exit
fi

# ----- #
# Other #
# ----- #

# Do not change unless Linux or DPDK change!
# Note: Change this to the one you are using
export KERN_NIC_DRIVER=ixgbe

# Change only if you know what you're doing (e.g. you installed DPDK to a different path than )
if [ "$RTE_SDK" = '' ]; then
  export RTE_SDK=$HOME/dpdk
  export RTE_TARGET=x86_64-native-linuxapp-gcc
fi

if [ "$PKG_CONFIG_PATH" = '' ]; then
  export PKG_CONFIG_PATH=$HOME/dpdk-install/lib/x86_64-linux-gnu/pkgconfig
fi

# --- #
# Old #
# --- #

# No need to touch this unless you want to resurrect our old benchmarks for linux-based middleboxes
export MB_DEVICE_INTERNAL=p802p2
export MB_DEVICE_EXTERNAL=p802p1
export MB_IP_INTERNAL=192.168.6.2
export MB_IP_EXTERNAL=192.168.4.2
export MB_IPS_BACKENDS="192.168.4.3 192.168.4.4 192.168.4.5 192.168.4.6"
export TESTER_MAC_INTERNAL=90:e2:ba:55:12:21
export TESTER_MAC_EXTERNAL=90:e2:ba:55:12:20
export TESTER_IP_INTERNAL=192.168.6.5
export TESTER_IP_EXTERNAL=192.168.4.10
