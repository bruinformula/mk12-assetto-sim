# VCU sim spike: checklist

About an hour on a Windows PC with Assetto Corsa. You drive the MK12 a few laps
twice, once with a small helper program running, and send back screenshots. It
tells us whether the car can be driven by the real VCU code later.

## 0. Before you start

- **Content Manager** (free version is fine): https://assettocorsa.club/content-manager.html
- **Custom Shaders Patch (CSP):** in Content Manager, Settings → Custom Shaders Patch →
  install or update to the latest version. Write the CSP version down; it's on that page.
- **The MK12 car:** `bfr_mk12` from Box in `assettocorsa\content\cars\`, with this repo's
  `bfr_mk12/data/` copied over its `data/` folder (see the repo README). If the car
  folder has a `data.acd`, rename it to `data.acd.off`.

## 1. Install the spike

1. In `content\cars\bfr_mk12\data\`, rename `script.lua` to `script.lua.bak`.
2. Copy the three files from this kit's `data\` folder (`script.lua`, `spike.lua`,
   `brakes.lua`) into `content\cars\bfr_mk12\data\`.
3. Keep `vcu_dummy.exe` anywhere, for example the desktop.

## 2. Part A: without the program

1. Start a practice session with the MK12 on any track.
2. Open the **Lua Debug** app from the apps bar on the right side of the screen.
   It lists rows `01 mode` to `22 CSP version`.
3. Put it in first gear. `01 mode` should say `LOCAL`.
4. Drive a lap. Floor it at least once on a straight, and lift completely off the pedal
   at least once.
5. Take a screenshot with the Lua Debug app visible.

| Row | Pass looks like |
|---|---|
| `03 tick Hz (wall)` | about 333 (300 to 370 is fine) |
| `09 gas check` | `follows the pedal` |
| `15 car responds` | `YES` |
| `16 shared memory` | `ok` |

## 3. Part B: with the program

1. With AC still running, double-click `vcu_dummy.exe`. If Windows says it protected
   your PC, click **More info → Run anyway**. It's our program and doesn't use the
   network. A black window opens with a status line every second.
2. Back in AC, `01 mode` should say `EXE` and `17 exe link` should say `CONNECTED`.
3. Drive a lap. The car gets a flat 50 Nm whenever the pedal is past 10%.
4. Screenshot the Lua Debug app **and** the black program window.
5. Close the program window. Within a moment `01 mode` goes back to `LOCAL` and
   `17 exe link` says `LOST`. The car should stay drivable.

| Row | Pass looks like |
|---|---|
| `17 exe link` | `CONNECTED` while the program runs |
| `19 exe answers/s` | about the same as the tick rate |
| `20 exe lag ticks` | 1 or 2 |
| Program window | `LINKED`, and its gas follows your pedal |

Optional: run it again from a command prompt as `vcu_dummy.exe --sleep` (it then uses
less CPU) and note `20 exe lag ticks`.

## 4. Put it back

Delete `script.lua` and `spike.lua` from the car's `data\`, and rename
`script.lua.bak` back to `script.lua`.

## 5. Send back

- The CSP version
- The screenshots from Parts A and B
- The CSP log, usually `Documents\Assetto Corsa\logs\custom_shaders_patch.log`
  (search it for `spike:`)
- Anything odd: the car pulling on its own, no response, AC crashing, or a row
  showing `FAILED` or `STUCK`
