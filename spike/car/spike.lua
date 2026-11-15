-- spike.lua: the software-in-the-loop spike. Answers, on a PC with AC + CSP:
--   1. Does the car script run at about 333 Hz?
--   2. Does positive ac.overrideEngineTorque move the car?
--   3. Does car.gas still read the driver's pedal while the script holds the
--      gas input at 1 (as powertrain.lua does)?
--   4. Can the car script and a separate program (vcu_dummy.exe) share memory?
-- Everything shows in CSP's Lua Debug app, and a summary goes to the CSP log
-- every 5 s.
--
-- Without the program: 50 Nm x pedal ("LOCAL"). With it: whatever it sends
-- ("EXE"), which is 50 Nm while the pedal is past 10%. No answer for 50 ms:
-- back to LOCAL. Brake past 10% always means 0 Nm.
--
-- The shared block's layout must match bridge_layout.h.

local NAME = "AcTools.CSP.Limited.BFR_VCU"
local MAGIC, VERSION = 0x56524642, 1
local LAYOUT = [[
  uint32_t magic; uint32_t version; uint32_t seq;
  float dt; float gas; float brake; float rpm; int32_t gear;
  uint32_t seq_echo; float torque_nm; int32_t vcu_state; uint32_t fault_bits;
  uint32_t vcu_ticks;
]]
local LOCAL_NM = 50       -- Nm at full pedal without the program
local LINK_TIMEOUT = 0.05 -- s without a new answer before falling back
local BRAKE_CUT = 0.10    -- brake pedal above this: no torque
local LOG_EVERY = 5       -- s between summaries in the CSP log

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
  ac.log("spike: shared memory " .. link_status)
end

local csp_version = "see Content Manager"
pcall(function()
  if ac.getPatchVersionCode then csp_version = tostring(ac.getPatchVersionCode()) end
end)

-- Physics-thread gas, as a second opinion on car.gas. The field name isn't in
-- the public SDK, so give up quietly the first time it fails.
local physics_gas_ok = true
local function physicsGas()
  if not physics_gas_ok then return nil end
  local ok, v = pcall(function() return ac.accessCarPhysics().gas end)
  if ok and type(v) == "number" then return v end
  physics_gas_ok = false
  return nil
end

local s = {
  seq = 0, echo = -1, answer_t = -math.huge, exe_nm = 0, exe_state = 0,
  exe_faults = 0, exe_ticks = 0, ever_linked = false,
  win_t = clock(), win_ticks = 0, win_dt = 0, win_answers = 0,
  hz_wall = 0, hz_sim = 0, answers_hz = 0,
  log_t = clock(), gmin = math.huge, gmax = -math.huge, held_ticks = 0,
  speed_prev = nil, accel = 0, responded = false,
}

-- The program's answer, if it has a new one. Echo is written last, so read it
-- before and after the fields and drop a read that straddled a write.
local function readAnswer(now)
  local e = link.seq_echo
  local nm, state, faults, ticks = link.torque_nm, link.vcu_state, link.fault_bits, link.vcu_ticks
  if link.seq_echo ~= e or e == s.echo then return end
  -- The first value seen may be left over from an earlier run: only count changes.
  if s.echo < 0 then s.echo = e return end
  s.echo, s.exe_nm, s.exe_state, s.exe_faults, s.exe_ticks = e, nm, state, faults, ticks
  s.answer_t = now
  s.win_answers = s.win_answers + 1
  s.ever_linked = true
end

local function fmt(x) return string.format("%.2f", x) end

function spike(dt)
  local now = clock()
  local gas, brake, rpm, gear = car.gas, car.brake, car.rpm, car.gear

  -- 4. Hand this tick to the program, then take its newest answer. Never wait.
  s.seq = u32(s.seq + 1)
  if link then
    link.dt, link.gas, link.brake, link.rpm, link.gear = dt, gas, brake, rpm, gear
    link.seq = s.seq
    readAnswer(now)
  end
  local linked = now - s.answer_t < LINK_TIMEOUT

  local mode, nm
  if gear < 1 then
    mode = "OFF (not in a forward gear)"
  elseif linked then
    mode, nm = "EXE", s.exe_nm
  else
    mode, nm = "LOCAL", LOCAL_NM * gas
  end
  if nm and brake > BRAKE_CUT then nm = 0 end

  -- Same calls as powertrain.lua: gas held open so the override is the whole
  -- motor torque. (The SDK says overrideCarState only changes what's drawn,
  -- not physics; kept so this tests exactly what we'd ship.)
  if nm then
    ac.overrideCarState("clutch", 1, ac.Wheel.All)
    ac.overrideGasInput(1)
    ac.overrideEngineTorque(nm)
  else
    ac.overrideEngineTorque(math.nan)
    ac.overrideGasInput(math.nan)
  end

  -- 3. Range of car.gas while the script holds the gas input.
  if nm then
    s.held_ticks = s.held_ticks + 1
    if gas < s.gmin then s.gmin = gas end
    if gas > s.gmax then s.gmax = gas end
  end

  -- 2. Does the car accelerate on the override? Smoothed over ~0.2 s.
  local v = car.speedKmh / 3.6
  if s.speed_prev and dt > 0 then
    local a = (v - s.speed_prev) / dt
    local k = dt / (0.2 + dt)
    s.accel = s.accel + (a - s.accel) * k
  end
  s.speed_prev = v
  if nm and nm >= 40 and s.accel > 1 then s.responded = true end

  -- 1. Tick rate, by wall clock and by summed dt, once a second.
  s.win_ticks = s.win_ticks + 1
  s.win_dt = s.win_dt + dt
  if now - s.win_t >= 1 then
    s.hz_wall = s.win_ticks / (now - s.win_t)
    s.hz_sim = s.win_dt > 0 and s.win_ticks / s.win_dt or 0
    s.answers_hz = s.win_answers / (now - s.win_t)
    s.win_t, s.win_ticks, s.win_dt, s.win_answers = now, 0, 0, 0
  end

  local gas_range = s.held_ticks > 0 and (fmt(s.gmin) .. " to " .. fmt(s.gmax)) or "not held yet"
  local gas_verdict
  if s.held_ticks == 0 then gas_verdict = "drive in gear first"
  elseif s.gmin > 0.99 then gas_verdict = "STUCK AT 1? lift off fully"
  elseif s.gmax < 0.9 then gas_verdict = "press the pedal to the floor"
  else gas_verdict = "follows the pedal" end

  local link_text
  if not link then link_text = "no shared memory"
  elseif linked then link_text = "CONNECTED"
  elseif s.ever_linked then link_text = "LOST (back to LOCAL)"
  else link_text = "waiting for vcu_dummy.exe" end
  local age = s.answer_t > -math.huge and string.format("%.0f ms", (now - s.answer_t) * 1000) or "never"

  ac.debug("01 mode", mode)
  ac.debug("02 torque cmd Nm", nm or 0)
  ac.debug("03 tick Hz (wall)", string.format("%.0f", s.hz_wall))
  ac.debug("04 tick Hz (dt)", string.format("%.0f", s.hz_sim))
  ac.debug("05 dt ms", string.format("%.2f", dt * 1000))
  ac.debug("06 car.gas", fmt(gas))
  ac.debug("07 physics gas", physicsGas() or "n/a")
  ac.debug("08 gas while held", gas_range)
  ac.debug("09 gas check", gas_verdict)
  ac.debug("10 brake", fmt(brake))
  ac.debug("11 gear", gear)
  ac.debug("12 rpm", string.format("%.0f", rpm))
  ac.debug("13 speed km/h", string.format("%.1f", car.speedKmh))
  ac.debug("14 accel m/s2", fmt(s.accel))
  ac.debug("15 car responds", s.responded and "YES" or "not yet (floor it in LOCAL or EXE)")
  ac.debug("16 shared memory", link_status)
  ac.debug("17 exe link", link_text)
  ac.debug("18 exe answer age", age)
  ac.debug("19 exe answers/s", string.format("%.0f", s.answers_hz))
  ac.debug("20 exe lag ticks", linked and u32(s.seq - s.echo) or "-")
  ac.debug("21 exe torque Nm", s.exe_nm)
  ac.debug("22 CSP version", csp_version)

  if now - s.log_t >= LOG_EVERY then
    s.log_t = now
    ac.log(string.format(
      "spike: mode=%s hz_wall=%.0f hz_dt=%.0f gas_held=%s gas_check=%s responds=%s shm=%s link=%s answers_hz=%.0f lag=%s csp=%s",
      mode, s.hz_wall, s.hz_sim, gas_range, gas_verdict, tostring(s.responded), link_status, link_text,
      s.answers_hz, tostring(linked and u32(s.seq - s.echo) or "-"), csp_version))
  end
end
