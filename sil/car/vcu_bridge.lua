-- vcu_bridge.lua: thin 333 Hz shared-memory bridge between Assetto Corsa (CSP)
-- and the MK12 VCU SIL executable (bfr_vcu.exe).
--
-- No VCU torque logic lives in Lua: every physics tick this script writes
-- driver inputs, wheel speeds, vehicle speed, and IMU signals to shared memory
-- and applies the torque command written back by bfr_vcu.exe.
--
-- Fail-closed fallback: if bfr_vcu.exe is not running or stops answering for
-- >50 ms while in 1st gear, applied motor torque is held at 0 Nm so a driver
-- can never unknowingly practice on AC's stock engine map.
--
-- The shared block layout must match sil/bridge_layout.h (26 x 4 = 104 bytes).

local NAME = "AcTools.CSP.Limited.BFR_VCU"
local MAGIC, VERSION = 0x56524642, 2
local LAYOUT = [[
  uint32_t magic; uint32_t version; uint32_t seq;
  float dt; float gas; float brake; float rpm; int32_t gear;
  float wheel_omega[4]; float speed_mps;
  float accel_x; float accel_y; float yaw_rate;
  uint32_t seq_echo; float torque_nm; int32_t vcu_state; uint32_t fault_bits;
  uint32_t vcu_ticks; float driver_nm; float tc_nm; float slip_ratio;
  uint32_t tc_active; uint32_t profile_hash;
]]

local LINK_TIMEOUT = 0.05 -- s without a new VCU answer before 0 Nm fallback
local LOG_EVERY = 5       -- s between CSP log summaries

local FLAG_TC_BYPASSED = 1 -- bit 0: --no-tc passed on CLI
local FLAG_TC_SHADOW   = 2 -- bit 1: firmware built with TC_ENABLED == 0

local STATE_NAMES = {
  [0] = "IDLE",
  [1] = "PRECHARGED",
  [2] = "DRIVE",
  [3] = "FAULT",
}

local clock = os.preciseClock or os.clock
local function u32(x) return x % 4294967296 end

local ok_link, link = pcall(ac.writeMemoryMappedFile, NAME, LAYOUT)
local link_status = "ok"
if ok_link then
  link.magic = MAGIC
  link.version = VERSION
else
  link_status = "FAILED: " .. tostring(link)
  link = nil
  ac.log("vcu_bridge: shared memory " .. link_status)
end

-- Prefer physics-thread wheel angular speeds (333 Hz, same as brakes.lua);
-- fall back to car.wheels if ac.accessCarPhysics() isn't present (e.g. unit tests).
local phys_wheels_ok = true
local function readPhysWheels()
  local w = ac.accessCarPhysics().wheels
  return w[0].angularSpeed, w[1].angularSpeed, w[2].angularSpeed, w[3].angularSpeed
end

local function readWheelOmegas()
  if phys_wheels_ok and ac.accessCarPhysics then
    local ok, w0, w1, w2, w3 = pcall(readPhysWheels)
    if ok and type(w0) == "number" then
      return w0, w1, w2, w3
    end
    phys_wheels_ok = false
  end
  local w = car.wheels
  if w and w[0] then
    return w[0].angularSpeed or 0, w[1].angularSpeed or 0,
           w[2].angularSpeed or 0, w[3].angularSpeed or 0
  end
  return 0, 0, 0, 0
end

local s = {
  seq = 0, echo = -1, answer_t = -math.huge, ever_linked = false,
  exe_nm = 0, vcu_state = 0, fault_bits = 0, vcu_ticks = 0,
  driver_nm = 0, tc_nm = 0, slip_ratio = 0, tc_active = 0, profile_hash = 0,
  win_t = clock(), win_ticks = 0, win_answers = 0,
  hz_wall = 0, answers_hz = 0,
  log_t = clock(),
}

-- Lock-free read of the VCU's reply. seq_echo is written last by bfr_vcu, so
-- read it before and after the payload fields and discard straddled reads.
local function readAnswer(now)
  local e = link.seq_echo
  local nm = link.torque_nm
  local st = link.vcu_state
  local fl = link.fault_bits
  local tk = link.vcu_ticks
  local drv = link.driver_nm
  local tcnm = link.tc_nm
  local slip = link.slip_ratio
  local tcact = link.tc_active
  local phash = link.profile_hash
  if link.seq_echo ~= e or e == s.echo then return end
  -- Ignore any leftover seq_echo from a previous session; count changes only.
  if s.echo < 0 then s.echo = e return end
  s.echo = e
  s.exe_nm = nm
  s.vcu_state = st
  s.fault_bits = fl
  s.vcu_ticks = tk
  s.driver_nm = drv
  s.tc_nm = tcnm
  s.slip_ratio = slip
  s.tc_active = tcact
  s.profile_hash = phash
  s.answer_t = now
  s.win_answers = s.win_answers + 1
  s.ever_linked = true
end

local function bitSet(flags, mask)
  return math.floor(flags / mask) % 2 == 1
end

local function tcStatusText(linked)
  if not linked then return "-" end
  if bitSet(s.fault_bits, FLAG_TC_BYPASSED) then return "OFF (--no-tc)" end
  if bitSet(s.fault_bits, FLAG_TC_SHADOW) then return "SHADOW (TC_ENABLED=0)" end
  if s.tc_active ~= 0 then return "ACTIVE" end
  return "READY"
end

function vcu_bridge(dt)
  local now = clock()
  local gas = car.gas or 0
  local brake = car.brake or 0
  local rpm = car.rpm or 0
  local gear = car.gear or 0
  local speed_mps = (car.speedKmh or 0) / 3.6
  local w0, w1, w2, w3 = readWheelOmegas()

  local acc = car.acceleration
  local accel_x = (acc and acc.z) or 0 -- AC: z = longitudinal, x = lateral
  local accel_y = (acc and acc.x) or 0
  local ang = car.localAngularVelocity
  local yaw_rate = (ang and ang.y) or 0

  -- Publish this tick to shared memory, seq last; never block waiting for a reply.
  s.seq = u32(s.seq + 1)
  if link then
    link.dt = dt
    link.gas = gas
    link.brake = brake
    link.rpm = rpm
    link.gear = gear
    link.wheel_omega[0] = w0
    link.wheel_omega[1] = w1
    link.wheel_omega[2] = w2
    link.wheel_omega[3] = w3
    link.speed_mps = speed_mps
    link.accel_x = accel_x
    link.accel_y = accel_y
    link.yaw_rate = yaw_rate
    link.seq = s.seq
    readAnswer(now)
  end

  local linked = link ~= nil and (now - s.answer_t < LINK_TIMEOUT)

  local applied_nm
  if gear < 1 then
    -- Not in a forward gear: release overrides so AC handles N/R normally.
    ac.overrideEngineTorque(math.nan)
    ac.overrideGasInput(math.nan)
  else
    -- In drive: apply VCU torque when connected, or 0 Nm fail-closed fallback.
    applied_nm = linked and s.exe_nm or 0
    ac.overrideCarState("clutch", 1, ac.Wheel.All)
    ac.overrideGasInput(1)
    ac.overrideEngineTorque(applied_nm)
  end

  -- Tick and answer rates over a 1 s window.
  s.win_ticks = s.win_ticks + 1
  if now - s.win_t >= 1 then
    local span = now - s.win_t
    s.hz_wall = s.win_ticks / span
    s.answers_hz = s.win_answers / span
    s.win_t, s.win_ticks, s.win_answers = now, 0, 0
  end

  local link_text
  if not link then
    link_text = "SHM " .. link_status
  elseif linked then
    link_text = "CONNECTED"
  elseif s.ever_linked then
    link_text = "LOST (0 Nm fallback)"
  else
    link_text = "WAITING (start bfr_vcu.exe)"
  end

  local hash_text = linked and string.format("%08x", u32(s.profile_hash)) or "-"
  local lag_text = linked and u32(s.seq - s.echo) or "-"
  local age_text = s.answer_t > -math.huge and string.format("%.0f ms", (now - s.answer_t) * 1000) or "never"
  local state_text = linked and (STATE_NAMES[s.vcu_state] or tostring(s.vcu_state)) or "-"
  local tc_text = tcStatusText(linked)

  ac.debug("01 VCU link", link_text)
  ac.debug("02 profile hash", hash_text)
  ac.debug("03 torque cmd Nm", applied_nm and string.format("%.1f", applied_nm) or "OFF (not in gear)")
  ac.debug("04 driver req Nm", linked and string.format("%.1f", s.driver_nm) or "-")
  ac.debug("05 TC model Nm", linked and string.format("%.1f", s.tc_nm) or "-")
  ac.debug("06 TC status", tc_text)
  ac.debug("07 rear slip ratio", linked and string.format("%+.3f", s.slip_ratio) or "-")
  ac.debug("08 VCU state", state_text)
  ac.debug("09 lag ticks", lag_text)
  ac.debug("10 answer age", age_text)
  ac.debug("11 answers/s", string.format("%.0f", s.answers_hz))
  ac.debug("12 tick Hz", string.format("%.0f", s.hz_wall))

  if now - s.log_t >= LOG_EVERY then
    s.log_t = now
    ac.log(string.format(
      "vcu_bridge: link=%s hash=%s cmd=%s drv=%.1f tc=%.1f slip=%+.3f answers_hz=%.0f lag=%s",
      link_text, hash_text, applied_nm and string.format("%.1f", applied_nm) or "OFF",
      s.driver_nm, s.tc_nm, s.slip_ratio, s.answers_hz, tostring(lag_text)))
  end
end
