# j7elte ROM ↔ Nura driver parity contract

## Ground truth (stable, read-only)
- Stock ROM: SM-J700M `J700MUBU2BRC2`, Android 6.0.1, kernel `3.10.61-13213628`, Exynos 7580 (`j7elte`).
- ROM boot image: `/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/j7elte-session/boot_out/`
  - `kernel.bin` raw arm64 Image; `vmlinux.elf` reconstructed ELF with 55,634 kallsyms symbols;
  - `symbols.txt` sorted symbol list; `dtbs/*.dts` (dtc -s), `rd/` unpacked ramdisk.
- ROM system: `/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/j7elte-session/system.raw` (ext4; use `debugfs -R`).
- ROM firmware (already extracted): `parity/firmware/`, see `parity/firmware/MANIFEST.sha512`.
- Nura kernel tree (downstream LineageOS universal7580 3.10.108):
  `/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/j7elte-session/android_kernel_samsung_universal7580-2a746c1bc1a41eae05988c153f527c8a5dc72d07`
  baseline git commit `b5e363a`.
- Nura pmaports baseline: `parity/pmaports`, git commit `ba682bd`
  (`device/archived/linux-samsung-j7elte`, `device/archived/device-samsung-j7elte`).
- Nura kernel config: `/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/j7elte-session/config-nura-j7elte`
  (copy of `config-samsung-j7elte.aarch64`).
- Upstream Linux 7.3 (reference only): `/run/media/asdf/30620d0d-1e41-4d55-af08-b4de2fb9c517/linux-7.3`.
- Upstream ROM↔Nura DT diff: `/tmp/nura_j7.sorted.dts`, `/tmp/rom_000_800.sorted.dts` (Nura vs ROM, sorted).

## Parity definition (per driver)
- **P1 symbol coverage**: ROM symbols for the driver are implemented in the Nura tree.
  ≥98% counts as pass; every exception must be explained (module_init/exit wrappers,
  generic subsystem symbols, or dead code). Cite `symbols.txt` and source files.
- **P2 device tree**: every ROM DT node/property for the driver exists with the same
  value in the Nura j7elte DTS. Phandle/label numbering differences are ignored.
- **P3 config**: Nura config enables the driver and its dependencies.
- **P4 firmware/blobs**: every runtime firmware/blob is present at the path the Nura
  driver opens, or the driver is patched to use a Linux path.
- **P5 patch**: if P1–P4 fail, produce a patch that applies to the baseline.

A driver is at parity only when P1–P4 pass. Otherwise report the exact failing item.

## Rules
- Evidence only. Cite `file:line`, symbol name, DTS path, or sha512. No guesses.
- NEVER modify the baseline commits. NEVER `git add`, `git commit`, `git checkout`,
  `git stash`, `git reset`, or rebase in the shared trees.
- Kernel edits only inside your ownership paths (given per task). Generate patches with
  `git -C <kernel-tree> diff -- <owned paths> > parity/patches/<driver>/0001-<name>.patch`.
- pmaports edits only in `parity/pmaports`; generate `git -C parity/pmaports diff > ...`.
- Verify every patch: `git -C <tree> apply --check <patch>` (against a clean baseline).
- No kernel cross-toolchain is installed. Static verification only; state this limitation.
- Disassembly: `objdump -d --start-address=... --stop-address=... boot_out/vmlinux.elf`.
  Ghidra headless (`/opt/ghidra/support/analyzeHeadless`) only if decompilation is truly
  required; do not run a full 18 MB auto-analysis unless unavoidable.
- Deliverables (exact paths):
  - `parity/reports/<driver>.md`
  - `parity/patches/<driver>/*.patch` (only if P1–P4 fail)
  - `parity/firmware/files-<driver>.list` (ROM path → install path, one per line)
- Report format: verdict, P1–P4 table with evidence, patches, residual risks/tests.
