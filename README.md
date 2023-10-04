# Network Function Operating System (NFOS)

A framework that enables developers to develop scalable software network functions without dealing with concurrency.

## Repository structure

- `README.md`: This file
- `src`: Source code
  - `src/include/nf.h`: NFOS interface
- `nf`: Example NFs
- `utils`: Scripts, see utils/README.md 

## Build dependencies

```bash
# Note: Put nfos under ${HOME}, currently the dependencies
# can only be built if nfos is put under ${HOME}.

cd ~/nfos
git submodule init
git submodule update

cd ~/nfos && bash deps/setup-deps.sh
. ${HOME}/.profile
```

## Run NF

If you would like to manually run a NF, go to the right NF directory and one of the
appropriate make target listed below.

If you have any issue building an NF, a common fix is to source .profile

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
