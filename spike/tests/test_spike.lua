-- test_spike.lua: car/spike.lua against the real vcu_dummy over a shared file,
-- with ac.* stubbed. Checks LOCAL, EXE, the brake cut, fallback when the
-- program stops, and that the layout matches bridge_layout.h's size.
--
--   luajit tests/test_spike.lua VCU_DUMMY_BINARY       (run from spike/)

local ffi = require "ffi"
local exe = assert(arg[1], "usage: test_spike.lua VCU_DUMMY_BINARY")

ffi.cdef [[
  int open(const char *path, int flags, int mode);
  int close(int fd);
  int ftruncate(int fd, int64_t length);
  void *mmap(void *addr, size_t len, int prot, int flags, int fd, int64_t off);
  int usleep(unsigned int usec);
]]
local O_RDWR = 0x0002
local PROT_RW, MAP_SHARED = 3, 1

local path = "build/test_bridge.bin"
os.remove(path)
math.nan = math.nan or 0 / 0 -- CSP adds math.nan; plain LuaJIT doesn't

-- Stand-ins for CSP: shared memory over a file, recorded overrides.
local applied = {}
ac = {
  Wheel = { All = 15 },
  log = function() end,
  debug = function(k, v) applied.debug = applied.debug or {}; applied.debug[k] = v end,
  overrideCarState = function() end,
  overrideGasInput = function(v) applied.gas = v end,
  overrideEngineTorque = function(v) applied.nm = v end,
  accessCarPhysics = function() return { gas = car.gas } end,
  writeMemoryMappedFile = function(name, layout)
    assert(name:find("^AcTools%.CSP%.Limited%."), "CSP rejects this name")
    ffi.cdef("typedef struct {" .. layout .. "} spike_block;")
    local size = ffi.sizeof("spike_block")
    -- Create it with io.open: open()'s mode is variadic and FFI can't pass it portably.
    assert(io.open(path, "ab")):close()
    local fd = ffi.C.open(path, O_RDWR, 0)
    assert(fd >= 0 and ffi.C.ftruncate(fd, size) == 0, "can't create " .. path)
    local p = ffi.C.mmap(nil, size, PROT_RW, MAP_SHARED, fd, 0)
    ffi.C.close(fd)
    return ffi.cast("spike_block*", p)
  end,
}
car = { gas = 0, brake = 0, rpm = 0, gear = 1, speedKmh = 0 }
local fake_t = 0
os.preciseClock = function() return fake_t end -- real time is slept below

package.path = "car/?.lua;" .. package.path
require "spike"

local failed = 0
local function check(cond, msg)
  print((cond and "ok   " or "FAIL ") .. msg)
  if not cond then failed = failed + 1 end
end

local DT = 1 / 333
local function run(seconds, f)
  for _ = 1, math.floor(seconds / DT) do
    if f then f() end
    fake_t = fake_t + DT
    spike(DT)
    ffi.C.usleep(math.floor(DT * 1e6))
  end
end

check(ffi.sizeof("spike_block") == 52, "layout is 52 bytes, as bridge_layout.h (13 x 4)")

-- No program: LOCAL, 50 Nm x pedal, brake cuts.
car.gas = 0.5
run(0.2)
check(applied.debug["01 mode"] == "LOCAL", "no program: LOCAL")
check(math.abs(applied.nm - 25) < 1e-6 and applied.gas == 1, "LOCAL: 50 Nm x 0.5 pedal, gas held at 1")
car.brake = 0.5
run(0.05)
check(applied.nm == 0, "LOCAL: brake past 10% gives 0 Nm")
car.brake = 0

-- Not in gear: overrides released.
car.gear = 0
run(0.05)
check(applied.nm ~= applied.nm and applied.gas ~= applied.gas, "not in gear: overrides released (NaN)")
car.gear = 1

-- Start the program for 1.5 s.
assert(os.execute(string.format("%q --file %q --seconds 1.5 > /dev/null &", exe, path)))
run(0.8)
check(applied.debug["01 mode"] == "EXE", "program running: EXE")
check(applied.debug["17 exe link"] == "CONNECTED", "link shows CONNECTED")
check(applied.nm == 50, "EXE: 50 Nm with the pedal past 10%")
local lag = applied.debug["20 exe lag ticks"]
check(type(lag) == "number" and lag <= 3, "EXE: answer lag " .. tostring(lag) .. " ticks (<= 3)")
car.gas = 0.05
run(0.1)
check(applied.nm == 0, "EXE: 0 Nm below 10% pedal")
car.gas = 0.6

-- Program stops: back to LOCAL within about 50 ms.
run(1.2)
check(applied.debug["01 mode"] == "LOCAL", "program stopped: back to LOCAL")
check(applied.debug["17 exe link"] == "LOST (back to LOCAL)", "link shows LOST")
check(math.abs(applied.nm - 30) < 1e-6, "LOCAL again: 50 Nm x 0.6 pedal")

os.remove(path)
if failed > 0 then
  print(failed .. " failed")
  os.exit(1)
end
print("all passed")
