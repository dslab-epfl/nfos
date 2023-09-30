#!/bin/bash
cd $HOME/vigor/vignop

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

RESULT_FOLDER="$HOME/vigor/exps/lb-nov-29-2020/vignop"
rm -rf "$RESULT_FOLDER"
mkdir "$RESULT_FOLDER"

# bench two
BENCH="bench2"
touch "$RESULT_FOLDER/$BENCH"
for freq in 500 1000 2000; do
    search_max_goodput 3 2 $freq 256 "$RESULT_FOLDER/$BENCH"
    search_max_goodput 3 2 $freq 32768 "$RESULT_FOLDER/$BENCH"
done

# bench one
BENCH="bench1"
touch "$RESULT_FOLDER/$BENCH"
for extra_load in 1.0 2.0 4.0; do
    for speed in 0.1 3 6; do
        search_max_goodput $speed $extra_load 100 32768 "$RESULT_FOLDER/$BENCH"
    done
done
