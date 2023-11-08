# Network Function Operating System (NFOS)

A programming model, runtime, and profiler for productively developing software network functions (NFs) that scale on multicore machines without dealing with concurrency.

See our [EuroSys'24](https://2024.eurosys.org/) paper "Transparent Multicore Scaling of Single-Threaded Network Functions".

## Repository structure

- `README.md`: This file
- `src`: Source code
  - `src/include/nf.h`: NFOS interface
- `nf`: Example NFs
- `nfos-experiments`: Scripts for the experiments presented in the paper and instructions.
- `utils`: Scripts, see utils/README.md 

## Build dependencies

```bash
# Note: NFOS has been tested on Ubuntu 20.04 with E810 100Gbps NICs (firmware
# version 2.32, driver version 1.3.2).

# Note: Put nfos under ${HOME}, currently the dependencies
# can only be built if nfos is put under ${HOME}.

cd ~/nfos
git submodule init
git submodule update

cd ~/nfos && bash deps/setup-deps.sh
. ${HOME}/.profile

# Note: As the last step of dependency setup, make sure you follow the
# instruction in deps/mv-rlu on "Setting Ordo value"!
```

## Set up the server for performance

To maximize and stabilize performance, we recommend the following server configurations:

- Enable the "performance" configuration in the BIOS if it exists.

- In ```/etc/default/grub```, replace the line ```GRUB_CMDLINE_LINUX_DEFAULT="..."``` with
```GRUB_CMDLINE_LINUX_DEFAULT="quiet splash hugepages=8192 intel_iommu=on iommu=pt isolcpus=0-46:1/2 nohz_full=0-46:1/2 rcu_nocbs=0-46:1/2 idle=poll intel_pstate=disable intel_idle.max_cstate=0 processor.max_cstate=0 audit=0 nosoflockup nmi_watchdog=0 hpet=disable mce=off numa_balancing=disable"```.
Replace "0-46:1/2" in the relevant boot parameters with the list of CPU cores you will use for the NF ([format of this list](https://www.kernel.org/doc/html/v4.14/admin-guide/kernel-parameters.html#cpu-lists)). Then do ```sudo update-grub; sudo reboot```

Note: These configurations make the server run with maximum power, revert them if you are not benchmarking your NF!

## Run NF

To manually run a NF, go to the directory of a specific NF and use one of the
make target listed below.

If you have any issue building an NF, a common fix is to source .profile.

```bash
cd ~/nfos/nf/<name of the nf>

# Build and run an NF
#
# EXP_TIME: expiration time of packet sets (in microseconds)
# START_CORE: id of the first core to use
# NUM_CORES: number of nfos worker cores that process packets 
# CORE_ID_STRIDE: stride of the core id
# Example: START_CORE=8 NUM_CORES=2 CORE_ID_STRIDE=2
#        => actual cores used: 8,10,12
#        with core 12 as the control task core that NFOS reserves to execute periodic tasks, etc.
# NOTE: NFOS always reserves one core for control tasks.
make run EXP_TIME=<EXP_TIME> LCORES=$(python3 -c "print(','.join([str(<START_CORE> + x * <CORE_ID_STRIDE>) for x in range(<NUM_CORES> + 1)]))")

# Build and profile an NF with NFOS's scalability profiler
make run-scal-profile EXP_TIME=<EXP_TIME> LCORES=$(python3 -c "print(','.join([str(<START_CORE> + x * <CORE_ID_STRIDE>) for x in range(<NUM_CORES> + 1)]))")

# Build and run an NF with debug logs
make run-debug-log EXP_TIME=<EXP_TIME> LCORES=$(python3 -c "print(','.join([str(<START_CORE> + x * <CORE_ID_STRIDE>) for x in range(<NUM_CORES> + 1)]))")

# Build and debug the NF with gdb
# Step 1: Build NF with debug symbols
make debug EXP_TIME=<EXP_TIME> LCORES=$(python3 -c "print(','.join([str(<START_CORE> + x * <CORE_ID_STRIDE>) for x in range(<NUM_CORES> + 1)]))")
# Step 2: launch the NF in gdb
sudo su
gdb build/app/nf
(gdb) run
exit
```

## Run experiments in the paper

See nfos-experiments/README.md
