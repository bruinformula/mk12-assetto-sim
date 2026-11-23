# MK12 SIL VCU — Assetto Corsa Test Guide

This guide walks through validating the **real MK12 VCU firmware** (`bfr_vcu.exe`) in Assetto Corsa with two `main`-equivalent profiles (**150 Nm** and **220 Nm**), **Simulink traction control in shadow mode**, and **automatic 100 Hz CSV telemetry logging**.

## What's in `bfr_vcu_kit`

- `bfr_vcu.exe` — real MK12 C firmware (1 kHz torque map, 50 ms LPF + 2000 Nm/s slew, EV.3.3.1 power cap, Simulink TC v2.0, 100 Hz / 0.1 Nm inverter commands).
- `base.yaml` — **150 Nm** default profile (linear pedal from 5%, flat `-30 Nm` lift-off regen above 6 km/h fading out 6 → 5 km/h, cancels on >5% brake).
- `main_220nm.yaml` — **220 Nm** profile (same linear curve, regen, and filter as `base.yaml`, only `max_nm: 220`).
- `data/` — `script.lua`, `vcu_bridge.lua`, `brakes.lua`, `drivetrain.ini` (clutch `MAX_TORQUE` bumped to 250 Nm so 220 Nm doesn't slip).

> **Why TC defaults to shadow mode:** By default, `bfr_vcu.exe` runs Simulink TC at 1 kHz and logs what it *would* cut (`05 TC model Nm`, `07 rear slip ratio`, and `SHADOW (would cut)`), **without modifying the torque sent to the car**. That lets us verify AC's wheel-speed ordering/signs and TC behavior in the logs before turning on live TC cuts.
>
> **Heads-up on high RPM:** The EV.3.3.1 power cap (`75 kW × 0.85` mechanical) is active in `bfr_vcu.exe`. At `150 Nm` it tapers above ~`4060 rpm`; at `220 Nm` it tapers above ~`2770 rpm` (down to ~`101 Nm` at `6000 rpm`). That's expected.

---

## Step 0: One-Time Setup

1. Unzip `bfr_vcu_kit` somewhere convenient (e.g. `Desktop\bfr_vcu_kit\`).
2. Open your Assetto Corsa MK12 car folder:
   `assettocorsa\content\cars\bfr_mk12\`
3. If `data.acd` exists, rename it to `data.acd.bak` (AC ignores `data\` whenever `data.acd` is present).
4. Back up your existing `data\script.lua` and `data\drivetrain.ini`, then copy all 4 files from `bfr_vcu_kit\data\` into `bfr_mk12\data\`:
   - `script.lua`
   - `vcu_bridge.lua`
   - `brakes.lua`
   - `drivetrain.ini`
5. In Assetto Corsa, enable CSP's **Lua Debug** app on screen so rows `01`–`12` are visible alongside the `bfr_vcu.exe` console window.

---

## Step 1: Test `base.yaml` (150 Nm, TC Shadow Mode)

1. **Double-click `bfr_vcu.exe`** (or drag `base.yaml` onto `bfr_vcu.exe`).
   - Confirm the console prints:
     - `profile: base (hash 1c599911) | max 150 Nm | regen ON | TC SHADOW (telemetry only)`
     - `csv log: ...\logs\vcu_base_<timestamp>.csv (100 Hz)`
2. Load `bfr_mk12` in Assetto Corsa (any track / skidpad) and shift into **1st gear**.
3. Check the **Lua Debug** app:
   - [ ] `01 VCU link` = `CONNECTED`
   - [ ] `02 profile hash` = `1c599911` (matches the console window)
   - [ ] `06 TC status` = `SHADOW (ready)`
   - [ ] `08 VCU state` = `DRIVE`
   - [ ] `09 lag ticks` ≈ `1` (steady, `11 answers/s` ≈ `333`, `12 tick Hz` ≈ `333`)
4. **Drive checks (2–3 minutes):**
   - [ ] **Linear pedal & 150 Nm:** Roll on throttle smoothly (`0% → 50% → 100%`). Confirm `03 torque cmd Nm` ramps cleanly up to `150.0 Nm` below ~4000 rpm and tapers smoothly toward ~`101 Nm` at 6000 rpm.
   - [ ] **Flat `-30 Nm` regen:** Lift completely off gas above ~20 km/h (brake untouched). Confirm `03 torque cmd Nm` ramps smoothly to `-30.0 Nm` and feels consistent.
   - [ ] **Regen fade & brake cancel:** Coast down past `6 → 5 km/h` and confirm regen fades cleanly to `0.0 Nm`. Then repeat a lift-off at speed and tap the brake (>5%) — confirm `03 torque cmd Nm` snaps to `0.0 Nm` immediately with no weird jerk from `brakes.lua`.
   - [ ] **Shadow TC sanity (hard launch + corner exit):** Do 2–3 hard standing launches and aggressive throttle exits to provoke rear wheelspin. Watch:
     - `07 rear slip ratio`: near `0.000` cruising straight, **positive** (`+0.15` to `+0.50+`) when spinning the rears.
     - `06 TC status`: flips to `SHADOW (would cut)` during wheelspin.
     - `05 TC model Nm`: drops below `04 driver req Nm` while `03 torque cmd Nm` stays full driver torque (shadow mode).
5. **Take 1–2 screenshots** showing Lua Debug + the `bfr_vcu.exe` console (ideally one at full throttle / wheelspin and one during `-30 Nm` regen).

---

## Step 2: Fail-Closed Fallback & Mid-Session Switch to `main_220nm.yaml` (220 Nm)

**Keep Assetto Corsa running on track** — do not restart the session:

1. **Close `bfr_vcu.exe`** (press `Ctrl+C` in its console or click `X`).
2. Check Lua Debug in Assetto Corsa:
   - [ ] `01 VCU link` switches to `LOST (0 Nm fallback)`.
   - [ ] Press the gas pedal in 1st gear — confirm `03 torque cmd Nm` stays **`0.0`** and the car does not accelerate. *(Take a quick screenshot if easy.)*
3. **Drag and drop `main_220nm.yaml` directly onto `bfr_vcu.exe`.**
   - Confirm the console opens with `profile: main_220nm (hash 2eabfaa7) | max 220 Nm | regen ON | TC SHADOW (telemetry only)` and starts a new CSV in `logs\vcu_main_220nm_<timestamp>.csv`.
4. Back in Assetto Corsa (still in the same session):
   - [ ] `01 VCU link` automatically returns to `CONNECTED`.
   - [ ] `02 profile hash` updates to `2eabfaa7` (matches the `main_220nm` console hash).
   - [ ] **Full 220 Nm & no clutch slip:** Floor it from low speed — confirm `03 torque cmd Nm` reaches `220.0 Nm` and accelerates noticeably harder than 150 Nm without the engine flaring/clutch slipping.
   - [ ] **High-RPM power cap:** Run up through 3000 → 6000 rpm — confirm torque tapers smoothly from `220 Nm` down to ~`101 Nm` at 6000 rpm.
   - [ ] **Heavy wheelspin / shadow TC:** Do 2–3 hard launches and corner exits at 220 Nm so the CSV captures strong TC shadow interventions.
5. **Take a screenshot** of Lua Debug + console showing `main_220nm` connected.
6. Close `bfr_vcu.exe` cleanly (`Ctrl+C`) so the CSV flushes cleanly.

---

## Step 3: Live Simulink TC Test (`--tc-active`) *(run after Step 1–2 look sane)*

If `07 rear slip ratio` was positive under wheelspin (not inverted or noisy) and `05 TC model Nm` looked reasonable in Steps 1–2:

1. Open PowerShell or Command Prompt inside `bfr_vcu_kit\`.
2. Run:
   ```bat
   bfr_vcu.exe main_220nm.yaml --tc-active
   ```
   *(Or `bfr_vcu.exe base.yaml --tc-active` for 150 Nm.)*
3. Confirm console shows `TC ACTIVE (--tc-active)` and Lua Debug shows `06 TC status = READY`.
4. Do 2–3 standing launches and aggressive corner exits:
   - [ ] `06 TC status` flips `READY` → `ACTIVE` during wheelspin.
   - [ ] `03 torque cmd Nm` drops to match `05 TC model Nm`.
   - [ ] Note whether TC feels smooth and helpful vs too aggressive / oscillating / falsely cutting on straights.
5. Close `bfr_vcu.exe` cleanly (`Ctrl+C`).

---

## Step 4: Optional `--sleep` Check

If the default spinning loop noticeably impacts AC FPS or CPU temp, run a quick 30-second check:
```bat
bfr_vcu.exe base.yaml --sleep
```
Note `09 lag ticks`, `11 answers/s`, and CPU usage compared to default mode.

---

## What to Send Back

Please zip and send:
1. **The `logs\` folder** inside `bfr_vcu_kit\` (all `vcu_*.csv` files — 100 Hz telemetry for every run).
2. **CSP log:** `Documents\Assetto Corsa\logs\custom_shaders_patch.log` (note your CSP version too).
3. **Screenshots** of Lua Debug + `bfr_vcu.exe` console from:
   - `base.yaml` (150 Nm, shadow TC)
   - `LOST (0 Nm fallback)` *(optional)*
   - `main_220nm.yaml` (220 Nm, shadow TC)
   - `--tc-active` *(if run)*
4. **Quick driver notes:**
   - How did 150 Nm vs 220 Nm pedal response feel (smoothness, LPF lag, power cap taper)?
   - How did `-30 Nm` lift-off regen, 6→5 km/h fade, and brake cancel feel?
   - Any clutch slip at 220 Nm, FPS drop, or lag spikes?
   - If you ran `--tc-active`, how did TC feel on launches and corner exits?
