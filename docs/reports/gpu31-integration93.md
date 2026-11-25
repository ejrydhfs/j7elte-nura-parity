# integration93 — DECON window/register evidence + KMS dma-buf free guard

KWin's commits are accepted after int92 (the pipe is re-enabled when a commit
finds it off) but the panel is black, so the question moved from "is the
commit rejected" to "what does it program".  int93 adds the evidence and stops
the vendor free path from touching KMS-owned dma-buf entries.

## Changes (`patches/drm-kms/0143-kms-decon-window-dump-ion-guard.patch`)

**Shim, `decon_kms_api.c`:**

- `decon_kms_program()`: one block per commit —
  `decon_kms_program: nwin=N max_win=M num_of_window(pre)=…` and one line per
  window: KMS state, vendor state, fourcc, vendor format enum, idma,
  blending, plane alpha, the three DMA addresses, src (x/y/w/h/f_w/f_h) and
  dst rect.  After `decon_export_set_win_config()`: its rc, `num_of_window`
  and the three `wincon[]` values.
- `decon_kms_commit()`: `decon_kms_dump_regs()` after
  `decon_export_update_regs()` and again after the bounded vsync wait.
  Header line: VIDCON0, VIDOUTCON0, VIDINTCON0, VIDINTCON1, TRIGCON.  Per
  window: WINCON, VIDW_ADD0/ADD2/ADD3, VIDOSD_A/B/C/D, VIDW_WHOLE_X/Y,
  VIDW_OFFSET_X/Y.  A kmscube commit (working) and a KWin commit (black) can
  be diffed line by line.

**Vendor free path, `decon_core.c`:**

- `decon_free_dma_buf()`: KMS-programmed entries carry only the IOVMM address
  the shim mapped itself (`decon_export_regs_param_addr()` fills
  `dma_addr` and nothing else), so `dma->attachment` is NULL.  The function
  now skips `ion_iovmm_unmap()` for such entries (that path BUG_ON()s on a
  non-ion dma_buf and would dereference the NULL attachment), drops a pending
  fence, logs `kms-owned addr 0x… skipped (no attachment)` and clears the
  entry.  Real ion/vendor entries keep the unchanged path.

int92's non-ON commit re-enable and all earlier diagnostics stay.

## Artifacts (Image needed: both files are built-in)

| file | size | sha256 |
|---|---|---|
| `Image-int93` | 18191112 | `4e8f746faf7cec988d74b7c7e9bd3af9c1dd4695cf9ecc6b28f907a61c6f57fe` |
| `kernel-boot-int93.img` | 27330560 | `02085312db3c951e98acd4570d1d6ce105afb442e95040b53e6d2255165baa12` |
| `exynos-drm7580-int93.ko` | — | `6464cb85a8f3135193548307262d75aa7edad22d45bfdef4b0a67bc142f65920` |
| `drm_kms_helper-int93.ko` | — | `44034b6b5ec3a8d048186f7ea6bbbd1f07b305dfe2a5a1823f7c010af113a16a` |

Build 0 errors.  All int92 gates plus `int93 gate: DECON window/register dump
+ ion-free guard` PASS (program/per-window/register log sites, both dump
calls, attachment guard and skip log, and the two strings in vmlinux).  Log
sha256s: build `7ff438b52881e71bbf271a14fa4e8f92cb3e081728fddafcbdb2dfcc1e949240`, gates `2eeedf947ad091288a1a94d06cd0085987972794ba70a73d18a2ce24e9a24251`, evidence `eb0e54f3660e246248f9c48696b56650a83b992e4fca9678c705bc1616d27ac6`, config `541d5ff0d74c0d2221bd73103c1a602fc1783931bea831a48ab28a781e78207e`.

## How to read the two captures

Run on a clean boot: `kmscube` first (known-good), capture
`dmesg | grep -E 'decon_kms_program|decon_kms_regs'`; then KWin, capture the
same.  For the KWin commit:

- `num_of_window` must be >= 1 and `win[0]` must be
  `kms_state=BUFFER`, a supported `format`, and the logged
  `addr=0x…` must equal the read-back `ADD0=0x…`;
- `WINCON` must carry WINCON_ENWIN (bit 0); if it does not, the window state
  was lost between program and register readback;
- `VIDOSD`/`WHOLE`/`OFF` values must match the logged dst/src rects; a mismatch
  names geometry translation;
- if the KWin block matches kmscube's shape except for the addresses, the
  programming is right and the difference is the BO/IOMMU mapping of KWin's
  buffers (the INT93 free guard only removes the spurious ion unmap).

No `ion_iovmm_unmap` BUG may appear from a commit any more; the skip line
`decon_free_dma_buf: kms-owned addr … skipped` is the expected replacement.
