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

### Planned: torque map maker

A tool where drivers build their own pedal map and get both:

- the sim files for this repo, to try the map in Assetto, and
- the `torqueMap_table[]` values for the VCU, to run the same map on the car.

One map file would be the source of truth for both, so the sim and the car can't drift apart.

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
