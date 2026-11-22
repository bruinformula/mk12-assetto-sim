-- test_sil.lua: end-to-end LuaJIT <-> C shared-memory SIL test suite.
--
-- Runs sil/car/vcu_bridge.lua against the real compiled build/bfr_vcu over a
-- shared memory file, with ac.* stubbed. Checks:
--   1. Bridge v2 layout size (104 bytes, matching sil/bridge_layout.h)
--   2. Fail-closed 0 Nm fallback when bfr_vcu is not running, and NaN release in N/R
--   3. Default base profile torque mapping, LPF ramp, 0.1 Nm quantization, gear reset,
--      wheel-speed slip telemetry, and FNV-1a profile hash
--   4. Link-loss detection and 0 Nm fallback when bfr_vcu exits
--   5. Custom YAML loading (positional drag-and-drop arg), power limit cap at high RPM
--   6. Invalid YAML rejection (non-monotonic table, out-of-range torque, bad bands, unknown key)
--   7. --no-tc flag and TC shadow-mode status reporting
--
-- Usage (from sil/):
--   luajit tests/test_sil.lua build/bfr_vcu ../vendor/mk12-vcu

local bit = require "bit"
local ffi = require "ffi"

local exe = assert(arg[1], "usage: test_sil.lua BFR_VCU_BINARY [VCU_DIR]")
local vcu_dir = arg[2] or "../vendor/mk12-vcu"

ffi.cdef [[
  int open(const char *path, int flags, int mode);
  int close(int fd);
  int ftruncate(int fd, int64_t length);
  void *mmap(void *addr, size_t len, int prot, int flags, int fd, int64_t off);
  int usleep(unsigned int usec);
]]

local O_RDWR = 0x0002
local PROT_RW, MAP_SHARED = 3, 1

local shm_path = "build/test_sil_bridge.bin"
os.remove(shm_path)
math.nan = math.nan or 0 / 0

-- FNV-1a 32-bit matching Profile Studio (docs/index.html) and sil/profile_loader.c.
local function fnv1a(s)
  local h = 0x811c9dc5
  for i = 1, #s do
    h = bit.bxor(h, s:byte(i))
    -- 32-bit unsigned multiply by 0x01000193 (avoiding double precision loss):
    -- 0x01000193 = (1 << 24) + 0x193
    local hi = bit.lshift(h, 24)
    local lo = (bit.band(h, 0xffff) * 0x193) + bit.lshift(bit.rshift(h, 16) * 0x193, 16)
    h = bit.tobit(hi + lo)
  end
  return string.format("%08x", h % 4294967296)
end

-- Stub CSP environment.
local applied = {}
car = {
  gas = 0, brake = 0, rpm = 0, gear = 1, speedKmh = 0,
  wheels = {
    [0] = { angularSpeed = 0 },
    [1] = { angularSpeed = 0 },
    [2] = { angularSpeed = 0 },
    [3] = { angularSpeed = 0 },
  },
  acceleration = { x = 0, y = 0, z = 0 },
  localAngularVelocity = { x = 0, y = 0, z = 0 },
}

ac = {
  Wheel = { All = 15 },
  log = function() end,
  debug = function(k, v) applied.debug = applied.debug or {}; applied.debug[k] = v end,
  overrideCarState = function() end,
  overrideGasInput = function(v) applied.gas = v end,
  overrideEngineTorque = function(v) applied.nm = v end,
  accessCarPhysics = function() return { wheels = car.wheels } end,
  writeMemoryMappedFile = function(name, layout)
    assert(name:find("^AcTools%.CSP%.Limited%."), "CSP rejects this shared memory name")
    ffi.cdef("typedef struct {" .. layout .. "} sil_block;")
    local size = ffi.sizeof("sil_block")
    assert(io.open(shm_path, "ab")):close()
    local fd = ffi.C.open(shm_path, O_RDWR, 0)
    assert(fd >= 0 and ffi.C.ftruncate(fd, size) == 0, "can't create " .. shm_path)
    local p = ffi.C.mmap(nil, size, PROT_RW, MAP_SHARED, fd, 0)
    ffi.C.close(fd)
    return ffi.cast("sil_block*", p)
  end,
}

local fake_t = 0
os.preciseClock = function() return fake_t end

package.path = "car/?.lua;" .. package.path
require "vcu_bridge"

local failed = 0
local function check(cond, msg)
  print((cond and "ok   " or "FAIL ") .. msg)
  if not cond then failed = failed + 1 end
end

local DT = 1 / 333
local function run(seconds, fn)
  local steps = math.floor(seconds / DT)
  for i = 1, steps do
    if fn then fn(i) end
    fake_t = fake_t + DT
    vcu_bridge(DT)
    ffi.C.usleep(math.floor(DT * 1e6))
  end
end

local function write_file(path, text)
  local f = assert(io.open(path, "wb"))
  f:write(text)
  f:close()
end

local function set_wheels(fl, fr, rl, rr)
  car.wheels[0].angularSpeed = fl
  car.wheels[1].angularSpeed = fr
  car.wheels[2].angularSpeed = rl
  car.wheels[3].angularSpeed = rr
end

-- 1. Layout size check
check(ffi.sizeof("sil_block") == 104,
      "bridge v2 layout is 104 bytes, matching bridge_layout.h (26 x 4)")

-- 2. No VCU running: fail-closed 0 Nm in 1st gear, NaN release in neutral
car.gas = 0.7
car.gear = 1
run(0.1)
check(applied.debug["01 VCU link"] == "WAITING (start bfr_vcu.exe)",
      "no VCU: link shows WAITING")
check(applied.nm == 0 and applied.gas == 1,
      "no VCU: fail-closed 0 Nm in 1st gear, gas held at 1")

car.gear = 0
run(0.05)
check(applied.nm ~= applied.nm and applied.gas ~= applied.gas,
      "not in gear: overrides released (NaN)")
car.gear = 1

-- 3. Default base profile over shared memory (with ACTIVE Simulink TC!)
local base_canon = '{"torque":{"max_nm":80,"pedal_table":[0,0.1,0.2,0.3,0.4,0.5,0.6,0.7,0.8,0.9,1]},"pedal":{"coast_end":0.03,"drive_start":0.08},"regen":{"liftoff_nm":-5,"fade_lo_kph":8,"fade_hi_kph":20,"ramp_nm_s":40},"filter":{"tau_s":0.05,"slew_nm_s":2000}}'
local base_hash = fnv1a(base_canon)

car.gas = 1.0
car.brake = 0
car.rpm = 2000
car.speedKmh = 36 -- 10 m/s
set_wheels(50.0, 50.0, 50.0, 50.0) -- zero slip at 10 m/s (0.2 m effective radius)

assert(os.execute(string.format("%q --file %q --seconds 2.4 > /dev/null 2>&1 &", exe, shm_path)))
run(0.50)

check(applied.debug["01 VCU link"] == "CONNECTED", "base run: CONNECTED")
check(applied.debug["02 profile hash"] == base_hash,
      "base run: profile hash " .. tostring(applied.debug["02 profile hash"]) .. " matches FNV-1a (" .. base_hash .. ")")
check(applied.debug["06 TC status"] == "READY",
      "base run: TC status is READY with zero wheel slip (got " .. tostring(applied.debug["06 TC status"]) .. ")")
check(applied.debug["08 VCU state"] == "DRIVE", "base run: VCU state is DRIVE")
local lag = applied.debug["09 lag ticks"]
check(type(lag) == "number" and lag <= 4, "base run: answer lag " .. tostring(lag) .. " ticks (<= 4)")
check(math.abs(applied.nm - 80.0) < 0.2,
      string.format("base run: full throttle settles to 80 Nm with no slip (got %.2f)", applied.nm))
check(math.abs(applied.nm * 10 - math.floor(applied.nm * 10 + 0.5)) < 1e-4,
      string.format("base run: torque is 0.1 Nm quantized (got %.4f)", applied.nm))
check(applied.debug["04 driver req Nm"] == "80.0", "base run: driver req telemetry is 80.0 Nm")

-- 3b. Active Simulink TC: 20% rear wheelspin (50 rad/s front, 60 rad/s rear => SR = +0.200)
-- Target SR = 0.136, P = -5 => u = 0.32 => TC cuts 80 Nm to (1 - 0.32) * 80 = 54.4 Nm!
set_wheels(50.0, 50.0, 60.0, 60.0)
run(0.08)
check(applied.debug["06 TC status"] == "ACTIVE",
      "active TC: 20% rear wheelspin triggers ACTIVE status (got " .. tostring(applied.debug["06 TC status"]) .. ")")
local slip_val = tonumber(applied.debug["07 rear slip ratio"])
check(slip_val ~= nil and math.abs(slip_val - 0.200) < 0.01,
      "active TC: rear slip ratio telemetry is +0.200 (got " .. tostring(applied.debug["07 rear slip ratio"]) .. ")")
check(math.abs(applied.nm - 54.4) < 0.3,
      string.format("active TC: 20%% slip cuts 80 Nm -> ~54.4 Nm (got %.2f)", applied.nm))

-- Heavy wheelspin (40% slip: 70 rad/s rear) => full cut to 0 Nm
set_wheels(50.0, 50.0, 70.0, 70.0)
run(0.08)
check(applied.nm == 0,
      string.format("active TC: 40%% slip cuts torque to 0 Nm (got %.2f)", applied.nm))

-- Standing launch (0 rad/s front, 8 rad/s rear => slip speed 1.6 m/s => u=0.6 => 32.0 Nm)
car.speedKmh = 0
set_wheels(0.0, 0.0, 8.0, 8.0)
run(0.08)
check(math.abs(applied.nm - 32.0) < 0.3,
      string.format("active TC: standing launch slip-speed mode cuts 80 Nm -> ~32.0 Nm (got %.2f)", applied.nm))

-- Back to zero slip rolling for remaining pedal checks
car.speedKmh = 36
set_wheels(50.0, 50.0, 50.0, 50.0)

-- 50% normalized pedal on base: (0.54 - 0.08) / (1 - 0.08) = 0.5 => 40.0 Nm
car.gas = 0.54
run(0.35)
check(math.abs(applied.nm - 40.0) < 0.2,
      string.format("base run: 50%% normalized pedal gives 40.0 Nm (got %.2f)", applied.nm))

-- Coast band (below drive_start = 0.08) => 0 Nm
car.gas = 0.05
run(0.08)
check(applied.nm == 0, string.format("base run: coast band gives 0 Nm (got %.2f)", applied.nm))

-- Neutral resets TorqueFilter
car.gas = 1.0
car.gear = 0
run(0.05)
check(applied.debug["08 VCU state"] == "IDLE", "neutral: VCU state is IDLE")
car.gear = 1
-- After 25 ms back in 1st gear with tau=50ms, LPF should be well below 80 Nm (~26 Nm)
run(0.025)
check(applied.nm > 0 and applied.nm < 45,
      string.format("re-enter drive: filter reset confirmed, ramps from 0 (got %.1f Nm at 25 ms)", applied.nm))

-- 4. Wait for bfr_vcu to finish and verify link-loss 0 Nm fallback
car.gas = 1.0
run(1.25)
check(applied.debug["01 VCU link"] == "LOST (0 Nm fallback)",
      "VCU stopped: link shows LOST (0 Nm fallback)")
check(applied.nm == 0, "VCU stopped: applied torque drops to 0 Nm")

-- 5. Custom YAML profile via positional drag-and-drop argument + power limit check
local custom_yaml = "build/test_custom.yaml"
write_file(custom_yaml, [[
name: tyler_endurance
extends: base

torque:
  max_nm: 120
  pedal_table: [0, 0.04, 0.1, 0.17, 0.26, 0.36, 0.47, 0.59, 0.72, 0.86, 1]

filter:
  tau_s: 0.02
  slew_nm_s: 3000
]])
local custom_canon = '{"torque":{"max_nm":120,"pedal_table":[0,0.04,0.1,0.17,0.26,0.36,0.47,0.59,0.72,0.86,1]},"pedal":{"coast_end":0.03,"drive_start":0.08},"regen":{"liftoff_nm":-5,"fade_lo_kph":8,"fade_hi_kph":20,"ramp_nm_s":40},"filter":{"tau_s":0.02,"slew_nm_s":3000}}'
local custom_hash = fnv1a(custom_canon)

car.gear = 1
car.rpm = 2000
car.speedKmh = 36
set_wheels(50.0, 50.0, 70.0, 70.0) -- heavy 40% rear wheelspin: must be ignored under --no-tc!
car.gas = 0.54 -- 50% normalized pedal => table[5]=0.36 => 120 * 0.36 = 43.2 Nm
-- Pass custom_yaml as a POSITIONAL arg (tests Windows drag-and-drop UX) + --no-tc
assert(os.execute(string.format("%q %q --no-tc --file %q --seconds 1.2 > /dev/null 2>&1 &",
                                exe, custom_yaml, shm_path)))
run(0.35)

check(applied.debug["01 VCU link"] == "CONNECTED", "custom profile: CONNECTED (positional YAML arg)")
check(applied.debug["02 profile hash"] == custom_hash and custom_hash ~= base_hash,
      "custom profile: hash " .. tostring(applied.debug["02 profile hash"]) .. " matches expected (" .. custom_hash .. ")")
check(applied.debug["06 TC status"] == "OFF (--no-tc)", "custom profile: --no-tc reported in TC status")
check(math.abs(applied.nm - 43.2) < 0.2,
      string.format("custom profile (--no-tc): passes full 43.2 Nm despite 40%% wheelspin (got %.2f)", applied.nm))

-- Full throttle at 6000 rpm: EV.3.3.1 power cap = 75000 * 0.85 / (6000 * 2*pi/60) = 101.46 Nm -> 101.4 Nm
car.gas = 1.0
car.rpm = 6000
run(0.35)
check(math.abs(applied.nm - 101.4) < 0.3,
      string.format("custom profile: EV.3.3.1 power cap limits 120 Nm to ~101.4 Nm at 6000 rpm (got %.2f)", applied.nm))

-- Let the second VCU process finish cleanly before cleanup.
run(0.6)

-- 6. Invalid YAML rejection tests (must exit non-zero)
local bad_cases = {
  {
    label = "non-monotonic pedal_table",
    yaml = "name: bad_mono\nextends: base\ntorque:\n  pedal_table: [0, 0.2, 0.1, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1]\n",
  },
  {
    label = "out-of-range max_nm (221 > 220)",
    yaml = "name: bad_max\nextends: base\ntorque:\n  max_nm: 221\n",
  },
  {
    label = "coast_end >= drive_start",
    yaml = "name: bad_bands\nextends: base\npedal:\n  coast_end: 0.09\n  drive_start: 0.08\n",
  },
  {
    label = "unknown key",
    yaml = "name: bad_key\nextends: base\ntorque:\n  boost_nm: 50\n",
  },
  {
    label = "missing name",
    yaml = "torque:\n  max_nm: 70\n",
  },
}

local bad_path = "build/test_bad.yaml"
for _, tc in ipairs(bad_cases) do
  write_file(bad_path, tc.yaml)
  local rc = os.execute(string.format("%q --profile %q --verify-reference %q > /dev/null 2>&1",
                                      exe, bad_path, vcu_dir .. "/tests/reference/outputs/base.csv"))
  check(rc ~= 0 and rc ~= true, "reject invalid YAML: " .. tc.label)
end

os.remove(shm_path)
os.remove(custom_yaml)
os.remove(bad_path)

if failed > 0 then
  print(failed .. " failed")
  os.exit(1)
end
print("all passed")
