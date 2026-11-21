# gpu31-microdraw — attribute-less triangle + tiler forensics

Date: 2026-11-21. Owner: G31_MicroDraw. Phone: link LOST mid-ticket
(RNDIS `172.16.42.1` unreachable, ping 100% loss; only a `18d1:d001`
fastboot USB device visible; Main owns physical access/recovery).
Kernel at last contact: `3.10.108 #37` (int34-track, all gates PASS).
Mesa 20.3.5 on device (`/data/mesabuild/mesa20` + `-b`), surfaceless.

Related: `gpu31-polylist.md` (tiler heap linkage), `gpu31-serialfence.md`
(per-file chain→fragment ordering, int34), `gpu31-integration34.md`,
`gpu31-primitive.md` (untextured control), `gpu31-tilerchain.md`,
`gpu31-jddecode.md` (JDBODY/PTEWALK decode), `gpu31-jobabi.md`.

## 0. TL;DR

- Microdraw client written and committed (`code/gpu31/microdraw/`):
  standalone EGL+GLES3 pbuffer client, one attribute-less solid-red
  triangle (`gl_VertexID`-selected constants, no VBO/attributes/textures/
  varyings), clear + 60× draw + `glFinish`, readback centre/corner/top.
- Microdraw on silicon: **RENDER_MISMATCH, all-zero readback**
  (`centre=0,0,0,0`, `corner=0,0,0,0`) — worse than textured `egltest`
  (clear-exact corner). The all-zero readback is a **retire artefact** of
  the bare `glReadPixels`-after-`glFinish` path on this driver (cf.
  `gpu31-primitive.md` §1: attribute-less solid binary also read
  all-zero; ES2-attribute ES3-context probes also all-zero), NOT proof
  the GPU wrote black.
- **New hard evidence (clean-dmesg windows via `/usr/local/bin/rootshell`):**
  every path faults on the chain head and then wedges behind it —
  - textured `egltest`: 1× `INSTR_TYPE_MISMATCH (0x52)` on the last
    fragment submit (`slot=0 … head=0x66cf600 seq=121 … sw=…00010013…`),
  - solid ES2 triangle: `0x52` on the chain head (`slot=1`,
    `head=0x72280c0`, `jc=0x7228400`, `sw=…00020005…`) then
    `POWER_FAULT (0x41)` cascades on every following seq,
  - microdraw (attribute-less): `0x51 INSTR_INVALID_ENC` on the chain
    head (`slot=1`, `head=0x88d60c0`, `jc=0x88d6580`,
    `sw=…00020005…`) then `0x41` cascades.
  `head = jc+0xC0` in all chain faults = the **VERTEX job**
  (WRITE_VALUE@+0x0, VERTEX@+0x40 COMPUTE_JOB len 192 → next job
  at +0xC0). The fragment job never runs cleanly because the chain
  ahead of it faulted. This supersedes `gpu31-polylist.md` §4 "zero
  ERR / DONE both slots": that was a stale-dmesg observation
  (user-space `dmesg -c` is permission-denied; old buffer showed the
  pre-fault window).
- Polygon-list decode: **BLOCKED — no settled tiler dump exists.**
  The `0xCF` debug-read route is live on the phone (probe: user+ kern
  views agree), but the phone link dropped before the post-fault
  polygon-list/heap census could run. The int34 serialfence gate
  (chain→fragment implicit dep) is built but its on-silicon effect is
  unverified for the same reason. The tiler never gets a clean run in
  any window observed, so "no triangle coverage" is expected: the
  VERTEX job faults before the tiler bins anything.
- Precise cause (from evidenced fault words, no speculation beyond):
  the chain-head VERTEX/COMPUTE job faults with `INSTR_*` on every
  program shape tried (textured ES2 `0x52`, solid ES2 `0x52`,
  attribute-less ES3 `0x51`). The JDBODY of the faulting microdraw
  chain was captured (`jc=0x88d6580`, `words=128` + `+64` + `+128`
  lines in the kernel log) but the log is on the unreachable phone;
  user-space re-decode is pending link recovery. Suspect pool (in
  order): (a) VERTEX COMPUTE_JOB descriptor field the T720 rejects
  on this path (shader-meta `state` BO / TLS `thread_storage` /
  `attributes` setup for zero-attribute VS); (b) invocation/params
  word the driver snapshot (`sw[4]=00020005`) vs live JDBODY can
  disambiguate once dumped. NOT the texture path (solid faults too),
  NOT heap flags (133/133 `flags=0x0`, polylist §2), NOT slot routing
  (req=0→JS1 / FS→JS0 intact, faults are per-job INSTR not slot
  storms), NOT chain→fragment ordering as primary (the FIRST fault
  is on the chain head itself, before any fragment runs).

## 1. Deliverables (committed)

- `code/gpu31/microdraw/microdraw.c` — the attribute-less client
  (ES 3.0, `gl_VertexID` constants, solid red FS, 60×draw retire
  loop, 3-pixel verify). Built on device with the system gcc
  (`gcc -O2 -o /tmp/microdraw /tmp/microdraw.c -lEGL -lGLESv2`).
- `code/gpu31/microdraw/mdhook.c` — `LD_PRELOAD` SUBMIT decoder
  (job-chain walk, header/invocation/primitive/DRAW/FBD-tiler decode
  against `midgard_pack.h` offsets, BO table). Lesson encoded:
  Mesa 20.3 speaks the pre-jm_ctx UAPI (40 B SUBMIT `0xc0286440`,
  `offset` returned inline in CREATE_BO); match ioctls by type+nr,
  not by `/usr/include` numbers. Partially exercised (chain/req/nbos
  + jc logged, 480 lines) before the link dropped; iova back-fill
  fix staged but unverified.
- `code/gpu31/microdraw/Makefile` — host-side build stub.
- Device-only artefacts (NOT committed, on the unreachable phone):
  `/tmp/microdraw`, `/tmp/mdhook.so`, `/tmp/mdhook.log`,
  `/tmp/mdhook.bos`, `/tmp/mdprobe`, `/tmp/md3/4/5`, `/tmp/dbg1/2.so`.

## 2. Exercised evidence (all from clean fault windows)

| Run | Pixels | Faults (rootshell dmesg) |
|---|---|---|
| textured `egltest` 60 draws | corner `26,26,102` clear-exact, centre clear, RENDER_MISMATCH | one `0x52` on last fragment submit (`slot=0 head=0x66cf600 seq=121 sw=…00010013…`, nbos=12) |
| solid ES2 triangle 60 draws | all-zero (retire artefact) | `0x52` chain head (`slot=1 head=0x72280c0 seq=0 sw=…00020005…`) + `0x41` cascades seq=1,2; 3 irq-enter only |
| microdraw ES3 attr-less | all-zero (retire artefact) | `0x51` chain head (`slot=1 head=0x88d60c0 jc=0x88d6580 sw=…00020005…`) + `0x41` cascades; JDBODY words=128/+64/+128 + PTEWALK MATCH captured in kmsg |
| `0xCF` probe | — | LIVE (`kern==user` on both test words) |

Fault-code table: `code/gpu31/jobs/panfrost7580_jm_hw.h:166-188`
(`0x41 POWER_FAULT`, `0x51 INSTR_INVALID_ENC`, `0x52 INSTR_TYPE_MISMATCH`).
`sw[]` = SUBMIT-time `job->desc[0..7]` snapshot
(`driver/0001…patch:2838-2872`); `sw[4]` packs
`is_64b|type|index` (`midgard_pack.h:6580-6591`): `00020005` =
VERTEX type 5 on the chain submits, `00010013` = FRAGMENT type 9.

## 3. Next step (hand to Main / whoever holds the phone)

1. Re-establish link (physical re-plug per SESSION-STATE; phone shows
   only as fastboot `18d1:d001` right now — confirm it boots our
   kernel, not S-Boot/fastboot).
2. Re-run `/tmp/microdraw` under `/tmp/mdhook.so` (fixed CREATE-BO
   iova arm is staged in the repo copy; rebuild the `.so` on device),
   then dump the settled polygon list via `0xCF`:
   `pl`/`body` from the hook's `FBD TILER` line, census
   head-nonzero/`a0000000` markers/heap-zero exactly as in
   `gpu31-polylist.md` §3 — but only meaningful after a clean chain.
3. The real fix is in the VERTEX job the T720 rejects: pull the
   faulting chain's JDBODY (`jc` from the ERR line, `w=…` + `sw=…`
   from PTEWALK) and diff the COMPUTE_JOB DRAW words
   (`state`/TLS/`attributes`/`varyings`/`thread_storage`) between
   textured-ES2 (0x52), solid-ES2 (0x52) and attr-less-ES3 (0x51).
   The `0x51`-vs-`0x52` split across shader shapes is the sharpest
   lead: identical chain skeleton, different fault code ⇒ the
   rejected word lives in the shader-meta/varying region, not the
   header/invocation. `pan_emit_draw_descs` / `panfrost_emit_vertex_data`
   / `panfrost_emit_varying_descriptor` (Mesa `pan_context.c` /
   `pan_cmdstream.c` reviewed this ticket, §4 notes) are the
   cross-reference.
4. Keep: `0xCF` route, JDBODY/PTEWALK/ERR-MAP gates, serialfence
   int34 image (unflashed at ticket close — flash is Main's call).

## 4. Mesa review notes (no code change; for the fixer)

- `pan_job.c:1015-1056` (`panfrost_batch_submit_jobs`): chain
  (`reqs=0`) then fragment (`PANFROST_JD_REQ_FS`), `in_sync=0` both —
  the serialfence dep is the kernel's job (int34 built, unverified).
- `pan_context.c:279-328` (`panfrost_draw_emit_vertex`): VERTEX is a
  COMPUTE_JOB (`job_task_split=5`, `pan_emit_draw_descs` fills
  `uniforms/textures/samplers`, `state` from
  `panfrost_emit_compute_shader_meta`, `thread_storage=shared_mem`).
  For the attribute-less VS, `panfrost_emit_vertex_data` emits only
  the `(n+1)'th` zero record (`pan_cmdstream.c:1099+`); whether a
  zero-attribute COMPUTE_JOB needs a non-null `attributes` word on
  T720 is open — compare with the textured JDBODY.
- Varying linkage (`pan_cmdstream.c:1700+`): `present =
  (1<<GENERAL)|(1<<POSITION)` always; `vs_size=0` for zero
  FS/vs-varying counts is normal; `position` buffer is always
  emitted. The `TILER DRAW position` pointer + `varyings` BOs are
  all in the submit handle list (hook prints them once iova
  back-fill is verified).
- Tiler math (`lib/pan_tiler.c:286-330`, `midgard_pack.h:3344-3420`,
  `3814-3870`): flat `header=0x200+tiles*8`,
  `bodyoff=header`, TILER section 10 words @ FBD+192, WEIGHTS @+256,
  SFBD offsets `LS@0 PARAMS@32 TILER@192 WEIGHTS@256` — used by the
  hook; verified against the `-b` build tree on device.
