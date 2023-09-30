--[[                             !!! BEWARE !!!

This benchmark saturates a 10G link, i.e. can send ~14.8 millions of packets per second.
This leaves VERY little budget for anything besides sending packets.
If you make ANY changes, try sending packets at 10G and make sure the right amount of packets is sent!
]]--

local ffi     = require "ffi"
local device  = require "device"
local hist    = require "histogram"
local memory  = require "memory"
local mg      = require "moongen"
local stats   = require "stats"
local timer   = require "timer"
local ts      = require "timestamping"
local limiter = require "software-ratecontrol"
local libmoon = require "libmoon"

-- Default batch size (for intel)
local BATCH_SIZE = 64 -- packets
local RATE_MIN   = 0 -- Mbps
-- Default max rate (for intel)
local RATE_MAX   = 7000 -- Mbps

local HEATUP_DURATION = 5 -- seconds
local HEATUP_RATE     = 80 -- Mbps

local LATENCY_LOAD_RATE = 1000 -- Mbps
local N_PROBE_FLOWS     = 1000

local RESULTS_FILE_NAME = 'results.tsv'

local NIC_TYPE = 'Intel'


-- Arguments for the script
function configure(parser)
  parser:description("Generates UDP traffic and measures throughput.")
  parser:argument("type", "'latency' or 'throughput'.")
  parser:argument("layer", "Layer at which the flows are meaningful."):convert(tonumber)
  parser:argument("txDev", "Device to transmit from."):convert(tonumber)
  parser:argument("rxDev", "Device to receive from."):convert(tonumber)
  parser:argument("numThr", "Number of threads to use."):convert(tonumber)
  parser:argument("flowCount", "Number of flows in both directions"):convert(tonumber)
  parser:option("-p --packetsize", "Packet size."):convert(tonumber)
  parser:option("-d --duration", "Step duration."):convert(tonumber)
  parser:option("-x --reverse", "Number of flows for reverse traffic, if required."):default(0):convert(tonumber)
  parser:option("-r --heatuprate", "Heat-up rate."):convert(tonumber)
  parser:option("-n --nictype", "NIC type.")
  -- Arguments for LB exp
  parser:option("-s --speed", "average skew build up speed, unit: 10^10 pkts / sec^2."):convert(tonumber)
  parser:option("-w --extraload", "extra offered load on the core when skew finishes building up, unit: mpps."):convert(tonumber)
  parser:option("-f --freq", "frequency of skew events, unit: #skew events / sec."):convert(tonumber)
end

-- Per-layer functions to configure a packet given a counter;
-- this assumes the total number of flows is <= 65536
-- Layer "one" is for NAT, which touches the real-world layer 3,4
local packetConfigs = {
  -- LAN->WAN flows
  [0] = {
    [2] = function(pkt, counter)
      pkt.eth.src:set(counter)
      pkt.eth.dst:set(0xFF0000000000 + counter)
    end,
    [3] = function(pkt, counter)
      pkt.ip4.dst:set(counter)
    end,
    [4] = function(pkt, counter)
      -- Use moongen's internal function to get endianess right!
      pkt.udp:setDstPort(counter)
    end,
    [1] = function(pkt, counter)
      pkt.ip4.dst:set(counter)
    end
  },
  -- WAN->LAN flows
  [1] = {
    [2] = function(pkt, counter)
      pkt.eth.src:set(counter)
      pkt.eth.dst:set(0xFF0000000000 + counter)
    end,
    [3] = function(pkt, counter)
      pkt.ip4.src:set(counter)
    end,
    [4] = function(pkt, counter)
      -- Use moongen's internal function to get endianess right!
      pkt.udp:setSrcPort(counter)
    end,
    [1] = function(pkt, counter)
      pkt.ip4.src:set(counter)
      -- Try to minimize spoofing in NAT
      pkt.udp.dst = counter
    end
  }
}

-- Fills a packet with default values
-- Set the src to the max, so that dst can easily be set to
-- the counter if needed without overlap
function packetInit(buf, packetSize)
  buf:getUdpPacket():fill{
    ethSrc = "FF:FF:FF:FF:FF:FF",
    ethDst = "00:00:00:00:00:00",
    ip4Src = "0.0.0.0",
    ip4Dst = "0.0.0.0",
    udpSrc = 0,
    udpDst = 0,
    pktLength = packetSize
  }
end

-- Helper function, has to be global because it's started as a task
function _latencyTask(txQueue, rxQueue, layer, flowCount, duration, counterStart)
  -- Ensure that the throughput task is running
  mg.sleepMillis(1000)

  local timestamper = ts:newUdpTimestamper(txQueue, rxQueue)
  local hist = hist:new()
  local sendTimer = timer:new(duration - 1) -- we just slept for a second earlier, so deduce that
  local rateLimiter = timer:new(1 / flowCount) -- ASSUMPTION: The NF is running
  -- with 1 second expiry time, we want new flows' latency
  local counter = 0

  while sendTimer:running() and mg.running() do
    -- Minimum size for these packets is 84
    local packetSize = 84
    hist:update(timestamper:measureLatency(packetSize, function(buf)
      packetInit(buf, packetSize)
      packetConfigs[layer](buf:getUdpPacket(), counterStart + counter)
      counter = (counter + 1) % flowCount
    end))
    rateLimiter:wait()
    rateLimiter:reset()
  end
  
  return hist:median(), hist:standardDeviation()
end

-- Starts a latency-measuring task, which returns (median, stdev)
function startMeasureLatency(txQueue, rxQueue, layer,
                             flowCount, duration, counterStart)
  return mg.startTask("_latencyTask", txQueue, rxQueue,
                      layer, flowCount, duration, counterStart)
end

-- Generates excessive load to certain flow partitions
function _excessiveLoadTask(txQueue, rxQueue, layer, packetSize,
                            flowCount, duration, interArrCycle, batchSize)
  local mempool = memory.createMemPool(function(buf) packetInit(buf, packetSize) end)
  local bufs = mempool:bufArray(batchSize)
  local packetConfig = packetConfigs[layer]
  local sendTimer = timer:new(duration)

  -- offset here is essentially the index of the core to overload
  local offset = 0

  -- stay idle for 5 secs
  libmoon.sleepMillis(5000)

  local lastSendCycle = libmoon.getCycles()
  interArrCycle = interArrCycle * batchSize

  while sendTimer:running() and mg.running() do
    bufs:alloc(packetSize)
    for _, buf in ipairs(bufs) do
      packetConfig(buf:getUdpPacket(), offset)
      offset = offset + 12
      if offset > 256 then
        offset = 0
      end
    end

    bufs:offloadIPChecksums() -- UDP checksum is optional,
    -- let's do the least possible amount of work
    txQueue:send(bufs)

    local curr = libmoon.getCycles()
    while (curr - lastSendCycle) < interArrCycle do
      curr = libmoon.getCycles()
    end
    lastSendCycle = lastSendCycle + interArrCycle
  end

end

-- Helper function, has to be global because it's started as a task
function _throughputTask(txQueue, rxQueue, layer, packetSize,
                        flowCount, duration, interArrCycle, batchSize
                        )
  local mempool = memory.createMemPool(function(buf) packetInit(buf, packetSize) end)
  local bufs = mempool:bufArray(batchSize)
  local packetConfig = packetConfigs[layer]
  local sendTimer = timer:new(duration)
  local index = 0
  local offset = 0
  local lastSendCycle = libmoon.getCycles()
  interArrCycle = interArrCycle * batchSize

  -- local c = 0
  while sendTimer:running() and mg.running() do
    bufs:alloc(packetSize)
    for _, buf in ipairs(bufs) do
      packetConfig(buf:getUdpPacket(), index + offset)
      -- incAndWrap does this in a supposedly fast way;
      -- in practice it's actually slower!
      -- with incAndWrap this code cannot do 10G line rate
      -- counter = (counter + 1) % flowCount
      index = (index + 4096) % flowCount
      offset = (offset + 1) % 4096

      -- DEBUG: dump first 2 packets
      -- if c < 2 then
      --         buf:dump()
      --         c = c + 1
      -- end
    end

    bufs:offloadIPChecksums() -- UDP checksum is optional,
    -- let's do the least possible amount of work
    txQueue:send(bufs)

    local curr = libmoon.getCycles()
    while (curr - lastSendCycle) < interArrCycle do
      curr = libmoon.getCycles()
    end
    lastSendCycle = lastSendCycle + interArrCycle
  end

end

-- Helper function, has to be global because it's started as a task
function _excessiveLoadTaskIntel(txQueue, rxQueue, layer, packetSize, flowCount, duration,
                                 batchSize, extraLoad, speed, freq, direction)
  local mempool = memory.createMemPool(function(buf) packetInit(buf, packetSize) end)
  local bufs = mempool:bufArray(batchSize)
  local packetConfig = packetConfigs[direction][layer]
  local sendTimer = timer:new(duration)

  local tick = 0
  local counter = 0
  local udpPort

  -- skew build up time in number of pkts
  local skewBuildupTime = math.floor(100 * extraLoad * extraLoad / speed)
  local skewEventLength = math.floor(1000 * 1000 * extraLoad / freq)
  local startSkew = math.floor(1000 * 1000 * extraLoad)
  local endSkew = math.floor(1000 * 1000 * extraLoad * (duration - 1))

  -- STEP ONE
  -- send packets without skew for 1 sec
  while sendTimer:running() and mg.running() and (tick < startSkew) do
    bufs:alloc(packetSize)
    for _, buf in ipairs(bufs) do
      udpPort = counter % flowCount
      packetConfig(buf:getUdpPacket(), udpPort)
      counter = counter + 1
      tick = tick + 1
    end

    bufs:offloadIPChecksums() -- UDP checksum is optional,
    -- let's do the least possible amount of work
    txQueue:send(bufs)
  end

  -- STEP TWO
  -- start first skew event
  startSkew = tick

  local overloadedUdpPort = {}
  overloadedUdpPort[0] = math.random(flowCount)
  overloadedUdpPort[1] = math.random(flowCount)
  local overloadedUdpPortInd = 0

  local batchCnt = 0
  bufs:alloc(packetSize)
  while sendTimer:running() and mg.running() and (tick < skewEventLength + startSkew) do
    -- get udp port for current packet
    local val = math.random(skewBuildupTime)
    if tick > (startSkew + skewBuildupTime) then
      udpPort = overloadedUdpPort[overloadedUdpPortInd]
      overloadedUdpPortInd = 1 - overloadedUdpPortInd
    else 
      if (val > (tick - startSkew)) then
        udpPort = counter % flowCount
        counter = counter + 1
      else
        udpPort = overloadedUdpPort[overloadedUdpPortInd]
        overloadedUdpPortInd = 1 - overloadedUdpPortInd
      end
    end

    batchCnt = batchCnt + 1
    local buf = bufs[batchCnt]
    packetConfig(buf:getUdpPacket(), udpPort)

    -- send when batch is full
    if (batchCnt >= batchSize) then
      bufs:offloadIPChecksums() -- UDP checksum is optional,
      -- let's do the least possible amount of work
      txQueue:send(bufs)
      bufs:alloc(packetSize)
      batchCnt = 0
    end

    -- increment time tick
    tick = tick + 1
  end

  -- STEP THREE
  -- start more skew events
  local prevOverloadedUdpPort
  local prevOverloadedUdpPortInd

  while sendTimer:running() and mg.running() do
    -- launch another skew event
    if (tick >= (startSkew + skewEventLength)) then
      startSkew = tick
      prevOverloadedUdpPort = overloadedUdpPort
      prevOverloadedUdpPortInd = overloadedUdpPortInd
      overloadedUdpPort[0] = math.random(flowCount)
      overloadedUdpPort[1] = math.random(flowCount)
      overloadedUdpPortInd = 0
    end

    -- get udp port for current packet
    local val = math.random(skewBuildupTime)
    if tick > (startSkew + skewBuildupTime) then
      udpPort = overloadedUdpPort[overloadedUdpPortInd]
      overloadedUdpPortInd = 1 - overloadedUdpPortInd
    else 
      if (val > (tick - startSkew)) then
        udpPort = prevOverloadedUdpPort[prevOverloadedUdpPortInd]
        prevOverloadedUdpPortInd = 1 - prevOverloadedUdpPortInd
      else
        udpPort = overloadedUdpPort[overloadedUdpPortInd]
        overloadedUdpPortInd = 1 - overloadedUdpPortInd
      end
    end

    batchCnt = batchCnt + 1
    local buf = bufs[batchCnt]
    packetConfig(buf:getUdpPacket(), udpPort)

    -- send when batch is full
    if (batchCnt >= batchSize) then
      bufs:offloadIPChecksums() -- UDP checksum is optional,
      -- let's do the least possible amount of work
      txQueue:send(bufs)
      bufs:alloc(packetSize)
      batchCnt = 0
    end

    -- increment time tick
    tick = tick + 1

  end

end

-- Helper function, has to be global because it's started as a task
function _throughputTaskIntel(txQueue, rxQueue, layer, packetSize, flowCount, duration,
                              batchSize, direction)
  local mempool = memory.createMemPool(function(buf) packetInit(buf, packetSize) end)
  local bufs = mempool:bufArray(batchSize)
  local packetConfig = packetConfigs[direction][layer]
  local sendTimer = timer:new(duration)
  local counter = 0

  while sendTimer:running() and mg.running() do
    bufs:alloc(packetSize)
    for _, buf in ipairs(bufs) do
      packetConfig(buf:getUdpPacket(), counter)
      -- incAndWrap does this in a supposedly fast way;
      -- in practice it's actually slower!
      -- with incAndWrap this code cannot do 10G line rate
      counter = (counter + 1) % flowCount
    end

    bufs:offloadIPChecksums() -- UDP checksum is optional,
    -- let's do the least possible amount of work
    txQueue:send(bufs)
  end

end

-- Helper function, has to be global because it's started as a task
-- Generate 1-packet LAN->WAN short flows that trigger expiration
function _shortFlowTaskIntel(txQueue, rxQueue, layer, packetSize, shortFlowCount,
                             longFlowCount, duration, direction)
  local mempool = memory.createMemPool(function(buf) packetInit(buf, packetSize) end)
  local bufs = mempool:bufArray(1)
  local packetConfig = packetConfigs[direction][layer]
  local sendTimer = timer:new(duration)
  local counter = 0

  local rateLimiter = timer:new(2 / shortFlowCount) -- ASSUMPTION: < 2 sec expiration time

  while sendTimer:running() and mg.running() do
    bufs:alloc(packetSize)
    for _, buf in ipairs(bufs) do
      packetConfig(buf:getUdpPacket(), counter + longFlowCount)
      -- incAndWrap does this in a supposedly fast way;
      -- in practice it's actually slower!
      -- with incAndWrap this code cannot do 10G line rate
      counter = (counter + 1) % shortFlowCount
    end

    bufs:offloadIPChecksums() -- UDP checksum is optional,

    rateLimiter:wait()
    rateLimiter:reset()
    -- let's do the least possible amount of work
    txQueue:send(bufs)
  end

end

-- Starts a throughput-measuring task,
-- measuring throughput of the entire tx/rx NICs
-- which returns (#tx, #rx) packets (where rx == tx iff no loss)
function startMeasureThroughput(txQueue, rxQueue, txQueueRev, rxQueueRev, rate, layer,
                                packetSize, flowCount, duration,
                                speed, extraload, freq, reverse, shortFlowCount)
  -- Get the rate that should be given to MoonGen
  -- using packets of the given size to achieve the given true rate
  function moongenRate(packetSize, rate)
    -- The rate the user wants is in total Mbits/s
    -- But MoonGen will send it as if the packet size
    -- was packetsize+4 (the 4 is for the hardware-offloaded MAC CRC)
    -- when in fact there are 20 bytes of framing on top of that
    -- (preamble, start delimiter, interpacket gap)
    -- Thus we must find the "moongen rate"
    -- at which MoonGen will transmit at the true rate the user wants
    -- Easiest way to do that is to convert in packets-per-second
    -- Beware, we count packets in bytes and rate in bits so we need to convert!
    -- Also, MoonGen internally calls DPDK in which the rate is an uint16_t,
    -- let's avoid floats...
    -- Furthermore, it seems from tests that rates less than 10 are just ignored...
    --
    -- When we talk about packet rate or bit rates, G = 1000 * 1000 * 1000
    local byteRate = rate * 1000 * 1000 / 8
    local packetsPerSec = byteRate / (packetSize + 24)
    local moongenByteRate = packetsPerSec * (packetSize + 4)
    local moongenRate = moongenByteRate * 8 / (1000 * 1000)
    -- if moongenRate < 10 then
    --   printf("WARNING - Rate %f (corresponding to desired rate %d) too low," ..
    --            " will be set to 10 instead.", moongenRate, rate)
    --   moongenRate = 10
    -- end
    return math.floor(moongenRate)
  end

  -- temp hack, assume two tx dev on Intel exp, one on Mellanox exp (i.e., no reverse traffic)
  if NIC_TYPE ~= "Mellanox" then
    rate = rate / 2
  end

  -- Batch size for Mellanox
  --
  -- HACKY, don't remove before understanding
  --
  -- Use lower batch size whenever possible
  --
  -- This effectively reduces bust size
  --
  -- This is especially important for middlebox with
  -- lower core count when the udp bitmask has LSBs set
  --
  -- Only works if using 12 cores and Mellanox cx-5 100G NIC
  -- Can be ignored when using 10G 82599 NIC
  -- Hardware & System-dependent code
  local batchSize
  if rate <= 40000 then
    batchSize = 1
  elseif rate <= 48000 then
    batchSize = 2
  elseif rate <= 62000 then
    batchSize = 4
  elseif rate <= 90000 then
    batchSize = 8
  else
    batchSize = 16
  end

  if NIC_TYPE ~= "Mellanox" then
    batchSize = BATCH_SIZE
  end

  -- For non-Intel case only
  --
  -- Effectively disable rate limiting
  -- if targeting maximum rate
  if NIC_TYPE ~= "Intel" then
    if rate >= 100000 then
      rate = 10000000
    end
  end

  local numTxQueue = table.getn(txQueue) + 1
  function perQueuePacketRate(packetSize, rate, numQueues)
    local byteRate = rate / 8
    -- no rounding needed since the packetRate is only used
    -- in calculating inter-packet gap later
    return byteRate / (packetSize + 24) / numQueues
  end

  -- global tx, rx counter
  -- data retrived directly from NIC statistic registers
  local txCounter = stats:newDevTxCounter(txQueue[0], "plain")
  local rxCounter = stats:newDevRxCounter(rxQueue[0], "plain")
  local txCounterRev = stats:newDevTxCounter(txQueueRev[0], "plain")
  local rxCounterRev = stats:newDevRxCounter(rxQueueRev[0], "plain")

  -- Traffic LAN->WAN --

  -- inter arrival timer in ns
  local interArrTime = 1 / perQueuePacketRate(packetSize, rate, numTxQueue) * 1000
  local interArrCycle = interArrTime * (libmoon.getCyclesFrequency() / 1000000000)

  -- Convert extraLoad (mpps) to extraRate (mbps)
  local extraRate = (extraload / 1.487) * 1000 * (packetSize + 24) / 84;
  -- Moongen rate on the core that generates extra load to selected partitions
  local extraMoongenRate = moongenRate(packetSize, extraRate)
  -- per Queue moongen rate in Mbps
  local perQueueMoongenRate = (moongenRate(packetSize, rate) - extraMoongenRate) / (numTxQueue - 2)

  if perQueueMoongenRate >= 10 then

    for i = 0, numTxQueue - 3 do
    -- hw rate limiting used for Intel NICs
      if NIC_TYPE == "Intel" then
        txQueue[i]:setRate(perQueueMoongenRate)
        mg.startTask("_throughputTaskIntel", txQueue[i], rxQueue[i],
                   layer, packetSize, flowCount, duration, batchSize, 0)
    -- sw rate limiting used for Mellanox NIC
      else
        mg.startTaskOnCore(i + 1, "_throughputTask", txQueue[i], rxQueue[i],
                   layer, packetSize, flowCount, duration, interArrCycle, batchSize)
      end
    end
  end

  if shortFlowCount > 0 then
    if NIC_TYPE == "Intel" then
      mg.startTask("_shortFlowTaskIntel", txQueue[numTxQueue-2], rxQueue[numTxQueue-2],
                 layer, packetSize, shortFlowCount, flowCount, duration, 0)
    end
  end

  if extraload > 0 then
    if NIC_TYPE == "Intel" then
      txQueue[numTxQueue-1]:setRate(extraMoongenRate)
      mg.startTask("_excessiveLoadTaskIntel", txQueue[numTxQueue-1],
                     rxQueue[numTxQueue-1],
                     layer, packetSize, flowCount, duration, batchSize,
                     extraload, speed, freq, 0)
    else
      mg.startTaskOnCore(numTxQueue, "_excessiveLoadTask", txQueue[numTxQueue-1],
                     rxQueue[numTxQueue-1],
                     layer, packetSize, flowCount, duration, interArrCycle, batchSize)
    end
  end

  -- Traffic WAN->LAN --
  if reverse == true then
    if NIC_TYPE ~= "Mellanox" then
      local numTxQueueRev = table.getn(txQueueRev) + 1

      -- per Queue moongen rate in Mbps
      local perQueueMoongenRateRev = moongenRate(packetSize, rate) / (numTxQueueRev - 1)

      if perQueueMoongenRateRev >= 10 then
        for i = 0, numTxQueueRev - 2 do
          -- hw rate limiting used for Intel NICs
          txQueueRev[i]:setRate(perQueueMoongenRateRev)
          mg.startTask("_throughputTaskIntel", txQueueRev[i], rxQueueRev[i],
                     layer, packetSize, flowCount, duration, batchSize, 1)
        end
      end
    end
  end

  mg.waitForTasks()

  txCounter:finalize()
  rxCounter:finalize()
  txCounterRev:finalize()
  rxCounterRev:finalize()

  -- Return stats for the forwarding and reverse flows separately
  -- Reverse flows are likely to be dropped by the NF in the case of NAT since the traffic
  -- generator has no idea of how the NAT translates the packet headers and generates reverse
  -- packets that cause spoofing...
  return txCounter.total, rxCounter.total, txCounterRev.total, rxCounterRev.total
end


-- Heats up with packets at the given layer, with the given size and number of flows.
-- Errors if the loss is over 0.1%, and ignoreNoResponse is false.
function heatUp(txQueue, rxQueue, txQueueRev, rxQueueRev, layer, packetSize, flowCount, ignoreNoResponse)
  io.write("Heating up for " .. HEATUP_DURATION .. " seconds at " ..
             HEATUP_RATE .. " Mbps with " .. flowCount .. " flows... ")
  -- TODO: there should be an diff generator here... otherwise heatup can only run once at the beginning of
  -- the benchmark
  local tx, rx, txRev, rxRev = startMeasureThroughput(txQueue, rxQueue, txQueueRev, rxQueueRev, HEATUP_RATE,
                                        layer, packetSize, flowCount,
                                        HEATUP_DURATION, 1, 0, 10, false, 0)
  -- Ignore  stats for reverse flows, in the case of
  -- NAT they are likely dropped by the NF
  local loss = (tx - rx) / tx
  if loss > 0.001 and not ignoreNoResponse then
    io.write("Over 0.1% loss!\n")
    os.exit(1)
  end
  io.write("OK\n")

  return tx, rx, txRev, rxRev
end

-- iterator that diffs a series of values
-- TODO: match its API with txOnlyDiffGenerator
function diffGenerator(initTx, initRx)
  local prevTx, prevRx = initTx, initRx
  return function (tx, rx)
           local diffTx = tx - prevTx
           local diffRx = rx - prevRx
           prevTx, prevRx = tx, rx
           return diffTx, diffRx
         end
end

-- tx diff-only generator 
function txOnlyDiffGenerator(initTx, initRx, initTxRev, initRxRev)
  local prevTx, prevTxRev = initTx, initTxRev
  return function (tx, rx, txRev, rxRev)
           local diffTx, diffTxRev = tx - prevTx, txRev - prevTxRev
           prevTx, prevTxRev = tx, txRev
           return diffTx, rx, diffTxRev, rxRev
         end
end

-- Note!!! This function is not adapted to the new experiment on --
-- 100G Mellanox NIC --

-- Measure latency under 1G load
function measureLatencyUnderLoad(txDev, rxDev, layer,
                                 packetSize, duration, reverseFlowCount)
  -- It's the same filter set every time
  -- so not setting it on subsequent attempts is OK
  io.write("\n\n!!! IMPORTANT: You can safely ignore the warnings" ..
             " about setting an fdir filter !!!\n\n\n")

  -- Do not change the name and format of this file
  -- unless you change the rest of the scripts that depend on it!
  local outFile = io.open(RESULTS_FILE_NAME, "w")
  outFile:write("#flows\trate (Mbps)\tmedianLat (ns)\tstdevLat (ns)\n")

  -- Latency task waits 1sec for throughput task to have started, so we compensate
  duration = duration + 1

  local txThroughputQueue = txDev:getTxQueue(0)
  local rxThroughputQueue = rxDev:getRxQueue(0)
  local txReverseQueue = rxDev:getTxQueue(0) -- the rx/tx inversion is voluntary
  local rxReverseQueue = txDev:getRxQueue(0)
  local txLatencyQueue = txDev:getTxQueue(1)
  local rxLatencyQueue = rxDev:getRxQueue(1)

  for _, flowCount in ipairs({60000}) do
    if reverseFlowCount > 0 then
      heatUp(txReverseQueue, rxReverseQueue, layer,
             packetSize, reverseFlowCount, true)
    end
    heatUp(txThroughputQueue, rxThroughputQueue, layer,
           packetSize, flowCount, false)


    io.write("Measuring latency for " .. flowCount .. " flows... ")
    local throughputTask =
      startMeasureThroughput(txThroughputQueue, rxThroughputQueue,
                             LATENCY_LOAD_RATE, layer, packetSize,
                             flowCount, duration,
                             1, 0, 10, false)
    local latencyTask =
      startMeasureLatency(txLatencyQueue, rxLatencyQueue, layer,
                          N_PROBE_FLOWS, duration, flowCount)

    -- We may have been interrupted
    if not mg.running() then
      io.write("Interrupted\n")
      os.exit(0)
    end

		local tx, rx = throughputTask:wait()
		local median, stdev = latencyTask:wait()
		local loss = (tx - rx) / tx
		
		if loss > 0.001 then
      io.write("Too much loss!\n")
      outFile:write(flowCount .. "\t" .. "too much loss" .. "\n")
      break
    else
      io.write("median " .. median .. ", stdev " .. stdev .. "\n")
      outFile:write(flowCount .. "\t" .. LATENCY_LOAD_RATE .. "\t" ..
                      median .. "\t" .. stdev .. "\n")
    end
  end
end

-- Measure max throughput with less than 0.1% loss
function measureMaxThroughputWithLowLoss(txDev, rxDev, layer, packetSize,
                                         duration, reverseFlowCount, numThr,
                                         _flowCount,
                                         speed, extraload, freq)
  -- Do not change the name and format of this file
  -- unless you change the rest of the scripts that depend on it!
  local outFile = io.open(RESULTS_FILE_NAME, "w")
  outFile:write("#flows\tMbps\t#packets\t#pkts/s\tloss\n")

  local txQueue = {}
  local rxQueue = {}
  local txQueueRev = {} -- the rx/tx inversion is voluntary
  local rxQueueRev = {}
  for i = 0, numThr - 1 do
    txQueue[i] = txDev:getTxQueue(i)
    rxQueue[i] = rxDev:getRxQueue(i)
    txQueueRev[i] = rxDev:getTxQueue(i) -- the rx/tx inversion is voluntary
    rxQueueRev[i] = txDev:getRxQueue(i)
  end
  -- additional txQueue to generate LAN->WAN short flows
  -- to benchmark expiration
  txQueue[numThr] = txDev:getTxQueue(numThr) 
  rxQueue[numThr] = rxDev:getRxQueue(numThr) 
  
  -- For Mellanox Only
  --
  -- Workaround for the counter accumulating across call to
  -- startMeasureThroughput() bug
  local counterTrueVal
  if NIC_TYPE == "Mellanox" then
    counterTrueVal = diffGenerator(0, 0)
  -- For Intel case
  else
    counterTrueVal = txOnlyDiffGenerator(0, 0, 0, 0)
  end

  for _, flowCount in ipairs({_flowCount}) do
    counterTrueVal(heatUp(txQueue, rxQueue, txQueueRev, rxQueueRev, layer, packetSize, flowCount, false))

    -- temp hack
    -- os.exit(0)

    io.write("Running binary search with " .. flowCount .. " flows...\n")
    local upperBound = RATE_MAX
    local lowerBound = RATE_MIN
    local rate = upperBound
    local bestRate = 0
    local bestTx = 0
    local bestLoss = 1

    local nBsIters
    if NIC_TYPE == "Mellanox" then
      nBsIters = 13
    else
      nBsIters = 1
    end

    -- Binary search phase
    for i = 1, nBsIters do
      io.write("Step " .. i .. ": " .. rate .. " Mbps... ")
      local tx, rx, txRev, rxRev

      -- For Mellanox Only
      --
      -- Sometimes the rate of traffic generated by moongen deviates a lot
      -- from the desired rate
      -- repeat the current round until the error between the desired and
      -- generated rate is < 1%
      local gen_rate
      local times_repeated = 0
      repeat 
        times_repeated = times_repeated + 1
        tx, rx, txRev, rxRev = counterTrueVal(startMeasureThroughput(txQueue, rxQueue, txQueueRev, rxQueueRev, rate,
                                              layer, packetSize, flowCount,
                                              duration, speed, extraload, freq, true, 1024))
        tx = tx + txRev
        rx = rx + rxRev
        gen_rate = (tx / duration) * (packetSize + 24) * 8 / 1000 / 1000         
        if times_repeated > 1 then
          io.write("Repeating due to moongen's inaccuracy in rate control")
        end
      until (gen_rate / rate) >= 0.99 or NIC_TYPE ~= "Mellanox"
        if times_repeated > 1 then
          io.write("Repeation done!")
        end

      -- We may have been interrupted
      if not mg.running() then
        io.write("Interrupted\n")
        os.exit(0)
      end

      local loss = (tx - rx) / tx
      io.write(tx .. " sent, " .. rx .. " received, loss = " .. loss .. "\n")

      -- Update the initial rate upperbound
      -- Moongen may not be able to generate traffic rate of RATE_MAX
      if (i == 1) then
        rate = (tx / duration) * (packetSize + 24) * 8 / 1000 / 1000
        upperBound = rate
      end

      if (loss < 0.001) then
        bestRate = rate
        bestTx = tx
        bestLoss = loss

        lowerBound = rate
        rate = rate + (upperBound - rate)/2
      else
        upperBound = rate
        rate = lowerBound + (rate - lowerBound)/2
      end

      -- Stop if the first step is already successful,
      -- let's not do pointless iterations
      if (i == nBsIters) or (loss < 0.001 and bestRate == upperBound) then
        -- Note that we write 'bestRate' here,
        -- i.e. the last rate with < 0.001 loss, not the current one
        -- (which may cause > 0.001 loss
        --  since our binary search is bounded in steps)
        outFile:write(flowCount .. "\t" ..
            math.floor(bestRate) .. "\t" ..
            bestTx .. "\t" ..
            math.floor(bestTx/duration) .. "\t" ..
            bestLoss .. "\n")
        break
      end
    end
  end
end

function master(args)
  -- ignore the latency part for now
  -- additional txQueue to generate LAN->WAN short flows
  -- to benchmark expiration
  local txDev = device.config{port = args.txDev, rxQueues = args.numThr, txQueues = args.numThr + 1}
  local rxDev = device.config{port = args.rxDev, rxQueues = args.numThr + 1, txQueues = args.numThr}
  device.waitForLinks()

  measureFunc = nil
  if args.type == 'latency' then
    measureFunc = measureLatencyUnderLoad
  elseif args.type == 'throughput' then
    measureFunc = measureMaxThroughputWithLowLoss
  else
    print("Unknown type.")
    os.exit(1)
  end

  if args.heatuprate ~= nil then
    RATE_MAX = args.heatuprate
  end

  if args.nictype ~= nil then
    NIC_TYPE = args.nictype
  end

  if NIC_TYPE == "Mellanox" then
    RATE_MAX = 100000
  end

  if args.speed == nil then
    args.speed = 1
  end

  if args.extraload == nil then
    args.extraload = 0
  end

  if args.freq == nil then
    args.freq = 10
  end

  measureFunc(txDev, rxDev, args.layer, args.packetsize,
              args.duration, args.reverse, args.numThr,
              args.flowCount,
              args.speed, args.extraload, args.freq)
end
