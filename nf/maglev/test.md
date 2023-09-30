1. Single-thread test

(tester):$ bash moongen-wrapper-mlx5.sh "sudo moon-gen/build/MoonGen test.lua throughput 3 0 1 -b 2,3 2 4 -p 60 -d 10 -r 100000 -n Mellanox"

Gen heartbeats every two seconds,
Gen 4 flows 1 sec after first heartbeats for 5 secs, at heatup rate (10s of mbps). Flows differ in src IP.


2. Concurrent test

(tester):$ bash moongen-wrapper-mlx5.sh "sudo moon-gen/build/MoonGen concurrent-test.lua throughput 3 0 1 -b 2,3 2 4 -p 60 -d 10 -r 100000 -n Mellanox"

Scenario sum-ups:

- Concurrent flow insertion/deletion/lookup => concurrent pkt set manager
- Concurrent flow ops + backend expiration/insertion/refreshing => RCU (sync between cpcs and rest)

scenario one (frequent backend expiration/insertion): 

- Two backends

- Gen heartbeats for both backends every 20 usecs, backend expiration time 0, backend expiration check frequency 10 usecs.

- Gen 4 flows 100 msec after first heartbeats for 5 secs, at 800mbps. Flows differ in dst IP. Flow expiration time 10 secs.

- Two dataplane core + one periodic handler core

- MB cflags: --O3 --DENABLE_LOG

scenario two (frequent flow insertion/deletion/lookup): 

- Two backends

- Gen heartbeats for both backends every 20 usecs, backend expiration time 10secs, backend expiration check frequency 10 usecs.

- Gen 4 flows 100 msec after first heartbeats for 5 secs, at 800mbps. Flows differ in dst IP. Flow expiration time 0 secs.

- Two dataplane core + one periodic handler core

- MB cflags: --O3 --DENABLE_LOG 