1. Concurrent test

Scenario sum-ups:

(1) Concurrent flow insertion/lookup, flow table/tuple allocator capacity > max. #active flows, no reverse flow

scenario one setup:

- (tester):$ sudo moon-gen/build/MoonGen concurrent-test.lua throughput 3 0 1 2 40 -p 60 -d 10 -r 100000 -n Mellanox
- (middlebox):$ make run-debug-log LCORES="8,10,12" EXP_TIME=20000000

- Gen 40 internal flows for 5 secs, at 800mbps. Flows differ in src IP. Flow expiration time 20 secs,
  8M flow table size (default), 8M available tuples (default)

- Flows span 10 users, 2 session per user, and 2 flows per session.

- Two dataplane core + one periodic handler core
