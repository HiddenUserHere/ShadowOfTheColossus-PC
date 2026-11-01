# N5 pipelines: boot prebuild and steady-state counter (SOTC_N5_PSO)

Specialist report (psos2), saved by the coordinator from the agent's hand-back. Not built by the agent, not run in
game; integrated in commit "vulkan: SOTC_N5_PSO, boot prebuild ordered by use and a steady-state compile counter".

## Item 1: boot prebuild (SOTC_N5_PSO=1, needs SOTC_NATIVE)
- WarmUp reads every pipeline blob first, stable-sorts by saved use count (most used first) and queues them in that
  order; workers pop FIFO, computes still go to the front.
- WaitPreloadBoot loops CollectAsyncPipelines and waits on g_preload_cv until every queued graphics pipeline is
  built, capped at SOTC_N5_PSO_WAIT_S (default 15 s); the rest installs as before. The loading bar shows
  SotcStats::warmup_total/done.
- Use count: draws/dispatches binding each pipeline (plain increment on the CP thread), stored as an 8-byte trailer
  after the blob: `[magic u32 'CU5N'][count u32]`, written only in N5. Readers stop after the pipeline data (nothing
  checks EOS), so old readers and blobs without a trailer work; no trailer reads as 0.
- SaveUseCounts runs only from ~PipelineCache and rewrites blobs whose count changed (not from the 60 s saver: it
  would race the CP on the maps).

## Item 2: steady-state counter
"SOTC N5 PSO" 5 s line: sync compiles by kind (graphics sync build, compute not deferred, successful GPL fast link,
CompileModule without worker pools), excluding WarmUp, its boot wait and the first SOTC_N5_PSO_GRACE_S seconds
(default 30). Boot line "SOTC N5 PSO boot: built of total graphics pipelines, ms (done)" until done, then once.

## Mode 0
The trailer, SaveUseCounts, boot ordering and counters return early; what runs regardless is atomic increments, the
use_count bump in the GetGraphicsPipeline/GetComputePipeline wrappers and the WarmingScope store.

## Open
1. The compute boot wait is not capped (computes are queued first; measure).
2. Counts are saved only at ~PipelineCache: a killed process loses the session's counts; check that the DataBase
   singleton still exists at that point of static teardown.
3. With SOTC_PIPE_REMAP=1 a remapped compute key built after the cap keeps its old blob (harmless).
4. N5 holds all key blobs in RAM during warm-up (transient).

## Item 3 (design only): skip RefreshFlatBuf when nothing it read changed
- The SRT walker reads guest memory only through the copy function it is passed (SrtCopyWindow,
  flatten_extended_userdata_pass.cpp:83) plus RefreshSharpTables; the read set is a function of the user data.
- Pass a recording copy function at the first walk of a (program, user data) pair; store the ranges (cap ~16) and
  a write generation per range (N1 announced regions) in StageMemo next to flat_buf.
- Skip only when the user data matches, every range is in an announced region and no generation changed.
- Gate SOTC_N5_FLATBUF=1; =2 walks anyway and counts mismatches; counters walked / skipped / ranges.
- Depends on N1 write generations and on N2 proving no unannounced writes to SRT-read memory.
