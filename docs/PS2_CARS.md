# PS2 cars

An opt-in loader that runs the **PlayStation 2** version's car files in the Xbox recomp: `vp_*.pck`, its `_o` and `_g`
variants and the `*.mesh.pck` part files (bumpers, hoods, side skirts, lights, kits). The PS2 files are read at run time
from the user's own copy and rebuilt in the layout the Xbox loaders expect; nothing is converted ahead of time, and no
PS2 or Xbox game file is part of this repository. It is off unless asked for: without the flag every car loads from the
Xbox assets exactly as before.

Everything here is from the Xbox build of Midnight Club 3: DUB Edition (see `config/supported_xbe.txt`). Addresses are
guest virtual addresses in that build. Task MC3-0021.

## Turning it on

```text
--mc3-ps2-cars=<folder>
```

`<folder>` is a PS2 `resources\vehicle` tree: `<folder>\vp_jetta_03\vp_jetta_03.pck`, `vp_jetta_03_o.pck`,
`vp_jetta_03_g.pck` and the `vroot_*.mesh.pck` parts beside them. A car or part with no PS2 file, or one the parser
refuses, loads from the Xbox assets as usual, and the log says why. The flag works in every render profile.

| Flag | Default | What it does |
| --- | --- | --- |
| `--mc3-ps2-cars=<folder>` | off | Loads vehicles (resource type `0x38D030`) and meshes (type 22) from the PS2 files in `<folder>` when they exist. |

Each converted file logs one line, for example:

```text
MC3PS2CAR vp_jetta_03.pck: 310464 bytes: 99.87% consumed (header 128, typed 285017, by pointer 20180, padding 4747),
0 dead, 392 unclaimed; 20 meshes, 202 VU batches, 5061 triangles, 6315 vertices, 62 shaders, 3 textures (65664 texels)
```

and, below it, anything that could not be carried over in full (`MC3PS2CAR   ...`).

## How it works

`mcResourceMgr::LoadResource` (`0x2105F0`, which jumps into `0x2126C0`: open `<name>.xbck`, read the 0x80-byte header,
check version 1 and the type, allocate the body with `0x20DB20(size, 0x80)`, read it) is answered by
`src/game/ps2_car_loader.cpp` for a vehicle or a mesh whose PS2 file exists. `src/game/ps2_vehicle.cpp` parses the PS2
image and returns an Xbox body; the buffer comes from the game's own allocator and the game's relocators, shader
factory, texture binder, physics and renderer take it from there.

Both builds serialize the same C++ object graph with the same root, so most objects keep their layout: the body is
copied whole and keeps the PS2 virtual base (the game relocates every pointer by `buffer - base`). What differs is
rewritten in place when the Xbox object is smaller, or appended after the PS2 body when it is larger:

| Object | PS2 | Xbox | Done |
| --- | --- | --- | --- |
| Car model type (`root+0x08`) | 0x3890; vptr after the base data; matrices 16-byte aligned | 0x3880; vptr first; 4-byte aligned | rewritten in place |
| LOD table | kind/count word, then vptr | vptr, then kind/count | rewritten in place |
| Mesh (`rmcModelGeom`) | VIF packets: one UNPACK stream per attribute and VU batch, ADC bit in the normal | D3D vertex buffer (float3 position, NORMPACKED3 normal, D3DCOLOR, float2 uv [, float2 uv]) and one indexed strip per group | VIF emulated (cycle, mask, row and offset modes), header in place, buffers appended |
| Texture | 8/4 bpp indexed, GS-swizzled, CLUT with 0..0x80 alpha | `D3DTexture` in the descriptor | swizzled A8R8G8B8, appended; info rewritten in place |
| Shader | VU pass: state program, texture slots, waveforms, template name | effect index + 0x44-byte parameters (the passes' textures in order) | effect index looked up by name in the game's effect table (`0x500F28`, filled at boot); derived fields moved per class |
| Window damage records | 0x60 | 0x50 | rewritten in place |
| Occluder planes | 0x20 | 0x18 | rewritten in place |
| VehSim fluid, super stop turbo | base vptr at +0x0C; 16-byte aligned colours | vptr first; packed | rewritten in place |
| Tune, collision, audio, skeleton, bone tags, rider animation | same | same | kept |

`docs/history` has no log for this task: the layout differences were measured by walking the PS2 and Xbox files of the
same 94 cars side by side and reading the XBE's own loaders (`0x105830` root, `0x10A490` car model type, `0xE9620` LOD,
`0xDDF50` mesh, `0xE7790` set, `0xDCBC0` vertex declaration, `0x1BF910` shader factory, `0x4FD30` effect shader,
`0xE5410`/`0xE4F30` texture load and bind).

## What is not carried over

* **Wheels** are not in the car files on either platform: the game builds them from the shared rim and tire packs
  (`rim.xbpf`, `tire.xbpf`), which still load from the Xbox assets. The car's own wheel records (positions, sizes) do
  come from the PS2 file.
* Only the top mip level of a texture is converted (most car textures have one; some DUB cars have four).
* PS2-only bytes with no Xbox counterpart are dropped: the VU state programs, GS register words and waveforms of shader
  passes, the duplicate local-index bytes of a few shader classes.
* Bytes the parser counts as *dead* are allocations nothing points at; they occur in car files edited by tools that
  append and repoint.

## How it was checked

* The parser on every PS2 car in a retail `resources\vehicle` tree: 282 vehicle files (94 cars, base/`_o`/`_g`) and 4029
  part files, none refused; mean 99.3% of the bytes consumed for vehicles and 99.8% for parts. The two lowest vehicles
  (about 75%) were edited car files whose remaining bytes are dead data.
* The converted files of 12 cars (cars, bikes, DUB, police; base and `_o`) walked side by side with the Xbox files of
  the same cars: every object has the Xbox layout; what differs is data (float rounding, the PS2's own shader local
  indices, the PS2 `_o` textures at half size).
* Geometry against the Xbox meshes of the same cars: the same triangle count (52,566 in six cars), 99.9% of them at the
  same place with the same winding; UV identical, vertex colour identical at the 99th percentile.
* In game, legacy profile: PS2 Jetta in **Arcade > Cruise** in San Diego, driven for 70 s, pause, **Go to Menus**; no
  `exception`, no `KeBugCheck`. With the traffic pool (`--mc3-traffic-cruise-pilot`, `vp_g500_03`, enhanced) the pool
  cars load from the PS2 `_o` files. Without the flag nothing is logged and the game is unchanged.
* A car from a community PS2 mod pack that has no Xbox version (an Audi R8: 22 files, built with append-and-repoint
  tools) loaded through a folder that gives its files the Jetta's names: it drives, shows its own body, parts, lights
  and speedometer scale, and leaves through pause and Go to Menus cleanly. Its 84 vehicle files and 1965 parts convert
  without a refusal. The consumption figure of such files is low (about 50 to 80%): the tools leave old allocations
  behind, some of them still reachable through words that only look like pointers, so they count as unclaimed rather
  than dead. A car that is new to the game, rather than replacing one, also needs its entry in the game's vehicle list,
  tune and front-end files; that is not part of this loader.
