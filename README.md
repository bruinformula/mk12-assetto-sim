# MK12 Assetto Corsa Sim

Scripts and powertrain/torque config for the BFR cars in Assetto Corsa (with Custom Shaders
Patch). The goal is for the sim's torque logic to match the real MK12 VCU
([bruinformula/mk12-vcu](https://github.com/bruinformula/mk12-vcu)), so drivers practice on
the same maps they race with.

## The vehicle model lives on Box

This repo only holds the code and torque config. The vehicle model (3D models, tires, aero,
suspension, dampers) and in-game assets stay on Box:

**[BFR → MK12 → 07 Vehicle Dynamics → Assetto Sim](https://app.box.com/folder/414500198007)**

Folders here mirror that Box folder, so a file in `bfr_mk12/data/` here goes in
`bfr_mk12/data/` there.

## Setup

1. Download the car folder (e.g. `bfr_mk12`) from Box into
   `assettocorsa\content\cars\`.
2. Copy this repo's `bfr_mk12/data/` over the car's `data/` folder, replacing files.
3. If the car folder has a `data.acd`, rename or delete it. AC reads `data.acd` instead of
   `data/` when both exist.

## What's here

```
bfr_mk12/data/
  script.lua        CSP car script entry point; runs brakes() and regen() every frame
  brakes.lua        Rear brake torque from pedal force, master cylinder, caliper and disc
                    (reads brakes.ini), applied as electric torque on the rear wheels
  regen.lua         Off-throttle regen: -30 Nm above 5 km/h
  engine.ini        Motor curve (Motor/base.lut) and driver-selectable torque maps [MAP]
  Motor/            Motor torque vs rpm (base.lut, ~102 Nm flat to 5500 rpm)
  Torque/           Torque map multipliers on the motor curve, one file per map
  drivetrain.ini    Single speed (3.909:1), RWD, diff power/coast/preload
  electronics.ini   ABS and TC (both off)
  setup.ini         In-game setup screen, including the TORQUE tab
tire_test_machine/data/
  script.lua        Automated slip-angle tester for the tire test rig (horn to start)
  *.lut, engine.ini Torque maps for the rig
```

## Torque maps

Drivers switch maps in-game with AC's engine map keys. Each map multiplies `Motor/base.lut`:

| Map | File | Multiplier | Approx. torque |
|---|---|---|---|
| 0 | `Torque/140nm.lut` | 2.0 | ~205 Nm |
| 1 (default) | `Torque/120nm.lut` | 1.0 | ~102 Nm |
| 2 | `Torque/90nm.lut` | 0.75 | ~77 Nm |
| 3 | `Torque/70nm.lut` | 0.583 | ~60 Nm |

To add a map: add a LUT to `Torque/`, add a `MAP_n=` line to `engine.ini`, open a PR.

## Matching the VCU

The sim and the VCU don't model torque the same way yet:

| | VCU (`Core/Inc/torque_map.h`) | Sim |
|---|---|---|
| Pedal → torque | 11-point pedal map, fraction of `MAX_TORQUE` | Linear throttle (no pedal map) |
| Max torque | `MAX_TORQUE` = 80 Nm | Map-dependent, ~60–205 Nm |
| Torque vs rpm | Not modeled | Flat to 5500 rpm, then cut |
| Filtering | LPF (τ = 0.05 s) + 2000 Nm/s slew limit | `RESPONSE_TIME=0.01` in engine.ini |
| Regen | -30 Nm max, **disabled** (`REGEN_ENABLED 0`) | -30 Nm, always on |

## Known issues

- Map file names (`140nm`, `120nm`, ...) don't match the torque they produce (see table above).
- `setup.ini`'s TORQUE tab points at `power_maps.lut`, which isn't in `bfr_mk12/data/`, so
  that selector doesn't work. The engine map keys do.
- `Torque/base.lut` isn't referenced by anything.
- Box has a `bfr_mk12/data/NEW/` work-in-progress copy (torque maps commented out, 62.9 Nm
  coast torque). It isn't in this repo.

## Contributing

Make changes on a branch and open a PR. In the PR, say what changed and why (e.g. "regen
-30 → -20 Nm, drivers said lift-off was too abrupt"). That history is the reason this repo
exists.

Never commit model or asset files. `.gitignore` blocks the usual ones (`*.kn5`, `*.acd`,
tires, aero, suspension, dampers, skins, sounds).

## CSP Lua SDK (reference)

`reference/acc-lua-sdk` is the [CSP Lua SDK](https://github.com/ac-custom-shaders-patch/acc-lua-sdk)
(MIT) as a git submodule, pinned to a commit. It is for reading and editor autocomplete only:
nothing in it is copied into the car, and AC doesn't need it. It holds the `ac.*` and car-physics
API definitions (`ac_car_cphys.lua`), the shared-memory helper (`common/ac_extras_connectmmf.lua`)
and the script sandbox rules (`common/secure.lua`).

```
git submodule update --init
```

With VS Code and the Lua extension (LuaLS), `.luarc.json` loads it as a library, so `ac.*` calls
in `bfr_mk12/data/*.lua` get completion and type checks. To move the pin, check out a newer commit
in the submodule and commit the change. The SDK tracks the latest CSP build, so check it against
the CSP version the team runs.

## SIL VCU & Profile Studio

`sil/` compiles the real HAL-free MK12 VCU firmware (`vendor/mk12-vcu`, submodule tracking
`bruinformula/mk12-vcu`'s `assetto` branch) into a Windows executable (`bfr_vcu.exe`) that
talks to Assetto Corsa over shared memory at 333 Hz — running the real 1 kHz torque map,
LPF/slew filter, EV.3.3.1 power limit, and Simulink traction control with 100 Hz / 0.1 Nm
inverter quantization.

- `sil/bridge_layout.h`: shared-memory bridge v2 (pedals, RPM, gear, 4 wheel speeds, ground
  speed, IMU → VCU; torque command, driver request, TC telemetry, slip ratio, profile hash ← VCU)
- `sil/bfr_vcu.c`, `sil/profile_loader.c`: real VCU harness with runtime YAML profile loading
  (supports drag-and-drop `.yaml`, `--profile`, `--no-tc`, `--sleep`)
- `sil/car/vcu_bridge.lua`, `sil/car/script.lua`: thin CSP car script with fail-closed 0 Nm
  fallback if `bfr_vcu.exe` isn't running or disconnects (>50 ms)
- `sil/build_pages.py`: builds the [MK12 Profile Studio](https://bruinformula.github.io/mk12-assetto-sim/)
  synced to `vendor/mk12-vcu/profiles/base.yaml` for GitHub Pages (`.github/workflows/pages.yml`)

```
make -C sil test       # reference CSV parity + LuaJIT <-> C shared-memory SIL tests
make -C sil kit        # sil/build/bfr_vcu_kit/ (Windows static exe, needs mingw-w64)
git submodule update --remote vendor/mk12-vcu   # pull newer VCU firmware from assetto
```

CI (`.github/workflows/sil.yml`) runs `make -C sil test`, cross-compiles `bfr_vcu.exe`, and
uploads `bfr_vcu_kit` as a downloadable artifact.

## SIL spike (historical)

`spike/` validated the shared-memory bridge on Windows AC + CSP (passed at 333 Hz / 1-tick
lag; see [spike/RESULTS.md](spike/RESULTS.md)). Kept as historical reference.

- `car/spike.lua`, `car/script.lua`: spike car script. Debug readouts for tick rate, gas,
  torque response and the program link
- `vcu_dummy.c`: stand-in VCU program, 50 Nm past 10% pedal
- `bridge_layout.h`: the v1 shared-memory block

```
make -C spike test     # car script against vcu_dummy, on this machine
make -C spike kit      # spike/build/bfr_vcu_spike/, needs mingw-w64
```
