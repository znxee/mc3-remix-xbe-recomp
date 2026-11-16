# Midnight Club: LA Remix (PSP) cars

How a car from Midnight Club: LA Remix for the PSP ("MCLAR", `.psppck` files) is added to the game. Task MC3-0036.
No code in this repository is specific to it: the car is converted offline into a PS2 car and added as a car pack
(`docs/CAR_PACKS.md`), which the PS2 car loader (`docs/PS2_CARS.md`) turns into Xbox data at load time. Nothing changes
unless `--mc3-car-packs` points at such a pack.

## Why offline, and not MCLAR's own code

MCLAR's car files are load-in-place images like the PS2 `.pck`: there is no parser to reuse, only a relocation the
loader already does. What MCLAR's code would hand back is PSP layout: display-list geometry with 14-byte vertices,
every C++ member aligned to 16 bytes with `0xCD` fill, newer classes (its vehicle audio root has three more slots than
MC3's), CLUT8 textures in the PSP swizzle. A PSP-to-Xbox conversion layer would still be needed, the same conclusion as
for the PS2 executable (MC3-0031). Per-game Allegrex recompilers exist for other PSP games, but they need a decrypted
executable and still return PSP-layout data. Converting to the PS2 layout reuses the whole PS2 car path instead.

## What the conversion keeps

The converter is a Python tool of the modding toolkit, not part of this repository. It takes the MCLAR car folder, a
PS2 donor car of the same shape for the object layout, and writes a car pack with a new car name (same length as the
donor's, so no pointer moves and the retail car stays as it is):

| From the MCLAR car | From the PS2 donor |
| --- | --- |
| Geometry of the stock parts, skeleton anchors, trim texture | Object layout, shader objects, collision bounds |
| Performance: every VehSim/VehGyro/engine/gearbox/wheel/axle/aero field by name, VehMods lists | Spotlight parameters (MCLAR has none) |
| Audio: turbo, engine and exhaust blocks (bank, RPM ranges, distances, curves), backfire bank | Gear-shift and impact sounds |
| Its sample banks, rebuilt as XACT wave banks (`audio\xbanks\<bank>.xwb` + `.td`) | |
| Its `.carcfg`, `.damage`, `default.mccarcustom`, `.mcgarage`, cameras, `vehicle.lst` values | Keys MC3 has and MCLAR lacks; part-catalog indices (rims, tires, brakes, exhaust tips) |

Car packs serve any file the game lacks, so the wave banks need no new code: the game opens a car's engine bank as
`AUDIO\XBANKS\<bank>.td` and `.xwb` through `Stream::Open`, and the pack answers. The banks are 16-bit PCM
(`WAVEBANKMINIWAVEFORMAT` tag 0), one entry per `.td` sound, looping over the PSP loop points.

## How it was checked

Corvette Z06 2007 from MCLAR, added as a new car next to the retail 2003 Z06. Legacy profile, 128 MiB guest, an
isolated hdd instance:

* `vehicle.lst: 21 lines added by packs`; the car is listed in **Arcade > P1** by its pack name;
* San Diego Cruise, 64 s with throttle, pause, **Go to Menus**: no `exception` or `KeBugCheck`; again 43 s with
  steering;
* the PS2 loader consumed 86.56 % of the converted main file (the rest are donor objects the port leaves unused) and
  99.35-99.91 % of the external part files;
* the engine, exhaust and backfire banks were served from the pack and the log has no XACT error. The sound itself was
  not listened to.

Not covered: MCLAR's optional parts, wide body, rims and exhaust pages, damage and collision; the car's shops (a new
car runs the stock shops, `docs/CAR_PACKS.md`); cars other than the Corvette (the converter has one car profile).
