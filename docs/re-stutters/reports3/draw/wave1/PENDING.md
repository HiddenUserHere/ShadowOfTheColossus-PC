# Wave 1: call-site snippets waiting for their file's owner

Applied snippets are struck out with the commit that carries them.

| From | Target file (owner) | Snippet (details in <from>.md) | State |
|---|---|---|---|

| f2 | videoout/driver.cpp (f1) | include gnmdriver.h; ReportFrameAhead(report) | applied |
| f2 | sotc.py (me) | gameMaxDeltaTime=0.05 with SOTC_FRAME_AHEAD=1 | applied |
| f2 | liverpool.cpp | frame_tickets also with SOTC_FRAME_AHEAD=1 | applied |
| l1 | videoout/driver.cpp | SotcLabels::ReportLabels(report, seconds) + include | applied |

| p1 | videoout/driver.cpp | ReportPipeDirty decl + call | applied |

| m0 | liverpool.cpp, vk_presenter.cpp, driver.cpp | NoteDrawOp, SOTCSTALL ops, DRAW WORK line | applied |
| t1 | driver.cpp | VideoCore::ImageChurnReport() + include | applied |
| r1 | driver.cpp | SotcRbHost::Report + include (D) | applied |
| r1 | pc_ring_copy.cpp | g_heap_runs + merge SotcRbHost::Runs() (A) | applied |

| r1 | ? | RecordMirrorCopy after the result-writing dispatch (E, not located) | pending (me) |
| texbind | driver.cpp, image.h, vk_rasterizer.cpp | ReportTexIdentity; MarkGpuModified bump; FindTextureIdentity at the texture bind | applied |
| texbind | (wave 2) | SOTC_RT_MAXEXTENT not started | wave 2 |
| v1buf | vk_rasterizer.cpp, driver.cpp | SotcGen::Bump at MapMemory/UnmapMemory; SotcGen::Report | applied |
| l1/m1/r1 | buffer_cache.* | NoteSharedTouch, PcHostAddRing, NoteServedWrite, ApplyDownloads compare | applied |
| d1 | vk_pipeline_serialization.cpp, buffer.h, vk_rasterizer.*, driver.cpp | S1, B1, R1-R4, D1 | applied |
| t1b | driver.cpp | ReportRtMaxExtent | applied |
| d1 | vk_pipeline_cache.cpp | P1: Profile .bda_buffers + include | applied |
| p1b | driver.cpp | ReportPipeGpl decl + call | applied |
