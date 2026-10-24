-- Automatic slip angle tester (3-phase: ramp→hold→return)
-- 8/12/2025 C4
-- Yokai / Arch
-- For Arch/Girellu tire development http://github.com/archibaldmilton/Girellu
--
-- Honk horn to begin/cancel testing
-- Lowbeams on = XL wheel load
-- Highbeams on = XXL wheel load
--
-- Autotest runs the load table, input your desired N tests, as many as wanted
-- Right click numbers in Lua debug app to open live graphs
--
-- Change log:
-- 4/23/2025 B: Test logic lag + cooldown, enhanced debugs
-- 4/23/2025 C: Changed test speed, added auto testing
-- 4/24/2025 C3: First sweep version
-- 8/12/2025 C4: Replace sweep with ramp→HOLD(10s)→return (±12° configurable)
-- Added 5-second speed hold before turning
--
-- Notes:
--  • Uses CSP Lua app/physics APIs (script.update, ac.accessCarPhysics, ac.addHubForce, etc.).
--  • Slip angle target is in degrees. Sign follows LF left-turn convention (negative = left).
--  • Cooldown smoothly hands control back to user steering.
--  • Auto-test iterates load presets; horn cancels.
--  • Requires CSP Lua apps runtime (see CSP Lua apps/physics docs).

-- ===========================================================================
-- Globals
local CAR = car
local DATA = ac.accessCarPhysics()
local SIM = ac.getSimState()

-- ===========================================================================
-- Settings
local target_speed = 37.623854681484  -- km/h (≈35 mph)
local target_steer = -40     -- LF peak slip angle [deg]; set 12 for right-turn sweep

-- Extra load states in newtons (applied at wheel center downward)
local load_base = 0
local load_lo = -220 --(CAR.mass * 0.20) * 9.81  -- car kg percentage in newtons
local load_hi = -695  -- car kg percentage in newtons

-- ===========================================================================
-- Logic State
local ready = false
local speedPID = nil
local steerPID = nil

local trigger_old = nil
local disable_load = false
local benchmark = nil

local test = {
  duration = 10,        -- seconds to ramp each way
  hold_s = 10,         -- seconds to hold at peak
  speed_hold_s = 3,    -- seconds to hold at target speed before turning
  speed_hold_t = 5,    -- countdown for speed hold
  -- for logic
  dir = -1,            -- -1 ramps 0→target, +1 ramps target→0
  time = 0,            -- unlagged time accumulator
  nTime = 0,           -- normalized & lagged [0..1]
  phase = "RAMP_UP",   -- "RAMP_UP" -> "HOLD" -> "RAMP_DOWN"
  hold_t = 2,          -- countdown for HOLD
  active = false,
  waiting = true,
  cooldown = false,
}

local auto_test = {
  enabled = false,
  index = -1,
  loads = { load_base, load_lo, load_hi }
}

-- ===========================================================================
-- Setup function, runs once at start
local function prepare()
  -- PID controllers
  speedPID = newPID(
    15,    -- P
    0.1,   -- I
    0.1    -- D
  )
  speedPID:setTarget(target_speed)

  steerPID = newPID(
    100,   -- P
    20,    -- I
    0.1    -- D
  )
  steerPID:setTarget(target_steer)

  -- Debug slots
  ac.debug("A1] TARGET")
  ac.debug("A2] SLIP (Front Left)")
  ac.debug("A3] RADIUS (m)")
  ac.debug("Z] PID_SPEED")
  ac.debug("Z] PID_STEER")
  ac.debug("A", " ")
  ac.debug("T", " ")
  ac.debug("Z", " ")

  return true
end

-- ===========================================================================
-- Safety: returning to pits while extra load is applied can cause issues
function script.reset()
  disable_load = true
  setTimeout(function() disable_load = false end, 2)
end

-- ===========================================================================
function script.update(dt)
  -- Safeties
  if (SIM.inputMode == ac.UserInputMode.Wheel) then return end
  if (CAR.handbrake > 0.5) then return end
  -- if (not CAR.hazardLights) then return end

  -- Prepare once
  if (not ready) then ready = prepare() return end

  -- Locals
  local KMH = CAR.speedKmh
  local LF_SLIP = CAR.wheels[0].slipAngle   -- degrees
  local STEER = CAR.steer / CAR.steerLock

  -- Calculate Dynamic Turning Radius
  local speed_ms = KMH / 3.6
  local yaw_rate = math.abs(CAR.localAngularVelocity.y) -- rad/s
  local turning_radius = yaw_rate > 0.001 and (speed_ms / yaw_rate) or 0

  -- Debugs
  ac.debug("!] STEER (-1/+1)", math.round(DATA.steer, 2), -1, 1)
  ac.debug("A2] SLIP (Front Left)", LF_SLIP, target_steer, 0)
  ac.debug("A3] RADIUS (m)", turning_radius)
  ac.debug("T1] LOAD (N)", CAR.wheels[0].load)
  ac.debug("Z] AUTO TEST", auto_test.index)

  -- -------------------------------------------------------------------------
  -- Extra load application
  local force_state = "OFF"
  local added_force = load_base
  if (CAR.headlightsActive) or (auto_test.index > 0) then
    if (CAR.lowBeams) then
      force_state = "LOW (" .. load_lo .. " N)"
      added_force = load_lo
    else
      force_state = "HIGH (" .. load_hi .. " N)"
      added_force = load_hi
    end
    if (auto_test.index > 0) then
      added_force = auto_test.loads[auto_test.index]
      force_state = "AUTO: " .. added_force .. " N"
    end
  end

  -- Loop all 4 wheels, add force
  for x = 1, 2 do
    if (not disable_load) then
      ac.addHubForce(
        x - 1,                        -- wheel index
        CAR.wheels[x - 1].position,   -- from position vector
        vec3(0, -added_force, 0)      -- force [N]
      )
    end
  end

  ac.debug("!] HEADLIGHTS/LOAD", force_state)
  ac.setMessage("Extra load: " .. force_state, "", "illegal", 2)

  -- -------------------------------------------------------------------------
  -- Trigger test activation/cancel (horn)
  local trigger = CAR.hornActive
  if (not trigger_old) then trigger_old = trigger end
  if (trigger_old ~= trigger) then
    -- Reset controllers
    speedPID:reset()
    steerPID:reset()
    steerPID:setTarget(0)

    -- Reset test state
    test.time = 0
    test.nTime = 0
    test.dir = -1
    test.cooldown = false
    test.phase = "RAMP_UP"
    test.hold_t = test.hold_s
    test.speed_hold_t = test.speed_hold_s -- Reset speed timer
    benchmark = nil

    -- Auto test start
    if (auto_test.enabled) and (auto_test.index < 0) then
      auto_test.index = 0
      next_test() -- for log message
    end
    -- Cancel auto test if we were active
    if (test.active) then auto_test.index = -1 end

    -- Wait for speed-up
    test.waiting = true

    -- Cooldown if we were active
    if (test.active) then do_cooldown() end

    test.active = not test.active
    if (test.active) then
      ac.log("Test Started (load: " .. added_force .. " N)")
    else
      ac.log("Test Cancelled")
    end
  end
  trigger_old = trigger

  -- -------------------------------------------------------------------------
  -- Wait for target speed and hold for specified duration
  if (test.active) then
    auto_speed(KMH)
    if (test.waiting) then
      ac.debug("Z] SPEED HOLD TIMER", math.max(0, test.speed_hold_t))
      if (KMH > target_speed * 0.90) then
        test.speed_hold_t = test.speed_hold_t - dt
        if (test.speed_hold_t <= 0) then
          test.waiting = false
        end
      else
        -- If speed drops below threshold during the wait, reset timer
        test.speed_hold_t = test.speed_hold_s
      end
    end
  end

  -- -------------------------------------------------------------------------
  -- Steering profile: RAMP_UP → HOLD → RAMP_DOWN
  if (test.active) and (not test.waiting) then
    if (not benchmark) then benchmark = os.clock() end

    -- helper: update normalized time (0→1 up, 1→0 down) with lag
    local function update_nTime(dt)
      local test_gamma = 1.0
      if test.dir < 0 then
        test.time = math.min(test.time + dt, test.duration)
      else
        test.time = math.max(test.time - dt, 0)
      end
      local a = (test.time / test.duration) ^ test_gamma
      test.nTime = math.applyLag(test.nTime, a, 0.96, dt)
    end

    local target

    if test.phase == "RAMP_UP" then
      update_nTime(dt)
      target = math.lerp(0, target_steer, test.nTime)

      -- Enter HOLD once at peak (allow slip to reach target)
      if (test.nTime >= 1.0) and ( (target_steer >= 0 and LF_SLIP >= target_steer) or
                                   (target_steer < 0  and LF_SLIP <= target_steer) ) then
        test.phase = "HOLD"
        test.hold_t = test.hold_s
        test.time   = test.duration
        test.nTime  = 1.0
        target      = target_steer
      end

    elseif test.phase == "HOLD" then
      target = target_steer
      test.hold_t = test.hold_t - dt
      if test.hold_t <= 0 then
        test.phase = "RAMP_DOWN"
        test.dir   = 1 -- return to 0
        -- keep time at duration; next frame decreases
      end

    else -- RAMP_DOWN
      update_nTime(dt)
      target = math.lerp(0, target_steer, test.nTime)

      if (test.nTime <= 0.0) and (test.dir == 1) then
        test.active = false
        do_cooldown()
        ac.log("Test complete: " .. (os.clock() - benchmark))
        ac.log()
      end
    end

    ac.debug("A1] TARGET", target, target_steer, 0)
    steerPID:setTarget(target)
    auto_steer(LF_SLIP)
  end

  -- -------------------------------------------------------------------------
  -- Cooldown: smooth return to user steering
  if (test.cooldown) then
    ac.log("Cooldown " .. os.clock())
    -- Use direct input as override; still uses auto_steer lag for smoothness
    auto_steer(LF_SLIP, DATA.steer)
  end
end

-- ===========================================================================
function do_cooldown()
  test.cooldown = true
  setTimeout(function()
    test.cooldown = false
    next_test()
  end, 1) -- seconds
end

-- ===========================================================================
-- Auto-test iterator (called in cooldown)
function next_test()
  if (auto_test.index < 0) then return end
  if (auto_test.index == #auto_test.loads) then
    ac.log("Autotests finished")
    auto_test.index = -1
    return
  end
  auto_test.index = auto_test.index + 1
  ac.log("Autotest #" .. auto_test.index)
  trigger_old = 999
end

-- ===========================================================================
-- Speed control: apply PID output to throttle
function auto_speed()
  local output = speedPID:update(CAR.speedKmh)
  ac.debug("Z] PID_SPEED", output)

  local auto_gas = interpolate(
    0, 20,     -- input range
    output,    -- input
    0, 0.5     -- output (throttle) range
  )

  DATA.gas = math.clamp(DATA.gas + auto_gas, 0, 1)
end

-- ===========================================================================
-- Steering control: map PID to normalized steering, with lag
-- override_steer: optional direct input to blend during cooldown
function auto_steer(current_slip, override_steer)
  local output = steerPID:update(current_slip)
  ac.debug("Z] PID_STEER", output)

  local auto_steer = interpolate(
    -1000, 1000,  -- input range
    -output,      -- input (note sign)
    -1, 1         -- output range (normalized steering)
  )

  if (override_steer) then auto_steer = override_steer end

  auto_steer = math.applyLag(
    CAR.steer / CAR.steerLock, -- current
    auto_steer,                -- target
    0.98,                      -- lag multiplier
    SIM.dt                     -- delta time
  )

  DATA.steer = math.clamp(auto_steer, -1, 1)
end

-- ===========================================================================
-- Utilities

-- Map input in [x1,x2] to output in [y1,y2]
function interpolate(x1, x2, input, y1, y2)
  input = math.clamp(input, x1, x2)
  return y1 + ((input - x1) / (x2 - x1)) * (y2 - y1)
end

-- Simple PID object
function newPID(p, i, d)
  return {
    P = p or 0,
    I = i or 0,
    D = d or 0,
    integral = 0,
    prevError = 0,
    output = 0,
    target = 0,
    limit = nil,
    deadzone = nil,

    update = function(self, current)
      local dt = SIM.dt
      local error = self.target - current
      self.integral = self.integral + error * dt
      local derivative = (error - self.prevError) / dt

      -- Adaptive leash for integral term
      self.integral = math.clamp(self.integral, error * self.P, error * self.P)

      -- PID
      self.output = self.P * error + self.I * self.integral + self.D * derivative
      self.prevError = error

      if (self.limit) then
        self.output = math.clamp(self.output, -self.limit, self.limit)
      end
      return self.output
    end,

    setTarget = function(self, newTarget)
      self.target = newTarget
    end,

    reset = function(self)
      self.integral = 0
      self.prevError = 0
      self.output = 0
    end,

    debug = function(self)
      return string.format(
        "Err: %.2f  Int: %.2f  Der: %.2f  Out: %.2f",
        self.target - self.prevError, self.integral, self.prevError, self.output
      )
    end
  }
end