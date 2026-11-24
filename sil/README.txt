MK12 VCU Software-in-the-Loop (SIL) Kit for Assetto Corsa
=========================================================

This kit runs the real MK12 VCU firmware (torque map, LPF/slew filter,
EV.3.3.1 power limit, and Simulink traction control) alongside Assetto Corsa
over shared memory. Zero Python, git, or compiling required.

1. Install the car script (one-time setup)
------------------------------------------
1. Open your Assetto Corsa MK12 car folder:
     assettocorsa\content\cars\bfr_mk12\
2. If a `data.acd` file exists in that folder, rename it (e.g. `data.acd.bak`).
   Assetto Corsa ignores `data\` whenever `data.acd` is present.
3. Back up your existing `data\script.lua` and `data\drivetrain.ini`, then copy
   all files from this kit's `data\` folder (`script.lua`, `vcu_bridge.lua`,
   `brakes.lua`, `drivetrain.ini`) into:
     assettocorsa\content\cars\bfr_mk12\data\

2. Drive with the default `base` profile (150 Nm)
-------------------------------------------------
1. Double-click `bfr_vcu.exe` (or drag `base.yaml` onto it). A console window
   opens showing the active profile name, 8-character hash, TC mode (`SHADOW`),
   the CSV log path (`logs\vcu_base_*.csv`), and `waiting`.
2. Start Assetto Corsa with the `bfr_mk12` car and put it in 1st gear.
3. The console switches to `LINKED` and CSP's Lua Debug app shows:
     01 VCU link      CONNECTED
     02 profile hash  <matches the console>
     06 TC status     SHADOW (ready) / SHADOW (would cut)
     09 lag ticks     1

   Safety note: if `bfr_vcu.exe` is not running or closes mid-session,
   `vcu_bridge.lua` holds 0 Nm in 1st gear so you never drive on the wrong
   torque map by accident.

3. Switch profiles (`main_220nm.yaml` or custom `.yaml`)
--------------------------------------------------------
This kit includes two main-equivalent linear profiles (-30 Nm regen):
  - `base.yaml`        150 Nm (default)
  - `main_220nm.yaml`  220 Nm

1. Drag and drop `main_220nm.yaml` (or any custom `.yaml` exported from
   https://bruinformula.github.io/mk12-assetto-sim/) onto `bfr_vcu.exe`,
   or run `bfr_vcu.exe --profile path\to\name.yaml`.
2. Verify the 8-character profile hash matches between Profile Studio,
   the `bfr_vcu.exe` console, and AC's Lua Debug app (`02 profile hash`).
3. To switch profiles mid-session, close `bfr_vcu.exe` and drag a different
   `.yaml` onto it — Assetto Corsa reconnects automatically without restarting.
4. Every run saves a 100 Hz telemetry CSV in `logs\` next to `bfr_vcu.exe`.
   See `TEST_GUIDE.md` for the full test & log-collection checklist.

4. Command-line options
-----------------------
  bfr_vcu.exe                        Default `base` (150 Nm), TC in shadow mode, CSV log on
  bfr_vcu.exe main_220nm.yaml        Load profile via drag-and-drop (TC shadow mode)
  bfr_vcu.exe --profile file.yaml    Load custom profile (explicit flag)
  bfr_vcu.exe --tc-active            Enable live Simulink TC torque cuts
  bfr_vcu.exe --tc-shadow            Run Simulink TC in shadow mode (default)
  bfr_vcu.exe --no-tc                Bypass Simulink TC completely
  bfr_vcu.exe --log path.csv         Write 100 Hz telemetry CSV to a specific path
  bfr_vcu.exe --no-log               Disable CSV logging
  bfr_vcu.exe --sleep                Low-CPU 1 ms poll mode (slight extra lag)
  bfr_vcu.exe --version              Print kit version and embedded mk12-vcu SHA
