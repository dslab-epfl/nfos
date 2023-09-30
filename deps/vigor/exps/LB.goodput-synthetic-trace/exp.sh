#!/bin/bash
function search_max_goodput {
    local upper_bound=20000
    local lower_bound=0
    local rate=$upper_bound
    local best_rate=$lower_bound
   
    for iter in {1..11}; do
        make benchmark-throughput RATE_MAX=$rate SPEED=$1 EXTRA_LOAD=$2 FREQ=$3 FLOW_CNT=$4 CPUS=8-12 2>&1 >/dev/null

        local recv_port0=$(grep ipackets: benchmark-throughput.log | cut -d' ' -f2 | head -n1)
        local recv_port1=$(grep ipackets: benchmark-throughput.log | cut -d' ' -f2 | tail -n1)
        local recv=$(( recv_port0 + recv_port1 ))

        local loss="0"
        for cnter in imissed ierrors rx_nombuf; do
            local cnter_port0=$(grep $cnter benchmark-throughput.log | cut -d' ' -f2 | head -n1)
            local cnter_port1=$(grep $cnter benchmark-throughput.log | cut -d' ' -f2 | tail -n1)
            local cnter=$(( cnter_port0 + cnter_port1 ))
            loss=$(( loss + cnter ))
        done

        # disable ring_loss monitoring by default
        #local ring_loss=$(grep num_pkts_dropped benchmark-throughput.log | cut -d' ' -f2)
        #loss=$(( loss + ring_loss ))
        #recv=$(( recv - ring_loss ))

        local sent=$(( loss + recv ))

        local loss_rate=$(echo "print($loss/$sent)" | python3)

        echo "$rate $sent $recv $loss $loss_rate"

        if [[ $loss == "0" ]]; then
            best_rate=$rate
            lower_bound=$rate
            rate=$(( (upper_bound + lower_bound) / 2 ))
        else
            upper_bound=$rate
            rate=$(( (upper_bound + lower_bound) / 2 ))
        fi

        if [[ $loss == "0" ]]; then
            if [[ $best_rate == $upper_bound ]]; then
                break
            fi
        fi

    done

    echo "$1 $2 $3 $4 $best_rate"
    echo "$1 $2 $3 $4 $best_rate" >> $5
}

# only support vignat & vignop
NF=$1
EXTRA_LOADS=$2
SPEEDS=$3

# recreate results directory
RESULT_FOLDER="$PWD/$NF"
rm -rf "$RESULT_FOLDER"
mkdir "$RESULT_FOLDER"

# bench
for algo in algo1 algo2; do
    res="$RESULT_FOLDER/$algo"
    touch $res

    if [[ $algo == "algo2" ]]; then
        cd $HOME/vigor
        git apply patches/algo2
    fi
    cd $HOME/vigor/$NF

    for extra_load in $EXTRA_LOADS; do
        for speed in $SPEEDS; do
            search_max_goodput $speed $extra_load 100 32768 $res
        done
    done
done

# clean up
cd $HOME/vigor
git checkout nf-lb nf.c