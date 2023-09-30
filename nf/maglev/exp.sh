#!/bin/bash
function search_max_goodput {
    # upper_bound trace acceleration factor, change it for different traces
    local upper_bound=6
    local lower_bound=0
    local rate=$upper_bound
    local best_rate=$lower_bound
    local best_recv=0
    local target_loss="0.03"
   
    for iter in {1..10}; do
        bash ../../bench/bench.sh 8,$1,2 maglev "true $rate /home/lei/traces/caida/trace03-tcpudp.pcap,/home/lei/traces/caida/trace18-tcpudp.pcap,/home/lei/traces/caida/trace33-tcpudp.pcap,/home/lei/traces/caida/trace48-tcpudp.pcap,/home/lei/traces/caida/trace10-tcpudp.pcap,/home/lei/traces/caida/trace25-tcpudp.pcap,/home/lei/traces/caida/trace40-tcpudp.pcap,/home/lei/traces/caida/trace55-tcpudp.pcap"

        local loss=$(grep loss benchmark.result | cut -d ' ' -f9)

        local recv=$(grep pkts benchmark.result | cut -d ' ' -f1)

        echo "$loss $recv $rate"

        if [[ $(python3 -c "print($loss < $target_loss)") == "True" ]]; then
            best_rate=$rate
            best_recv=$recv
            lower_bound=$rate
        else
            upper_bound=$rate
        fi
        rate=$(python3 -c "print(($upper_bound + $lower_bound)/2)")

        if [[ $(python3 -c "print($loss < $target_loss)") == "True" ]]; then
            if [[ $best_rate == $upper_bound ]]; then
                break
            fi
        fi

    done

    echo "$best_rate $(python3 -c "print($best_recv / 10 / 1000 / 1000)")"

}

for i in 1 2 4 8 12 16; do
    search_max_goodput $i
    echo "#cores: $i"
done
