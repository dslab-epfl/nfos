1. Concurrent test

(tester):$ bash moongen-wrapper-mlx5.sh "sudo moon-gen/build/MoonGen concurrent-test.lua throughput 3 0 2 2 10 -p 60 -d 10 -r 100000 -n Mellanox"

Scenario sum-ups:

- Concurrent flow insertion/deletion/lookup => concurrent pkt set manager & tuple de-/allocation

scenario one (frequent flow insertion/deletion/lookup, no reverse flows): 

- Gen 10 internal flows for 5 secs, at 800mbps. Flows differ in dst IP. Flow expiration time 0 secs.
  , 2M flow table size

- Two dataplane core + one periodic handler core

- MB cflags: --O3 --DENABLE_LOG 

TODO: scenario one with reverse flows