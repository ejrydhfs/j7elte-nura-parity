# integration86 — dma-buf llseek (SEEK_END) fixes Mesa import mmap ENOMEM

int85's instrumentation proved the int84 ENOMEM was not in `p7580_gem_mmap`:
Mesa's panfrost import sized the BO with `lseek(dmabuf_fd, 0, SEEK_END)`,
the 3.10 `dma_buf_fops` has no `.llseek`, the call failed and Mesa passed
`SIZE_MAX` as the mmap length:

```
FBDDBG mmap FAIL handle=5 size=18446744073709551615 (SIZE_MAX) off=0x10193000
```

No `p7580_gem_mmap: enter` line follows: the syscall never reached the
driver.  int86 = int85 + `patches/drm-backport/0006-dma-buf-llseek-seek-end.patch`
(backport of the upstream dma-buf SEEK_END support).

## Change

`drivers/base/dma-buf.c`:

```c
static loff_t dma_buf_llseek(struct file *file, loff_t offset, int whence)
{
	struct dma_buf *dmabuf = file->private_data;
	loff_t base;

	if (!dmabuf)
		return -EINVAL;

	if (whence == SEEK_SET)
		base = 0;
	else if (whence == SEEK_CUR)
		base = file->f_pos;
	else if (whence == SEEK_END)
		base = dmabuf->size;
	else
		return -EINVAL;

	return base + offset;
}

static const struct file_operations dma_buf_fops = {
	.llseek		= dma_buf_llseek,
	.release	= dma_buf_release,
	.mmap		= dma_buf_mmap_internal,
};
```

`lseek(exported_fd, 0, SEEK_END)` now returns `dmabuf->size`; importers that
size a BO that way (Mesa panfrost import, kmsro path) get a sane length and
the mmap reaches `p7580_gem_mmap`.  Series: int85 unchanged (int84 + 0137
mmap instrumentation), plus 0006.

## Artifacts

| file | size | sha256 |
|---|---|---|
| `Image-int86` | 18191112 | `b85a3172bfe5163b6fe0cded16dcd2760ec6c2f15eff683a6a667191128c5a6f` |
| `kernel-boot-int86.img` | 27330560 | `3f91d806a4cd2406b5664e8eb4ae79430010d6b63765d2ed00fbabbd719b8582` |
| `exynos-drm7580-int86.ko` | — | `ec7632691787c6e29942bb10d8ef71d1f4165aebc5df32cc81bb78864522145b` |
| `drm_kms_helper-int86.ko` | — | `a40eb6d013eaf753e4a1163e87551a5046f58aff05df3e62e020ad340967772a` |

Build log `int86-kms-build.log`: 0 errors.  All int85 gates PASS, plus
`int86 gate: dma-buf llseek SEEK_END` (function present, SEEK_END branch,
`.llseek` wired in `dma_buf_fops`, `dma_buf_llseek` linked in vmlinux), two
`.ko`, imports resolved, stock-dt boot, pstore/lockup cmdline.

Log sha256s: build `bdc18ad92828eceb98875d75232aa0b0ca1916c384ddc1fb0ad791f13657bf84`, gates `97f5f84debf3fcb8080acc014a4de33ad5009eafa0239093e56ab85377cea813`, evidence `21cd336a4984b94395d4ffbe9717257d946d69c16202c8f708bce772369f711d`, config `541d5ff0d74c0d2221bd73103c1a602fc1783931bea831a48ab28a781e78207e`.

## Device acceptance

Flash `kernel-boot-int86.img` (`3f91d806…719b8582`) + the two int86 .ko, then
run the mesa20-kmsro probe/kmscube.  Expected:

- no `FBDDBG mmap FAIL ... size=18446744073709551615` line;
- a userspace `lseek(exported_fd, 0, SEEK_END)` returns the BO size (the
  mesaprobe import path exercises it; a minimal check is
  `echo 0 >/dev/null` not applicable — use mesaprobe's FBDDBG output or
  `lseek` in the kmscube import path);
- dmesg now shows `p7580_mmap_bo: ... fake_offset=0x…` followed by
  `p7580_gem_mmap: enter …`, `hit …`, `mixedmap … rc=0` and on first touch
  `p7580_gem_fault: … rc=0`;
- if the map still fails, the int85 discriminator table (in
  `gpu31-integration85.md`) localises it from those lines.
