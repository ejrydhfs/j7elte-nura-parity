# integration87 — dma-buf FMODE_LSEEK + DRM_RDWR PRIME exports

int86 wired `.llseek = dma_buf_llseek` and the symbol was in kallsyms, but on
the device every lseek on an exported dma-buf fd returned ESPIPE:

```
SEEK_SET: -1 errno=29 (ESPIPE)
SEEK_END: -1 errno=29 (ESPIPE)
```

**SEEK_SET also failing is the discriminator**: `fs/read_write.c:vfs_llseek()`
is

```c
fn = no_llseek;
if (file->f_mode & FMODE_LSEEK) {
	if (file->f_op && file->f_op->llseek)
		fn = file->f_op->llseek;
}
return fn(file, offset, whence);
```

and dma-buf files are created by `anon_inode_getfile()`, which allocates the
file with `alloc_file(&path, OPEN_FMODE(flags), fops)` — it never sets
`FMODE_LSEEK` (only `do_dentry_open()` does).  So the fops slot was not
consulted at all; `no_llseek()` returned ESPIPE for every whence.  int86's
fops wiring was therefore probably fine, and int87 now proves the slot in the
built vmlinux rather than assuming it.

## Changes

1. `patches/drm-backport/0007-dma-buf-export-fmode-lseek.patch`
   (`drivers/base/dma-buf.c`): check `IS_ERR(file)` from
   `anon_inode_getfile()` (3.10 lacked it) and set `file->f_mode |=
   FMODE_LSEEK`.  `SEEK_END` then reaches `dma_buf_llseek()` and returns
   `dmabuf->size`.

2. `patches/drm-kms/0137-kms-core-prime-allow-rdwr.patch`
   (`include/uapi/drm/drm.h`, `drivers/gpu/drm/drm_prime.c`): define
   `DRM_RDWR O_RDWR`; `DRM_IOCTL_PRIME_HANDLE_TO_FD` accepts
   `DRM_CLOEXEC | DRM_RDWR` and passes both through to
   `drm_gem_prime_export()` → `dma_buf_export()`, so the dma-buf file is
   opened writable (writable MAP_SHARED mmap for the importer).  On int86 the
   ioctl returned EINVAL at the `args->flags & ~DRM_CLOEXEC` check, before
   any dma_buf_export() call.

Series: int86 (int84 + 0137-driver mmap instrumentation + 0006 dma-buf
llseek) + 0007 + 0137-kms.

## Build-time proof of the fops slot

`int87 gate: dma-buf file is seekable, fops slot points at llseek` parses the
linked vmlinux ELF: it takes the `dma_buf_fops` address from nm, reads the
8-byte pointer at offset 8 (`struct file_operations.llseek`; owner is at 0)
from the LOAD segment, and requires it to equal `dma_buf_llseek`:

```
dma_buf_fops=0xffffffc000b5ab08 llseek_slot=0xffffffc0005d2fc0 dma_buf_llseek=0xffffffc0005d2fc0
```

The gate also requires `file->f_mode |= FMODE_LSEEK` in the built tree, and
the DRM_RDWR mask/define/plumbing.

## Artifacts

| file | size | sha256 |
|---|---|---|
| `Image-int87` | 18191112 | `534bbb2050fc4459061c3bab81b4c8c79c2079017006715890ae6618cc1a8d27` |
| `kernel-boot-int87.img` | 27330560 | `b0ff0ef7e7dac306d92a05d611e56caf7f1a58423501a2c23be032afcd804657` |
| `exynos-drm7580-int87.ko` | — | `be022c79bfd50b1e9ca14abc5cf22d65cbf6d4dbc8a3175303d0556b4b631f84` |
| `drm_kms_helper-int87.ko` | — | `eeea59fcfdcd9fbfe97af70d7b4fc2b552a449ada8b01e8525b18f62c4ec0c27` |

Build log `int87-kms-build.log`: 0 errors.  All int86 gates PASS plus the fops
slot proof and the DRM_RDWR gates; two .ko, imports resolved, stock-dt boot,
pstore/lockup cmdline unchanged.

Log sha256s: build `006747085c101daa028aa286b4cb8a605e5e0084bc18c13b0640f0fd1f9bd25b`, gates `6e4f0894d4c5875f7d34f6312b1dc41260e7be4fd627c356bd9a12a7bff33db2`, evidence `117466c7e61bf22ff85cbebde53fb5d090d07b81bfdf03e3ef9262310cfc85d3`, config `541d5ff0d74c0d2221bd73103c1a602fc1783931bea831a48ab28a781e78207e`.

## Device acceptance

Flash `kernel-boot-int87.img` (`b0ff0ef7…cd804657`) + the two int87 .ko.

- `/tmp/seek` on a PRIME-exported dumb buffer: `SEEK_SET=0`,
  `SEEK_END=<size>` (nonzero; the buffer size, e.g. 0x200000 for 2 MiB).
- `drmPrimeHandleToFD(card0, dumb_handle, DRM_CLOEXEC | DRM_RDWR)` returns
  a fd >= 0 (EINVAL gone), and an import of that fd over the render node
  maps it writable.
- mesaprobe/kmscube as before; dmesg keeps emitting the int85 p7580 mmap
  instrumentation so any remaining failure is localised by the
  `gpu31-integration85.md` discriminator table.
