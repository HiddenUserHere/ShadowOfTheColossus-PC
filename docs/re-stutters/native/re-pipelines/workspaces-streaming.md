# Workspaces (.cwks) and area streaming: RE notes

Scope: how the engine reads the workspace dependency file (`$/workspaces/_ps4/shadow.cwks`), what it holds in
memory, how area/zone activation works, and where the port could prepare pipelines for the next area.
Addresses are eboot virtual addresses (the decomp file names in `out/re/decomp/<4 hex>/<addr>.c`). Read-only work.

Evidence keys: **observed** = read in decomp code or logs; **inferred** = reasoned from the code, not run;
**open** = not settled by static reading.

## 1. Answers in short

1. **The .cwks is parsed at boot, once, synchronously.** The cvar `ResourceDependenciesFile` is read by
   `0x2567f0` (called from `0x2569d0`, engine init). It calls the resource pool's virtual slot `+0x30`,
   `ResourcePool::LoadResourceDependencies` (`0x14efb0`). The file is a `PSKW` v6 blob. Its body is kept in
   the pool as a sorted table of 16-byte entries. **observed** (code), **inferred** (run order: no run made).
2. **The table is consumed by one job.** `ResourcePool::RecordResourceDependencies` (`0x14edc0`) is queued by
   `0x14efb0` and, when it runs, frees the table (`pool+0x130`). After it, `GetResourceDependencies` (`0x14e560`)
   returns nothing. **observed** (code).
3. **Area streaming is driven by zones, not by the .cwks.** The zone manager update `0x3510e0` applies a
   pending zone id (`mgr+0xf8`) through `ZoneActivate` (`0x34fb30`), which sets the per-area
   `AreaLoaderState` objects (state setter `0x343890`). The area's zone/priority list comes from an
   `.export.xml` sibling file (parser `0x351a20`, `Zones` node). **observed** (code), the file content is unread.
4. **The engine has no lookahead it exposes.** The only signal we see for "next area" is the script call that
   sets the zone (strings `SetActiveZone`, `OnZoneChanged`, `zoneId`). When it comes is unknown. **open.**
5. **Boot can list the table's names.** Yes, at `0x14edc0` entry, or from the file itself. Whether the table
   covers every shader collection (`.cslt`) and material (`.cmat`) per area is **open**: the file bytes were
   not available (no game dump in this checkout, `user/` has no extracted `.cwks`).
6. **Port today:** only the image side is hooked (`SOTC_AREA_PREWARM`, replay of image records). Nothing reads the
   `.cwks` or the `.export.xml`. Pipelines and shader translation are not prepared per area. The best
   candidate hook for a "prepare next area" step is the `AreaLoaderState` setter (state request `Loading`)
   and, earlier, the `.export.xml` parse (section 5).

## 2. The dependency file: where it is read

### 2a. Startup command line and cvar

- `0x536730` `BuildStartupCommandLine` (decomp `0053/00536730.c`) writes the option text. Strings:
  `+ResourceDependenciesFile=$/workspaces/_ps4/shadow.cwks` (string `0x1015285`), `+StartupScript=$/scripts/shadow/shadowmain.sgpr`,
  `+startupArea=%s`. **observed** (`docs/re-stutters/reports/07-first-use.md`).
- The logs show both forms: `CmdLine: +ResourceDependenciesFile=$/workspaces/****/shadow.cwks ...
  +ResourceDependenciesFile=$/workspaces/_ps4/shadow.cwks`. The `****` is a 4-character placeholder replaced by
  the path builder `0x14c2f0` (see 2d). **observed** (`user/last_run.log`).
- `0x2534c0` registers the cvar (`Cvars::RegisterResourceDependenciesFile`, string `0xfc38b4`). The cvar
  object is at `GameCore+0x2bb0` (the caller reads `*(param_1+0x2bb0)+8` for the length). **observed** (code), name `inferred`.

### 2b. Boot call chain

| Step | Address | What it does | Evidence |
|---|---|---|---|
| 1 | `0x533100` | Engine startup routine (see 5b). Builds and resolves the precache lists, calls step 2 | decomp body |
| 2 | `0x2569d0` | 12.6 KB init. Creates the `ResourceFactory` (`0x144ca0`, 0x350 bytes, vtable `0x1598518`), the `ResourcePool` (`0x14db00`, 0x138 bytes, vtable `0x1598628`), registers ~40 resource types, then calls step 3 | decomp, call at `0x2569d0` line 612 |
| 3 | `0x2567f0` | If the cvar is empty: prints "No resource dependency file specified. Streaming will hitch. Lots." (`0xfc3a2a`). Else calls pool vtable `+0x30` = `0x14efb0` and prints "Resource dependency file loaded: %s" | decomp; vtable checked |
| 4 | `0x14efb0` | `LoadResourceDependencies`. Opens and reads the file (`0x1512b0`), checks magic and version, keeps the body, queues step 5 | decomp |
| 5 | `0x14edc0` | `RecordResourceDependencies`, queued as a job (`0x35480` on `DAT_02ea4718`, fence at pool+0x78). Frees the table at its end | decomp |

The log line `[MSG-Init] Resource dependency file loaded: $/workspaces/_ps4/shadow.cwks` is printed by `0x3e7e0`
(the message logger). **observed** in `user/last_run.log`.

### 2c. Resource type registration (cwks, cslt, and others)

`0x14ef10` registers the `Workspace` type, FourCC `0x63776b73` (`cwks`), with magic `0x6574b5350`, which is the
bytes `PSKW` and version 6. `0xb0e3a0` registers the `ShaderList` type, FourCC `0x63736c74` (`cslt`), magic
`0xa534c5354` (bytes `TSLS`, version 10). **observed**. The registration call is `0x1444e0(factory, fourcc, desc)`;
the list of other FourCC types is in the decomp of `0x2569d0` and its callees (for example `cmat` `0x636d6174`
at `0xf9270`, `cmdl` `0x636d646c` at `0x110540`, `cmsh` `0x636d7368` at `0x127860`, `calt` `0x63616c74` at `0xa5b5a0`,
`crmp` `0x63726d70` at `0xa5bbd0`, `csml` `0x63736d6c` at `0x270a90`, `cprt` `0x63707274` at `0xfde80`).

Note: `0xb0e3a0` is the registration of the `cslt` type, not a reader of a shader list. Earlier notes called it
"the ShaderList reader"; that is corrected here.

### 2d. Path and key building

- `0x14c2f0` resolves a path template: it replaces `****` with 4 characters from `0x147e20` (a map lookup in the
  factory, which the boot sets with `0x147e10(factory, 0x2a2a2a2a)`), falls back to factory `+0x40`, then `_ps4`
  (from the decomp and `scripts/hooks.d/area.py`). Then it hashes the result (see 2e). **observed** (decomp) for the
  hash; the `_ps4` fallback is from `area.py` (**inferred** there).
- Key hash: `0x161f80` is FNV-1a, 32 bit (`0x811c9dc5` basis, `*0x1000193` prime, xor before multiply). It is used
  by `0x14c530` (key from a string), by `0x14c2f0`, and by all resource lookups. **observed**.

## 3. In-memory form of the dependency table

### 3a. Pool object (ResourcePool, 0x138 bytes, vtable `0x1598628`)

Offsets are byte offsets from the pool object `P`. Fields below are the ones the code uses.

```c
struct ResourcePool {            // vtable 0x1598628 (ctor 0x14db00)
    void*    vtbl;               // +0x00
    Factory* factory;            // +0x08  (DAT_02ee2d00, the ResourceFactory, vtable 0x1598518)
    /* +0x18 name map sentinel, +0x28 root: a tree of 0x100-byte nodes keyed by FNV hash + name */
    pthread_mutex_t mtx;         // +0x70  (locked by 0x14e260, 0x14e560, 0x14edc0)
    uint8_t  dep_loaded;         // +0x120 set by 0x14efb0 when the file parsed
    uint8_t* dep_end;            // +0x128 base for name offsets (= table start + N*16 + 4)
    uint32_t* dep_table;         // +0x130 start of the body (count N, then entries)
    /* +0x78: job fence used by the Record job (0x35480) */
};
```

`dep_end` is stored as `(table + (N*16 + 4))` in `0x14efb0`. Name offsets in the entries are relative to this
base, not to the table start. **observed** (decomp of `0x14efb0`, `0x14edc0`, `0x14e560`).

### 3b. On-disk body (PSKW v6 file, after the 8-byte header)

Header read by `0x14efb0`: `u32 0x574b5350` (`PSKW`), `u32 6`. The rest is the body, read whole into memory.

```c
// body at dep_table
struct DepBody {
    uint32_t count;              // N
    struct DepEntry entries[N];  // 16 bytes each, sorted by key (binary search in 0x14e560)
    // string pool and dependency lists follow; offsets are from dep_end
};
struct DepEntry {                // 4 uint32 per entry (decomp index math)
    uint32_t key;                // FNV-1a 32 of the entry name (same hash as 2e)
    uint32_t name_off;           // offset from dep_end to a C string: the entry name
    uint32_t dep_count;          // M: number of dependencies
    uint32_t deps_off;           // offset from dep_end to the dependency list
};
// dependency list at deps_off: M items, each
//   uint8_t len; char name[len];   // 0x14e560 advances by len + 1 per item
```

Evidence for the layout: `0x14e560` binary-searches `key` (stride 4 uints from `dep_table+4`), matches the name
with `strlen` and `strcasecmp`, reads `dep_count` and `deps_off` for the match, and walks items with
`pbVar14 += len + 1`. `0x14edc0` walks `name_off` for every entry. **inferred** for the field names, **observed**
for the offsets.

**Open detail:** `0x14e560` turns each item name with `0x14c530`, which copies a C string (`0xf1a0`). With
`pbVar14 += len + 1`, the bytes after the name must not be a NUL, or the stride would be wrong. Check on a real file
which one holds (the first items decide it).

**Open detail:** the body length is not stored in the pool (`0x14efb0` keeps the size in a local). A snapshot must
bound the read (for example the largest `deps_off` plus its items, or the file size from the psarc).

### 3c. Get and Record

- `Record` (`0x14edc0`): for each entry, reads `name_off`, builds the key with `0x14c530`, and calls pool slot
  `+0x18` (`0x14e260`). Slot `+0x18` is a get-or-create on the name map under the pool mutex: it creates a
  0x100-byte node if the name is new and returns it. The returned reference is released at once. Then it sets
  `dep_end = 0`, frees `dep_table`, and clears the pointer, under the mutex. **observed** (code).
  **inferred:** the effect is to create the name nodes ahead of use. It does not walk the dependency lists, and it
  does not start loads (the node constructor `0x14c8a0` and the release were not read in full).
- `Get` (`0x14e560`, pool slot `+0x38`): input is a key struct `{uint hash; ...; char* name; size_t len}` (built by
  `0x14c530`) and an output vector of refs. If the table is still there, it finds the entry, then for each
  dependency item calls slot `+0x18` (get-or-create) and pushes the ref into the output vector (`0x14e800`).
  If the table is gone, it returns the current output count (`0x14e7a2`). **observed.** So `Get` only gives
  dependencies while the table exists, that is, between `0x14efb0` and `0x14edc0`.
- Callers of `Get` are not found in the static decomp (the pool pointer is passed through a vtable or a copy).
  **open.** A log at entry of `0x14e560` with the return address (`[rsp]`) would name them.

### 3d. Pool slot map (verified from the eboot vtable at `0x1598628`)

| Slot | Offset | Function | Role |
|---|---|---|---|
| 0 | +0x00 | `0x14de00` | dtor |
| 3 | +0x18 | `0x14e260` | get-or-create a name node (mutex) |
| 4 | +0x20 | `0x14e440` | name lookup (mutex), used by the precache list reader `0x25ddd0` and `0xf9530` |
| 6 | +0x30 | `0x14efb0` | LoadResourceDependencies |
| 7 | +0x38 | `0x14e560` | GetResourceDependencies |

The factory vtable `0x1598518`: slot 2 = `0x146b60` (BuildSync), slot 3 = `0x147490` (BuildAsync), slot 5 =
`0x1478d0` (DeleteResource). Those are as in `docs/re-stutters/reports/05-streaming.md`. **observed.**

## 4. Area streaming

### 4a. Zone manager and zone activation

- `0x3510e0`: per-frame update of the area loader manager (the object at `mgr`; its `+0x98` holds the area
  loader object). **observed** (code); the name `AreaLoaderManager::Update` is **inferred**.
  Fields used:

```c
struct AreaZoneMgr {            // "mgr" in 0x3510e0 / 0x34fb30 / 0x34f680 / 0x351a20
    /* +0x98 */ AreaLoaderObj* loader;   // loader+0xb0: the current area resource (or the default at +0x88)
    /* +0xa0 */ ZoneRec* zones_begin;    // sorted by uint zoneId at +0, 0x80 bytes per record
    /* +0xa8 */ ZoneRec* zones_end;
    /* +0xc0 */ Map     area_map;        // lookup used by 0x33f9b0 (AreaLoaderState by id)
    /* +0xf0 */ uint8_t  pending_flag_a;
    /* +0xf1 */ uint8_t  pending_flag_b;
    /* +0xf4 */ uint32_t current_zone;   // applied zone id
    /* +0xf8 */ uint32_t requested_zone; // 0xffffff = none
    /* +0xfc */ uint8_t  ready;          // *(loader+0xc0), gate for the update
    /* +0xff */ uint8_t  reload_zones;   // -> 0x351a20 (parse "<area>.export.xml")
    /* +0x100*/ uint8_t  reload_b;       // -> 0x351c30
};
```

- Zone activation `0x34fb30` (`ZoneActivate`): stores `zoneId` at `mgr+0xf4` (first statement), binary searches
  the zone table, and for the zone's list of 8-byte item ids (`zone+0x18` to `zone+0x20`) converts each id with
  `0xa77a90` and looks up the area object with `0x33f9b0(mgr+0xc0, ...)`. For each area it sets the state with
  `0x3427d0(area, priority_byte)`. **observed** (code). This is the `AREA_ZONE_ACTIVATE` hook point
  (`scripts/hooks.d/area.py`).
- The pending zone is applied in `0x3510e0` at the top of the update: if `requested_zone != 0xffffff`, it calls
  `0x34fb30(mgr, requested_zone)` and clears the request. Otherwise, on a zone-0 reset it calls `0x34fb30(mgr, 0)`
  (from `0x34f680`). **observed.** Who writes `requested_zone` (the script call `SetActiveZone`) is **open**.

### 4b. AreaLoaderState setter

- `0x343890` (`AreaLoaderState` setter, hook `AREA_LOADER_STATE`): rdi = state object, rsi = int* of the current
  state, rdx = int* of the requested state. The state names (eboot strings): Auto, Unavailable, NotLoaded
  (Unloaded), Loading, Loaded, Creating, Created_Hidden (Hidden), Created_FadeIn, Created, Created_FadeOut.
  `scripts/hooks.d/area.py` gives the value table. **observed** (strings), **inferred** (order 3..9).
- Callers: `0x343340` (string property parser), `0x3436e0` (virtual "apply requested state", tail-calls the
  setter), `0x343720` (timed advance). **observed** (decomp).
- The `Loading` state is where an area's resource loads start (inferred from the names; the link to the resource
  pool calls was not read).

### 4c. Zone and area definitions: the `.export.xml`

- `0x351a20` (called from `0x3510e0` when `reload_zones` is set): builds the path `<area path>.export.xml` (the area
  path from `loader+0xb0`, else the default at `loader+0x88`; `0x14c3a0`, `0x30730`, string `.export.xml`), parses it
  with the XML reader (`0xab3b50`, `0xab1b20` "Zones" node, `0xab0d80`), and calls `0x352350` for each zone record.
  **observed** (code).
- `0x352350` (about 3 KB) reads per-zone and per-area entries. Strings: `zoneId`, `priority`, `persistent`, `inherit`,
  `Current`, `background`, `default_low`, `default_high`, `default_fade`, `default_persistent`, `never`, `always`,
  `loaded`, `unloaded`, `created`, `hidden`. `0x350220` (about 3.5 KB, from `0x34f830`) parses the `Import` and
  `version` nodes. **observed** (strings and call graph).
- So the zone list and each area's load priority come from XML, not from the .cwks. The .cwks gives resource names
  and their dependency names. **inferred** from the code.
- The area file is a named resource, so its name (for example `$/…/<area>`) is known to the zone manager when the
  area loader is set. Its exact string is not read (the `loader+0xb0` chain is not checked at run time).

### 4d. Other streaming hooks in the engine

- Loader thread and barrier: see `docs/re-stutters/reports/05-streaming.md` (loader `0x1464d0`, frame barrier
  `0x1469d0`, completion sync `0x1480f0`). Not re-read here.
- Resource list readers: `0xf9530` checks a stream header `STAM` version 9 (`0x4d415453`, `0x39`), then looks up
  a name with pool slot `+0x20`. `0x25ddd0` reads a stream of two-int records; for type 3, subtype 2 it looks up
  the name (pool slot `+0x20`), then tests and queues it (ready checks `0x14cc80`, `0x14d2c0`,
  `0x14cf00`, then `0x14f8e0` adds it to a list; it also calls `0x1469d0`, the frame barrier). The purpose (precache
  lists per area) is **inferred**. It is a second route to per-area lists besides the .cwks. **observed** (code).

## 5. Port state and where a "prepare next area" step could run

### 5a. What the port has

- `gpu/sotc/probe_area.cpp`: hooks `AREA_ZONE_ACTIVATE` (`0x34fb30`, zone id known, name from the zone table),
  `AREA_LOADER_STATE` (`0x343890`, counters), and `AREA_PRECACHE` (`0x533100`, counters). Calls
  `SotcAreaPrewarm::OnAreaChange(name, seq)` (line 303 and 344), and `OnAreaChange("boot", 0)` at init (line 358).
- `gpu/sotc/area_prewarm.h`: records image records per area (`user/cache/area_manifest/<area>.bin`) and replays
  them with `SOTC_AREA_PREWARM=3` (image creation only, no data upload, no pipelines, no shader translation).
- No code reads `PSKW`, `.cwks` or `.export.xml`. `grep -rn cwks` in `gpu/`, `src/`, `scripts/` finds nothing.

### 5b. Caveat on `AREA_PRECACHE` (0x533100)

The earlier label "per-area loader" for `0x533100` does not match its body. It creates and sets the global
factory and pool (`DAT_02ee2d00`, `DAT_02ee2cf8`), calls the whole init `0x2569d0`, and then requests the precache
lists with a path template (`****`). The body is consistent with one-time engine startup, not with each area load.
**inferred.** Check: count `g_precache` in a run (`SOTC_AREA_PREWARM=1` area report) across a zone change. If it
stays at 1 (boot), `AREA_PRECACHE` is not a per-area hook.

### 5c. Candidate hooks for a "prepare next area's pipelines" step

Listed earliest first. "Early" means the time before the area's resources are needed.

| Hook | Address | Signal | Lead time | Notes |
|---|---|---|---|---|
| Table read at boot | `0x14edc0` entry (rdi = pool) | the whole `.cwks` name table (`dep_table`, `dep_end`) | boot | Read the names once and keep them. Does not give the next area |
| Dependency query | `0x14e560` entry (rsi = key, rdx = out) | a query for one entry's dependencies, with the caller in `[rsp]` | when the game asks | Shows which entries are queried and when. Only while the table exists |
| Area list parse | `0x351a20` entry (rdi = mgr) | the area's `.export.xml` is about to be read | start of an area change | Earliest point with the area's name. Zones and priorities follow |
| Zone activation | `0x34fb30` entry (esi = zone id) | zone applied this frame | current, not next | Already hooked (`AREA_ZONE_ACTIVATE`) |
| Area state request | `0x343890` entry (rdx = requested state) | an area asked for `Loading` | a few frames to seconds | Per area (AreaLoaderState identity from `area.py`). Best trigger for pipeline work |
| Shader collection load | `0xb0e440` (`cslt` loader callback, not read) | a `.cslt` file's bytes are available | when the file loads | Would name every shader collection as it loads (candidate for option 4 in STUTTER-ARCHITECTURE) |

Suggested first step (no behaviour change): entry logs at `0x14edc0`, `0x14e560`, `0x351a20`, `0x343890` with
their arguments and return addresses, one run that goes through one area change. This settles the order of the
calls, the real `Get` callers, and whether `Record` runs before or after the first area.

## 6. Corrections to earlier notes

- `0xb0e3a0` is the `cslt` type registration, not a "ShaderList reader" (`reports/07-first-use.md`,
  `STUTTER-ARCHITECTURE.md` section 5).
- `0x1451a0` is the resource statistics printer (strings: "Resources: Count ...", "Factory: ..."), not
  `LoadAsDependency` as `reports/05-streaming.md` and `STUTTER-ARCHITECTURE.md` suggested.
- `0x14efb0` body is now read: it parses `PSKW` v6 (earlier: "body inferred").
- `0x2567f0` is called from the boot init `0x2569d0`, so the .cwks load is a boot step (the earlier note only said
  "LoadResourceDependencies called from the cvar").
- `0x533100` is probably one-time startup, not per area (section 5b).

## 7. Open questions

1. What is in `shadow.cwks`: how many entries, which are areas, and whether the dependency lists contain `.cslt`
   and `.cmat` names. Needs the psarc entry (`pak*.psarc`, path `$/workspaces/_ps4/shadow.cwks`) read on a machine
   with the dump. The port could read it at run time with its own psarc path (no derived data shipped).
2. The item stride (section 3b, open detail) and whether names are NUL-terminated.
3. Callers of `GetResourceDependencies` (pool slot `+0x38`) and the time of `Record` (the job fence `+0x78`).
4. Whether `Record`'s `slot +0x18` calls only create nodes, or start loads (node ctor `0x14c8a0`, release `0x14ccc0`).
5. The zone id source: which script call sets `requested_zone` (`SetActiveZone`), and how far ahead of the load it runs.
6. Whether `0x533100` runs once (section 5b).

## Verified in a run (4 Nov 2026)

One run of the Lab test route (user/routes/tests.json) with SOTC_AREA_PREWARM=1 (observe only; log
user/area1/area_probe.log):
- AREA_PRECACHE (0x533100) fired once in the whole session, during boot (the 5 s report window in which the game
  went from spawn_00 to uiingame). It did not fire again.
- AREA_ZONE_ACTIVATE (0x34fb30) fired 9 times: spawn_00, uiingame (twice each), then during the route home_north,
  home, home_south, arena_01_entrance, home_south.
- AREA_LOADER_STATE (0x343890) recorded 0 state sets in every window.

So 0x533100 is one-time startup work, as suspected above, and not a per-area precache. Per-area work has to
hang on ZoneActivate (0x34fb30), which fires at each zone change, or on an earlier signal not found yet.
