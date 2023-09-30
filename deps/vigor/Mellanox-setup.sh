#!/bin/bash

netdevs=$(echo $1 | tr , ' ')
netdev_pci_addrs=$(echo $2 | tr , ' ')

set_numa_pages()
{
    echo > .echo_tmp
    for d in /sys/devices/system/node/node? ; do
        node=$(basename $d)
                    Pages=4096
        echo "echo $Pages > $d/hugepages/hugepages-2048kB/nr_hugepages" >> .echo_tmp
    done
    echo "Reserving hugepages"
    sudo sh .echo_tmp
    rm -f .echo_tmp
}

disable_rt_throttling()
{
    echo > .echo_tmp
    echo "echo -1 > /proc/sys/kernel/sched_rt_runtime_us" >> .echo_tmp
    sudo sh .echo_tmp
    rm -f .echo_tmp
}

sudo modprobe -a ib_uverbs mlx5_core mlx5_ib
#set_numa_pages

for netdev in ${netdevs}; do
    sudo ethtool -A $netdev rx off tx off
done

sudo sysctl -w vm.zone_reclaim_mode=0
sudo sysctl -w vm.swappiness=0

for netdev_pci_addr in ${netdev_pci_addrs}; do
    pci_MaxReadReq=$(sudo setpci -s $netdev_pci_addr 68.w)
    echo "$pci_MaxReadReq"
    sudo setpci -s $netdev_pci_addr 68.w=3$(echo $pci_MaxReadReq | cut -b2-4)
    pci_MaxReadReq=$(sudo setpci -s $netdev_pci_addr 68.w)
    echo "$pci_MaxReadReq"
done

disable_rt_throttling
