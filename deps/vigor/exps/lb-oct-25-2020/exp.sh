#!/bin/bash
cd ~/vigor/vignat

function search_max_goodput {
    local upper_bound=10000
    local lower_bound=0
    local rate=$upper_bound
    local best_rate=$lower_bound
   
    for iter in {1..10}; do
        local loss=$(make benchmark-throughput RATE_MAX=$rate SPEED=$1 EXTRA_LOAD=$2 FREQ=$3 2>&1 | grep loss | cut -d'=' -f2 | cut -d' ' -f2)

        echo "$rate $loss"

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

    echo "$1 $2 $3 $best_rate"
    echo "$1 $2 $3 $best_rate" >> $4
}

RESULT_FOLDER="$HOME/vigor/exps/lb-oct-25-2020/vignat"

# bench two
BENCH="bench2"
rm -f "$RESULT_FOLDER/$BENCH"
touch "$RESULT_FOLDER/$BENCH"
for freq in 1 10 100 1000 10000; do
    search_max_goodput 1 0.5 $freq "$RESULT_FOLDER/$BENCH"
done

# bench one
BENCH="bench1"
rm -f "$RESULT_FOLDER/$BENCH"
touch "$RESULT_FOLDER/$BENCH"
for extra_load in 0 0.5 1.0 1.5 2.0 2.5; do
    for speed in 0.1 2 4 6; do
        search_max_goodput $speed $extra_load 1000 "$RESULT_FOLDER/$BENCH"
    done
done
