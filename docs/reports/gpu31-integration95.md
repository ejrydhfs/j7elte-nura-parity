# integration95 — kbase-style interrupt masks, erratum 10797, MMU fault dump

The KWin 0x08 start timeouts were silent by construction: this driver never
unmasked the MMU/GPU-level interrupt sources stock kbase enables, so an MMU
fault or a GPU-level error was not delivered at all; a stuck slot only produced
the generic `p7580_job: TIMEOUT ... js_status=0x00000008` line.

## Changes (`code/gpu31/driver/0140-panfrost7580-irq-masks-erratum.patch`)

`panfrost7580_jm_hw.h`:

- kbase GPU_CONTROL `GPU_IRQ_MASK` 0x1058 and `ALL` = 0x10581.  (The power
  slice's `PF7580_GPU_IRQ_MASK` 0x028 is the mainline `GPU_INT_MASK` it uses
  for reset-completion only; it stays as is.)
- MMU block: `RAWSTAT` 0x2000, `CLEAR` 0x2004, `MASK` 0x2008, `STAT` 0x200C.
- AS registers (0x2400 + as*0x40): `FAULTSTATUS` +0x1C, `FAULTADDRESS_LO`/`HI`
  +0x20/+0x24, `FAULTEXTRA_LO` +0x38.
- t72x `SHADER_CONFIG` 0x0F04 and the erratum-10797 value `0x02000000`.
- `p7580_js_status_name()` now labels the kbase JM states: 0x00
  `NOT_STARTED/OK`, 0x01 `DONE`, 0x02 `INTERRUPTED`, 0x03 `STOPPED`, 0x04
  `TERMINATED`, 0x08 `ACTIVE` (previously everything below 0x40 printed
  `NO_EXCEPTION`, which is why 0x08 looked like "no exception").

`panfrost7580_job.c`:

- `p7580_job_irq_program()`: `MMU_INT_CLEAR = 0xFFFFFFFF`,
  `MMU_INT_MASK = 0xFFFFFFFF`, `GPU_IRQ_MASK = 0x10581`, with a readback log;
  called right after the JOB interrupt pair in `p7580_job_dev_init()` **and**
  `p7580_job_dev_rearm()` (so recovery/resume unmask again).
- `p7580_job_post_reset()`: `SHADER_CONFIG = 0x02000000`, readback logged;
  called after the plain SOFT/HARD-reset fallback **and** after the recovery
  power cycle (which performs the reset), so the erratum bit is present on
  every post-reset GPU.  **Coverage note:** there was no `SHADER_CONFIG`/
  0x0F04 write anywhere in the driver or power slices before this patch; the
  erratum was not covered.
- TIMEOUT: after the existing snapshot, `TIMEOUT-DIAG` prints AS0
  `FAULTSTATUS`, `FAULTADDRESS` HI/LO, `FAULTEXTRA_LO`,
  `MMU_INT_RAWSTAT`, `MMU_INT_STAT`, `GPU_INT_RAWSTAT` and the JS state name.
  AS0 is the only address space this driver programs (`P7580_DRV_AS_NR`).

No scheduling/recovery behaviour change; the KMS int94 instrumentation is
untouched.

## Artifacts (driver built-in: flash the Image)

| file | size | sha256 |
|---|---|---|
| `Image-int95` | 18191112 | `6b0e6f9d71c8acf88dc2e3bedc277c7c6a966e0ce80ca980cdbdd975caac05aa` |
| `kernel-boot-int95.img` | 27330560 | `03f84d8f7533231d5287a3378a37eff40793d27b569aae9a0fde94d424ce1c36` |
| `exynos-drm7580-int95.ko` | — | `9304c4a3020f2511869ba6577e1b427c275642d74b827d28d1018ce54903576a` |
| `drm_kms_helper-int95.ko` | — | `89349c1b2103eff5d6142b037c28f713313312462d14a9f93a4a77b617398bad` |

Build 0 errors.  All int94 gates plus `int95 gate: irq masks, erratum 10797,
timeout fault diagnostics` PASS (offsets/values, both irq-program call sites,
both post-reset call sites, TIMEOUT-DIAG, and the strings in vmlinux).  Log
sha256s: build `8fcc45dec50fc813c42ebe38aafdacdb27ef297c4203d0b26666cdb2a7803cd5`, gates `635fafa92860c2a27665d9d4a2670217e8939e3c745623d8db7a9d10336736f0`, evidence `0e7f1ef433b2c1bc8ceb8887ab4296e3210d95486e8e1ac8297a60aaee0aa64d`, config `541d5ff0d74c0d2221bd73103c1a602fc1783931bea831a48ab28a781e78207e`.

## Expected on the next KWin run

- probe: `p7580_job: dev-init irq-mask gpu_irq_mask=0x10581(rd ...)
  mmu_mask=0xffffffff(rd ...) mmu_raw=...`; recovery/resume:
  `rearm irq-mask ...`.
- after a reset/cycle: `p7580_job: post-reset SHADER_CONFIG=0x02000000
  (t72x erratum 10797)`.
- either no more 0x08 timeouts (the unmasked GPU/MMU IRQ lets the fault path
  do its job), or a timeout now followed by a `TIMEOUT-DIAG` line with a real
  AS0 FAULTSTATUS/FAULTADDRESS (MMU fault) or a non-zero MMU/GPU RAWSTAT — in
  both cases the line names the cause instead of silence.
