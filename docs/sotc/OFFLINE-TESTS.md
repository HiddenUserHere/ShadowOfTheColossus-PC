# Offline deterministic tests

The deterministic mode runs the game without a wall-clock timer and without sound, with deterministic file
input and output, and plays the SotC Lab test route by frame number. The intent is that two runs of the same
configuration produce the same frames, and that a set of flags can be measured automatically (without
watching the screen) and compared with another set.

By default the game opens a small **monitor window**: the game picture with the FPS overlay, so an
unattended run can be watched. `--hidden` (matrix runner) or `SOTC_HEADLESS=1` removes the window. This page
describes how to use the mode. It may still have thread-scheduling noise (see [Limits](#limits)).

## Turning the mode on

```
SOTC_DETERMINISTIC=1 python sotc.py
```

`SOTC_DETERMINISTIC=1` sets the values below **only when the variable does not exist yet** in the environment or
in `sotc.ini`. An explicit value always wins, so you can turn a single switch off or change the route.

| Variable | Default with `SOTC_DETERMINISTIC=1` | Effect |
|---|---|---|
| `SOTC_DET_CLOCK` | `1` | the game's virtual clock (not wall-clock time) |
| `SOTC_DET_FLIP` | `1` | presentation pacing without vsync |
| `SOTC_HEADLESS` | `2` | monitor window: a small window that shows the game and the FPS overlay (`1`: no window) |
| `SOTC_FPS` | `1` | the FPS overlay and its counters (the monitor window shows them) |
| `SOTC_DET_ROUTE` | `1` | the Lab route advances by frame number |
| `SOTC_NULL_AUDIO` | `1` | null audio (does not need `SOTC_SYS_MODULES`) |
| `SOTC_DET_IO` | `1` | deterministic file input and output |
| `SOTC_LAB` | `1` | turns on the SotC Lab (and the free camera, the control channel and the engine commands) |
| `SOTC_LAB_ROUTE` | `user/routes/new-route.json` | route played by itself when the game starts |
| `SOTC_FPS_CAP` | `off` | no 30 FPS cap in the game |

The route file `user/routes/new-route.json` is not in the repository: create it in your `user/` folder (the same
format as the routes in `tools/routes/`). Without it the route does not run and the run is marked invalid.

Two per-frame outputs are enabled by the matrix runner (or you can set them by hand):

| Variable | File | Content |
|---|---|---|
| `SOTC_FRAME_METRICS=<file>` | `metrics.csv` | one row per frame: `frame, wall_ms, draw_busy_ms, draw_ops, cp_busy_ms, cp_blocked_ms, rec_busy_ms, gpu_ms, game_wait_ms, upload_bytes, shader_compiles, pipeline_compiles` |
| `SOTC_FRAME_HASH=<folder>/hash` | `hashes.csv` | one row per frame: `frame, exact, phash` (exact image hash and pHash) |

sotc.py writes the session log to `user/last_run.log` (or to the folder of `SOTC_USER_DIR`), as usual.

## The monitor window

With the default `SOTC_HEADLESS=2`, the game opens a small window that shows the picture and the FPS overlay.
It is meant for unattended runs you want to keep an eye on. When the runner starts a run, the window title
names it, from the `SOTC_RUN_*` variables the matrix runner sets:

| Variable | Example | Meaning |
|---|---|---|
| `SOTC_RUN_LABEL` | `flag r2/3` | the configuration and the repeat (`r<k>/<repeats>`); warmup rounds are `<config> warmup` |
| `SOTC_RUN_INDEX` | `5/12` | the position of this run in the whole matrix |
| `SOTC_RUN_FLAGS` | `FILL_HLE_GPU=0` | the configuration's own variables, without the `SOTC_` prefix (cut to 120 characters) |

Set `SOTC_HEADLESS=1` (or run the matrix with `--hidden`) to get no window.

## Running a matrix of configurations

`tools/offline_matrix.py` runs `python sotc.py` for each configuration, with the shared environment (`base`) plus
the configuration's switches, and compares the results. It uses only the Python standard library; use the
Windows Python.

Example: compare three configurations (the first one is the reference):

```json
{
  "base": {"SOTC_GAME_DIR": "../CUSA08809"},
  "repeats": 3,
  "warmup": 1,
  "timeout_s": 900,
  "frames": [600, 1800],
  "configs": {
    "reference": {},
    "no_fill_hle_gpu": {"SOTC_FILL_HLE_GPU": "0"},
    "no_copy_guest_view": {"SOTC_COPY_GUEST_VIEW": "0"}
  }
}
```

```
python tools/offline_matrix.py matrix.json --out user/offline/test1
python tools/offline_matrix.py matrix.json --dry-run            (prints the planned runs and their environment, runs nothing)
python tools/offline_matrix.py matrix.json --hidden --out ...   (no monitor window)
python tools/offline_matrix.py --aggregate user/offline/test1   (rebuilds the summary of an earlier folder)
```

File fields:

- `base`: variables shared by every configuration (game path, user folder, etc.). Do not put
  `SOTC_DETERMINISTIC`, `SOTC_FRAME_METRICS` or `SOTC_FRAME_HASH` there: the runner sets those three.
- `configs`: name and variables of each configuration. The **first** one is the reference of every comparison.
- `repeats`: how many valid runs per configuration (default 3).
- `warmup`: how many rounds are discarded at the start (default 1). They warm up the shader cache; the first
  run of each configuration usually compiles shaders and is kept out of the measurement.
- `timeout_s`: maximum time of one run; past it, the process tree is ended (default 900).
- `frames`: optional window `[from, to]`, inclusive, in frame numbers.
- `exit_frames`: optional integer. The game exits after this frame (`SOTC_DET_EXIT_FRAMES`). Without it, a window
  `frames` ends at its last frame plus 120 frames; without either, the route decides when the game exits.

The runner also sets, for every run, unless the configuration sets the variable itself:
`SOTC_HEADLESS` (`2`, or `1` with `--hidden`), `SOTC_DET_EXIT_FRAMES` (as above) and the `SOTC_RUN_*` labels.
`--dry-run` shows these under `(runner)`.

The rounds are interleaved: round 1 runs every configuration, then round 2, and so on. This way a slow change
in the machine hits all of them alike.

Each run gets a folder `runs/<configuration>/r<round>/` with `metrics.csv`, `hashes.csv`, `last_run.log` and
`stdout.txt`. A run is **invalid** when it timed out, wrote no frames or, with a route set, did not log
`SOTC LAB ROUTE done:` in the log. Invalid and warmup runs are listed in the summary, but are left out of the
averages.

## Reading summary.md

The file `summary.md` (and the same content in `summary.json`) has four parts:

1. **Timing**. For each column of `metrics.csv` and each statistic (`mean` and `p99`), the value of each
   configuration. Before aggregating, each frame uses the **median** over the repeats; then the mean and the p99
   are taken over the frame series of the window. The `±` is the **spread** (minimum to maximum) of the same
   statistic over the repeats of that configuration: it is the noise measure.
2. **Difference from the reference**. In each cell, `Δ (Δ %)` and the noise (`±`), which is the larger spread
   of the two sides. A `*` marks a difference larger than the noise. With a single repeat there is no noise
   estimate and nothing is marked.
3. **Images**. For each configuration:
   - *nondeterministic*: frames whose exact hash changes between the repeats of the **same** configuration.
     This is noise of the mode itself, not a visual change.
   - *changed*: frames that are stable inside the configuration and differ from the reference (stable). The
     pHash distance (Hamming, in bits) says how much the image changed: 0 means perceptually identical images
     even if the exact hash differs; a few bits, a subtle difference; many bits, a visible difference.
   - *unstable*: frames where the reference or the configuration has no stable hash; they are not compared.
4. **Invalid runs**, with the reason.

A timing difference only counts when it is larger than the `±` (`*`). With `repeats` 3, a lone `*` is a hint to
confirm with another round, not a conclusion.

## Limits

- **Thread scheduling.** The game's threads and the Windows scheduler are not deterministic. Frames are
  reproducible, but the times (`wall_ms`, `cp_busy_ms` and similar) vary between runs. The `±` measures that
  noise; always read the times next to it.
- **Flags that depend on real time.** A switch whose effect depends on how long each frame takes (for example
  the FPS limit, or anything that waits on a busy thread) behaves differently in the offline mode. Do not use an
  offline result as a measure of that effect; measure it live with `SOTC_FPS=1`.
- **Exact hashes and drivers.** Exact hashes are only comparable between runs on the same machine, with the same
  driver and the same shader cache. A new shader cache can change the warm-up (hence `warmup`).
- **Null audio.** Without real sound, switches tied to audio are not measured by this mode.
- **Fixed route.** A route different from the user's produces different frames; comparisons only hold between
  configurations that play the same route.
- **Window.** The monitor window is a normal window on the desktop. Use `--hidden` or `SOTC_HEADLESS=1` when
  the desktop must stay untouched.
- **Cost.** Each run is a complete game run. A matrix of 3 configurations with 3 repeats and 1 warmup is 12 runs.
