# MK12 Assetto Corsa Sim

Scripts and powertrain/torque config for the BFR Assetto Corsa cars, laid out the same as the
Box folder `BFR → MK12 → 07 Vehicle Dynamics → Assetto Sim`.

The vehicle model (3D models, tires, aero, suspension, dampers) and in-game assets are **not**
in this repo. Get those from Box, then copy the files from this repo over the matching
folders in `assettocorsa\content\cars\`.

- `bfr_mk12/data/` — MK12 car scripts (`script.lua`, `brakes.lua`, `regen.lua`), motor curve,
  and torque maps (`engine.ini` `[MAP]`, `Torque/`).
- `tire_test_machine/data/` — tire test rig: slip-angle tester script and its torque maps.
