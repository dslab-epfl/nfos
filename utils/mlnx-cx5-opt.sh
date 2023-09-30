#!/bin/bash

# Opt for Mellanox connectx-5 card to get 100Gbps, run this every time you start the system
# TODO: Automate this at boot using???

sudo lshw -c network -businfo | grep ConnectX-5 | sed -n -e 's/pci@0000://p' > .nic_info
netdevs=$(awk '{printf "%s%s",sep,$2; sep=" "} END{print ""}' .nic_info)
netdev_pci_addrs=$(awk '{printf "%s%s",sep,$1; sep=" "} END{print ""}' .nic_info)
rm .nic_info

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
# Switch this on if using 2MB pages
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
