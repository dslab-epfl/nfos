scp concurrent-test.lua icdslab4.epfl.ch:~/

# mac table size 65536, exp time 1.2 sec or 0
# periodic handler period 10usec
# flow size 100, heat up rate
sudo moon-gen/build/MoonGen concurrent-test.lua throughput 2 0 2 2 100 -p 60 -d 10 -r 100000 -n Mellanox

