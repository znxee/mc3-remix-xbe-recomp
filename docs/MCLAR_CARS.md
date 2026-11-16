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

## What comes from the MCLAR file

The converter is a Python tool of the modding toolkit, not part of this repository. It takes the MCLAR car folder and
writes a car pack with a new car name (same length as the PS2 car whose object layout it fills, so no pointer moves and
the retail car stays as it is). Every value the `.psppck` holds is taken from it:

* geometry of every part (37 LOD slots, 31 of them paged), skeleton (113 bones), bone tags and every bone-id table of
  `rmcCarModelType`, wheel records, nitro and exhaust points, part lists;
* both collision bounds (physics material 1 becomes MC3's 29, the id every retail car uses);
* the shader group, in MCLAR's order, built on MC3's shader templates (type, flags, VU passes and local names are the
  same in all 94 retail cars), and the trim texture;
* every tune object word (VehSim/VehGyro/engine/gearbox/wheels/axles/aero/input/damage/zone/VehMods, the material),
  window damage, LOD distances, bounding sphere and box, occluders (none), car index;
* the audio graph: engine, exhaust and turbo blocks, impacts, wind, tire skids, suspension, gear shifts (none),
  backfires, warble and boost-mix fields; its sample banks, rebuilt as XACT wave banks
  (`audio\xbanks\<bank>.xwb` + `.td`) when the Xbox `banks.dat` lacks them;
* its `.carcfg`, `.damage`, `default.mccarcustom`, `.mcgarage`, cameras and `vehicle.lst` values.

What is not in the MCLAR file comes from MC3 itself: the class layout and vtable residues, fields that hold one value in
every retail car (spotlight parameters: MCLAR has none), and fields MC3 has and MCLAR lacks (turbo warble,
`HighPitchCurveEngageGear`, some engine-mix words), which take the most common retail value. Text tune keys MCLAR lacks
and the part-catalog indices (rims, tires, brakes, exhaust tips) are MC3's. Lamp lenses are MCLAR's plain shiny shader,
so the tail lights are white, not red.

Rules found on the way:

* MC3's PS2 compiler puts a vptr after the data of a non-polymorphic base, MCLAR's puts it first; with MCLAR's `0xCD`
  member padding dropped, the tune objects line up word for word.
* A part-list choice must not name a bone that owns a resident mesh: `DeletePagedParts` deletes every LOD mesh on the
  chosen bone when the car is released ("memMemoryAllocator::FindHeap failed").
* A texture reference must not be shared between shader slots: the shader constructor relocates its name pointer
  every time it runs.

Car packs serve any file the game lacks, so the wave banks need no new code: the game opens a car's engine bank as
`AUDIO\XBANKS\<bank>.td` and `.xwb` through `Stream::Open`, and the pack answers. The banks are 16-bit PCM
(`WAVEBANKMINIWAVEFORMAT` tag 0), one entry per `.td` sound, looping over the PSP loop points.

## How it was checked

Corvette Z06 2007 from MCLAR, added as a new car next to the retail 2003 Z06. Legacy profile, 128 MiB guest, an
isolated hdd instance:

* `vehicle.lst: 21 lines added by packs`; the car is listed in **Arcade > P1** by its pack name;
* San Diego Cruise, 64 s with throttle, pause, **Go to Menus**: no `exception` or `KeBugCheck`; again 43 s with
  steering;
* every stage (collision, skeleton, shaders, LODs, tune, model, audio) was driven the same way, 64 s each, no
  exception; the converted main file still carries about 580 KB of the PS2 car's old meshes that nothing points at;
* the engine, exhaust and backfire banks were served from the pack and the log has no XACT error. The sound itself was
  not listened to.

Not covered: choosing MCLAR's optional parts in the garage (the pack holds them, the shop menus were not driven), its
wide body, rims and exhaust pages; the car's shops (a new car runs the stock shops, `docs/CAR_PACKS.md`); cars other than
the Corvette (the converter has one car profile).
