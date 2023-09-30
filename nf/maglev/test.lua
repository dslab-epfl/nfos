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

-- Default batch size
local BATCH_SIZE = 1 -- packets
local RATE_MIN   = 0 -- Mbps
local RATE_MAX

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
  parser:option("-r --rate", "Max rate."):convert(tonumber)
  parser:option("-n --nictype", "NIC type.")
  -- Arguments for LB exp
  parser:option("-s --speed", "average skew build up speed, unit: 10^10 pkts / sec^2."):convert(tonumber)
  parser:option("-w --extraload", "extra offered load on the core when skew finishes building up, unit: mpps."):convert(tonumber)
  parser:option("-f --freq", "frequency of skew events, unit: #skew events / sec."):convert(tonumber)
  -- Arguments for maglev
  parser:option("-b --backends", "backend devices. Format: <backend1,backend2,...>")
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
      pkt.ip4.src:set(counter)
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
    -- Don't use broadcast MAC addr here since some NICs could
    -- block it by default even when set as promiscous...
    ethSrc = "FE:FF:FF:FF:FF:FF",
    ethDst = "00:00:00:00:00:00",
    ip4Src = "0.0.0.0",
    ip4Dst = "0.0.0.0",
    udpSrc = 0,
    udpDst = 0,
    pktLength = packetSize
  }
end

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
  return math.floor(moongenRate)
end

function getInterArrCycle(packetSize, batchSize, rate)
  local packetRate = rate / (packetSize + 24) / 8
  local interArrTime = 1 / packetRate * 1000
  local interArrCycle = batchSize * interArrTime * (libmoon.getCyclesFrequency() / 1000000000)
  return interArrCycle
end

function rateToBatchSize(hwRateLimitingSupport, rate)
  local batchSize

  if hwRateLimitingSupport == true then
    batchSize = BATCH_SIZE
  else
    -- HACKY, don't remove before understanding.
    -- Use lower batch size whenever possible.
    -- This effectively reduces burst size.
    -- Only works if using Mellanox cx-5 100G NIC.
    -- Hardware & System-dependent code.
    if rate <= 3000 then
      batchSize = 1
    elseif rate <= 4000 then
      batchSize = 2
    elseif rate <= 5000 then
      batchSize = 4
    elseif rate <= 7000 then
      batchSize = 8
    else
      batchSize = 16
    end
  end

  return batchSize
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

-- Helper function, has to be global because it's started as a task
function _excessiveLoadTask(txQueue, rxQueue, txQueueRev, rxQueueRev,
                            layer, packetSize, flowCount, duration,
                            hwRateLimitingSupport,
                            extraLoad, speed, freq)

  -- Traffic rate (Mbps) generated by the excessive load task
  local extraRate = extraload * (packetSize + 24) * 8
  local batchSize = rateToBatchSize(hwRateLimitingSupport, extraRate / 2)

  if hwRateLimitingSupport == true then
    txQueue:setRate(moongenRate(packetSize, extraRate / 2))
    txQueueRev:setRate(moongenRate(packetSize, extraRate / 2))
  else
    io.write("WARNING: No support to hw rate limiting, not sending skewed load\n")
    return
  end
  

  local mempool = {}
  local bufs
  local bufsRev
  local packetConfig
  local packetConfigRev
  for i = 0, 1 do
    mempool[i] = memory.createMemPool(function(buf) packetInit(buf, packetSize) end)
  end

  bufs = mempool[0]:bufArray(batchSize)
  bufsRev = mempool[1]:bufArray(batchSize)
  packetConfig = packetConfigs[0][layer]
  packetConfigRev = packetConfigs[1][layer]

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
    bufsRev:alloc(packetSize)

    for k = 1, batchSize do
      udpPort = counter % flowCount
      packetConfig(bufs[k]:getUdpPacket(), udpPort)
      packetConfigRev(bufsRev[k]:getUdpPacket(), udpPort)
      counter = counter + 1
      tick = tick + 2
    end

    bufs:offloadIPChecksums() -- UDP checksum is optional,
    bufsRev:offloadIPChecksums() -- UDP checksum is optional,
    -- let's do the least possible amount of work
    txQueue:send(bufs)
    txQueueRev:send(bufsRev)
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
  bufsRev:alloc(packetSize)
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
    packetConfig(bufs[batchCnt]:getUdpPacket(), udpPort)
    packetConfigRev(bufsRev[batchCnt]:getUdpPacket(), udpPort)

    -- send when batch is full
    if (batchCnt >= batchSize) then
      bufs:offloadIPChecksums() -- UDP checksum is optional,
      bufsRev:offloadIPChecksums() -- UDP checksum is optional,

      -- let's do the least possible amount of work
      txQueue:send(bufs)
      txQueueRev:send(bufsRev)

      bufs:alloc(packetSize)
      bufsRev:alloc(packetSize)

      batchCnt = 0
    end

    -- increment time tick
    tick = tick + 2
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
    packetConfig(bufs[batchCnt]:getUdpPacket(), udpPort)
    packetConfigRev(bufsRev[batchCnt]:getUdpPacket(), udpPort)

    -- send when batch is full
    if (batchCnt >= batchSize) then
      bufs:offloadIPChecksums() -- UDP checksum is optional,
      bufsRev:offloadIPChecksums() -- UDP checksum is optional,

      -- let's do the least possible amount of work
      txQueue:send(bufs)
      txQueueRev:send(bufsRev)

      bufs:alloc(packetSize)
      bufsRev:alloc(packetSize)

      batchCnt = 0
    end

    -- increment time tick
    tick = tick + 2

  end

end

-- Helper function, has to be global because it's started as a task
function _throughputTask(txQueue, rxQueue, layer, packetSize, flowCount, duration,
                         direction, hwRateLimitingSupport, rate)

  local curr
  local interArrCycle
  local lastSendCycle
  local batchSize = rateToBatchSize(hwRateLimitingSupport, rate)

  local mempool = memory.createMemPool(function(buf) packetInit(buf, packetSize) end)
  local bufs = mempool:bufArray(batchSize)
  local packetConfig = packetConfigs[direction][layer]
  local sendTimer = timer:new(duration)

  if hwRateLimitingSupport == true then
    txQueue:setRate(moongenRate(packetSize, rate))
  else
    interArrCycle = getInterArrCycle(packetSize, batchSize, rate)
    lastSendCycle = libmoon.getCycles()
  end
  local counter = 0

  while sendTimer:running() and mg.running() do
    bufs:alloc(packetSize)
    for _, buf in ipairs(bufs) do
      packetConfig(buf:getUdpPacket(), counter)
      -- incAndWrap does this in a supposedly fast way;
      -- in practice it's actually slower!
      -- with incAndWrap this code cannot do 10G line rate
      counter = (counter + 1) % flowCount
      -- Debug
      io.write("CLIENT PACKET\n")
      buf:dump()
    end

    bufs:offloadIPChecksums() -- UDP checksum is optional,
    -- let's do the least possible amount of work
    txQueue:send(bufs)

    if hwRateLimitingSupport == false then
      curr = libmoon.getCycles()
      while (curr - lastSendCycle) < interArrCycle do
        curr = libmoon.getCycles()
      end
      -- this ensures that moongen sends a batch of packets every interArrCycle
      lastSendCycle = lastSendCycle + interArrCycle
    end

    -- Debug
    if counter > 10 then
      return
    end

  end

end

-- Helper function, has to be global because it's started as a task
-- Generate 1-packet LAN->WAN short flows that trigger expiration
function _shortFlowTask(txQueue, rxQueue, layer, packetSize, shortFlowCount,
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

-- Helper function, has to be global because it's started as a task
-- Generates heartbeats
function _heartbeatTask(backendDevs, duration)
  local packetSize = 60
  local mempool = {}
  local bufs = {}
  local txQueues = {}
  for i = 0, table.getn(backendDevs) do
    mempool[i] = memory.createMemPool(function(buf) packetInit(buf, packetSize) end)
    bufs[i] = mempool[i]:bufArray(1)
    txQueues[i] = backendDevs[i]:getTxQueue(0)
  end


  local sendTimer = timer:new(duration)
  local rateLimiter = timer:new(2) -- Debug: one heartbeat from each backend every 2 sec

  while sendTimer:running() and mg.running() do
    for k = 0, table.getn(backendDevs) do
      bufs[k]:alloc(packetSize)
      for _, buf in ipairs(bufs[k]) do
        pkt = buf:getUdpPacket()
        pkt.eth.src:set(0xFE0000000000 + k)
        pkt.ip4.src:set(256 + k)
        -- debug
        io.write("HEARTBEAT PACKET\n")
        buf:dump()
      end
      bufs[k]:offloadIPChecksums() -- UDP checksum is optional,
    end

    -- let's do the least possible amount of work
    for k = 0, table.getn(backendDevs) do
      txQueues[k]:send(bufs[k])
    end
    rateLimiter:wait()
    rateLimiter:reset()
  end
end

-- Starts a throughput-measuring task,
-- measuring throughput of the entire tx/rx NICs
-- which returns (#tx, #rx) packets (where rx == tx iff no loss)
function startMeasureThroughput(txQueue, rxQueue, txQueueRev, rxQueueRev, rate, layer,
                                packetSize, flowCount, duration,
                                speed, extraload, freq, reverse, shortFlowCount, backendDevs)
  -- 1. Stats
  -- global tx, rx counter
  -- data retrived directly from NIC statistic registers
  local txCounter = stats:newDevTxCounter(txQueue[0], "plain")
  local rxCounter = stats:newDevRxCounter(rxQueue[0], "plain")
  local txCounterRev = stats:newDevTxCounter(txQueueRev[0], "plain")
  local rxCounterRev = stats:newDevRxCounter(rxQueueRev[0], "plain")

  local numTxQueue = table.getn(txQueue) + 1
  local numTxQueueRev = table.getn(txQueueRev) + 1

  -- 2. Rate limiting
  -- Only Intel 82599 supports hw rate limiting
  local hwRateLimitingSupport
  if NIC_TYPE == "Intel" then
    hwRateLimitingSupport = true
  else
    hwRateLimitingSupport = false
  end

  -- 3. Rate distribution
  -- Same total traffic rate (Mbps) in both directions (not including the short flows task)
  if reverse == true then
    rate = rate / 2
  end
  -- Traffic rate (Mbps) generated by the excessive load task
  local extraRate = extraload * (packetSize + 24) * 8
  -- Traffic rate (Mbps) generated by all the tasks excluding shortFlow & excessiveLoad tasks
  -- LAN->WAN
  local perQueueRate = (rate - (extraRate / 2)) / (numTxQueue - 2)
  -- WAN->LAN
  local perQueueRateRev = (rate - (extraRate / 2)) / (numTxQueueRev - 1)

  -- 4. Tasks
  if backendDevs ~= nil then
    mg.startTask("_heartbeatTask", backendDevs, duration)
    mg.sleepMillis(1000)
  end

  if shortFlowCount > 0 then
    mg.startTask("_shortFlowTask", txQueue[numTxQueue-2], rxQueue[numTxQueue-2],
                 layer, packetSize, shortFlowCount, flowCount, duration, 0)
  end

  if extraload > 0 then
    mg.startTask("_excessiveLoadTask", txQueue[numTxQueue-1],
                   rxQueue[numTxQueue-1],
                   txQueueRev[numTxQueueRev-1], rxQueueRev[numTxQueueRev-1],
                   layer, packetSize, flowCount, duration,
                   hwRateLimitingSupport,
                   extraload, speed, freq)
  end

  if moongenRate(packetSize, perQueueRate) >= 10 then
    for i = 0, numTxQueue - 3 do
      mg.startTask("_throughputTask", txQueue[i], rxQueue[i],
                   layer, packetSize, flowCount, duration, 0,
                   hwRateLimitingSupport, perQueueRate)
    end
  else
    io.write("rate limiting may not work with < 10mbps rate per queue\n")
  end

  if reverse == true then
    if moongenRate(packetSize, perQueueRateRev) >= 10 then
      for i = 0, numTxQueueRev - 2 do
        mg.startTask("_throughputTask", txQueueRev[i], rxQueueRev[i],
                   layer, packetSize, flowCount, duration, batchSize, 1,
                   hwRateLimitingSupport, perQueueRateRev)
      end
    else
      io.write("rate limiting may not work with < 10mbps rate per queue\n")
    end
  end

  mg.waitForTasks()

  -- 5. Collecting stats
  txCounter:finalize()
  rxCounter:finalize()
  txCounterRev:finalize()
  rxCounterRev:finalize()

  -- Return stats for the forwarding and reverse flows separately
  -- Reverse flows are likely to be dropped by the NF in the case of NAT since the traffic
  -- generator has no idea of how the NAT translates the packet headers and generates reverse
  -- packets that cause spoofing...
  -- masking wrong reverse stats when reverse is not set
  if reverse == false then
    txCounterRev.total = 0
    rxCounterRev.total = 0
  end
  return txCounter.total, rxCounter.total, txCounterRev.total, rxCounterRev.total
end


-- Heats up with packets at the given layer, with the given size and number of flows.
-- Errors if the loss is over 0.1%, and ignoreNoResponse is false.
function heatUp(txQueue, rxQueue, txQueueRev, rxQueueRev, layer, packetSize, flowCount, ignoreNoResponse,
                backendDevs)
  io.write("Heating up for " .. HEATUP_DURATION .. " seconds at " ..
             HEATUP_RATE .. " Mbps with " .. flowCount .. " flows... ")
  -- TODO: there should be an diff generator here... otherwise heatup can only run once at the beginning of
  -- the benchmark
  local tx, rx, txRev, rxRev = startMeasureThroughput(txQueue, rxQueue, txQueueRev, rxQueueRev, HEATUP_RATE,
                                        layer, packetSize, flowCount,
                                        HEATUP_DURATION, 1, 0, 10, false, 0,
                                        backendDevs)
  -- Ignore  stats for reverse flows, in the case of
  -- NAT they are likely dropped by the NF

  -- Disable this check for now since moongen's counter is problematic, use sysfiles instead
  -- local loss = (tx - rx) / tx
  -- if loss > 0.001 and not ignoreNoResponse then
  --   io.write("Over 0.1% loss!\n")
  --   os.exit(1)
  -- end
  -- io.write("OK\n")

  return tx, rx, txRev, rxRev
end

-- iterator that diffs a series of values
function diffGenerator(initTx, initRx, initTxRev, initRxRev)
  local prevTx, prevRx, prevTxRev, prevRxRev = initTx, initRx, initTxRev, initRxRev
  return function (tx, rx, txRev, rxRev)
           local diffTx = tx - prevTx
           local diffRx = rx - prevRx
           local diffTxRev = txRev - prevTxRev
           local diffRxRev = rxRev - prevRxRev
           prevTx, prevRx, prevTxRev, prevRxRev = tx, rx, txRev, rxRev
           return diffTx, diffRx, diffTxRev, diffRxRev
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
                                         speed, extraload, freq,
                                        backendDevs)
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
  
  -- Workaround for the counter accumulating across call to
  -- startMeasureThroughput() bug
  local counterTrueVal
  -- Mellanox ConnectX-5
  if NIC_TYPE == "Mellanox" then
    counterTrueVal = diffGenerator(0, 0, 0, 0)
  -- For Intel 82599 case
  else
    counterTrueVal = txOnlyDiffGenerator(0, 0, 0, 0)
  end

  for _, flowCount in ipairs({_flowCount}) do
    counterTrueVal(heatUp(txQueue, rxQueue, txQueueRev, rxQueueRev, layer, packetSize, flowCount, false, backendDevs))

    -- Debug
    os.exit(0)

    local rate = RATE_MAX
    io.write("Measuring goodput with " .. flowCount .. " flows (Offered load " .. rate .. " mpps)\n")

    local tx, rx, txRev, rxRev
    tx, rx, txRev, rxRev = counterTrueVal(startMeasureThroughput(txQueue, rxQueue, txQueueRev, rxQueueRev, rate,
                                          layer, packetSize, flowCount,
                                          duration, speed, extraload, freq, false, 0,
                                          backendDevs))
    tx = tx + txRev
    rx = rx + rxRev
    local loss = (tx - rx) / tx

    -- Disable for now since moongen's counter is problematic, use sysfiles instead
    -- io.write(tx .. " sent, " .. rx .. " received, loss = " .. loss .. "\n")

  end
end

function master(args)
  -- ignore the latency part for now
  -- additional txQueue to generate LAN->WAN short flows
  -- to benchmark expiration
  local txDev = device.config{port = args.txDev, rxQueues = args.numThr, txQueues = args.numThr + 1}
  local rxDev = device.config{port = args.rxDev, rxQueues = args.numThr + 1, txQueues = args.numThr}

  -- Get backends
  local backendDevs = {}
  local i = 0
  if args.backends ~= nil then
    for w in string.gmatch(args.backends, "%d+") do
      backendDevs[i] = device.config{port = tonumber(w), rxQueues = 1, txQueues = 1}
      i = i + 1
    end 
  end
  
  device.waitForLinks()

  measureFunc = nil
  if args.type == 'latency' then
    measureFunc = measureLatencyUnderLoad
    print("Unsupported type.")
    os.exit(1)
  elseif args.type == 'throughput' then
    measureFunc = measureMaxThroughputWithLowLoss
  else
    print("Unknown type.")
    os.exit(1)
  end

  if args.nictype ~= nil then
    NIC_TYPE = args.nictype
  end

  if NIC_TYPE == "Mellanox" then
    RATE_MAX = 100000 -- Mbps
  else
    RATE_MAX = 7000 -- Mbps
  end
  if args.rate ~= nil then
    RATE_MAX = args.rate
  end

  -- set per queue heatup rate as 40 Mbps
  HEATUP_RATE = 40 * 2 * (args.numThr - 1)

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
              args.speed, args.extraload, args.freq,
              backendDevs)
end
