#!/bin/bash
TAG="HEAD"
CUR_BRANCH=$(git rev-parse --abbrev-ref HEAD)

cd $HOME/vigor/vignop
# git stash push || exit
git checkout $TAG || exit

for LB_ENG in "FLOW_GROUP=0 RSS=1" "FLOW_GROUP=1 RSS=1" "FLOW_GROUP=0 RSS=0" "FLOW_GROUP=1 RSS=0"; do

    RESULTS=$(echo "results.apr29.$LB_ENG/" | tr ' ' '-')
    mkdir -p $RESULTS || exit

    ##1
    echo "Benchmark 1 (Throughput scaling)"
    for i in 1 2 4 6 8 10 12; do
        if [[ $(echo $LB_ENG | cut -d' ' -f2) == "RSS=0" ]]; then
            NUM_CORES=$((i + 1))
        else
            NUM_CORES=$i
        fi

        make clean

        make benchmark-throughput \
        CPUS="0-$NUM_CORES" \
        HEAT_UP_RATE=100000 \
        PACKET_SIZE=60 \
        FLOW_RULE_LEVELS=9 \
        BENCH_FLOW="bench4" \
        $LB_ENG \
        >/dev/null 2>&1

        mv benchmark-throughput.log "$RESULTS/thr-vs-core-count.log.$i"
    done

    for i in 1 2 4 6 8 10 12; do
        grep rx_good_packets "$RESULTS/thr-vs-core-count.log.$i" | tail -n1 | awk '{print var, $5}' var=$i >> "$RESULTS/thr-vs-core-count.summary"
    done


    if [[ $(echo $LB_ENG | cut -d' ' -f2) == "RSS=0" ]]; then
        NUM_CORES=13
    else
        NUM_CORES=12
    fi

    ##2
    echo "Benchmark 2 (Latency of load balancing steps)"
    make clean

    make benchmark-throughput \
    CPUS="0-$NUM_CORES" \
    HEAT_UP_RATE=100000 \
    PACKET_SIZE=60 \
    FLOW_RULE_LEVELS=9 \
    BENCH_FLOW="bench5" \
    $LB_ENG \
    >/dev/null 2>&1

    mv benchmark-throughput.log "$RESULTS/lb-latency.log"
    grep "step.*us" "$RESULTS/lb-latency.log" >> "$RESULTS/lb-latency.summary"

    ##3
    echo "Benchmark 3 (Packet drops of each load balancing step vs. load)"
    for thr in 40000 50000 60000 70000 80000 90000 100000; do
        for i in 0 1 2 3; do
            make clean

            make benchmark-throughput \
            CPUS="0-$NUM_CORES" \
            HEAT_UP_RATE=$thr \
            PACKET_SIZE=60 \
            FLOW_RULE_LEVELS=9 \
            BENCH_FLOW="bench$i" \
            $LB_ENG \
            >/dev/null 2>&1

            mv benchmark-throughput.log "$RESULTS/packet-drop-vs-load.log.$thr.$i"
        done
    done

    for thr in 40000 50000 60000 70000 80000 90000 100000; do
        for i in 0 1 2 3; do
            grep "rx_out_of_buffer" \
            "$RESULTS/packet-drop-vs-load.log.$thr.$i" | awk '{print var1, var2, $5}' var1=$thr var2=$i \
            >> "$RESULTS/packet-drop-vs-load.summary.out-of-buffer"

            grep "rx_discards_phy" \
            "$RESULTS/packet-drop-vs-load.log.$thr.$i" | awk '{print var1, var2, $5}' var1=$thr var2=$i \
            >> "$RESULTS/packet-drop-vs-load.summary.rx_discards_phy"
        done
    done
done


git checkout $CUR_BRANCH
# git stash pop