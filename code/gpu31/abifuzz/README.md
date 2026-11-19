# abifuzz — host fuzzer/validator for the six v5.2 panfrost ioctl encodings

Host-side only. Never part of any kernel build. Runs on the build host
against the compat headers (`../drmcore/`, `../gem/panfrost_drm.h`) via
the `stub/` kernel-environment stand-ins.

## Layout

- `abifuzz.c` — the fuzzer/validator (directed probes + 20000-iteration
  deterministic differential fuzz + layout/encoding/table checks).
- `compat_smoke.c` + `stub/drm_stub.c` — smoke test proving the drmcore
  compat macros expand and behave (fops wiring, `DRM_RENDER_ALLOW`
  mapping, `readl_poll_timeout` return shape).
- `stub/` — host-only kernel/uapi stand-ins. Never compiled into the kernel:
  `drm.h` (legacy include path), `drm/drm.h` (uapi subset, real
  `DRM_IOW(nr,type)` shape), `drm/drmP.h` (real 3.10 gate values),
  `drm/drm_gem_cma_helper.h`, `linux/{types,io,delay,jiffies,fs}.h`,
  `drm_stub.c` (linkable stub definitions for the smoke binary).
- `Makefile` — `make check` builds both binaries and runs them.

## Run

```
make check
./abifuzz      # exit 0 = clean; exit 1 = findings present (see report)
./compat_smoke # exit 0 = compat macros healthy
```

`./abifuzz` exit 1 is the *expected* steady state until findings F1–F6
in `docs/reports/gpu31-abifuzz.md` are fixed: it reports findings, not
build failure. `./compat_smoke` must always exit 0.

## What it checks

- Struct sizes/offsets: six `_310` mirrors byte-identical to the UAPI
  mirror (canonical LP64 sizes 40/16/24/16/16/16).
- Ioctl encodings: numbers 0x00–0x05 on `DRM_COMMAND_BASE`, direction
  bits, embedded size fields, `_310` macros == UAPI macros.
- Table contract: 6 rows in order, all `DRM_RENDER_ALLOW`, only SUBMIT
  additionally `DRM_AUTH` (matches v5.2 `panfrost_drv.c` table).
- Validation differential: three reference predicates per ioctl
  (upstream v5.2 / current gem shim / current jobs slice), compared over
  directed edge cases plus randomised buffers (fixed LCG seed, reproducible).
- Static contracts that cannot execute on the host (out_sync lookup,
  compat-ioctl wiring) are recorded as findings with tree references.

See `docs/reports/gpu31-abifuzz.md` for the findings and owner routing.
