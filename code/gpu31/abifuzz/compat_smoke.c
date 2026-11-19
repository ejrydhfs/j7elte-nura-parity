/* Host smoke: drmcore compat macros expand and behave under stub env.
 *
 * Exercises code/gpu31/drmcore/drm_compat_310.h:
 *  - both DEFINE_DRM_GEM_FOPS / _COMPAT expand with all 8 entries wired
 *    (the two macros must differ ONLY by the compat-ioctl question —
 *    currently they are byte-identical, see report F6);
 *  - DRM_RENDER_ALLOW maps to DRM_UNLOCKED (real 3.10 gate values);
 *  - readl_poll_timeout returns 0 on an immediately-true condition.
 * Exit 0 iff all smoke probes pass (exit code is independent of the
 * F1-F5 validation findings in ./abifuzz).
 */
#include <stdio.h>
#include <drm/drmP.h>
#include "drm_compat_310.h"

DEFINE_DRM_GEM_FOPS(abf_fops, drm_gem_cma_mmap);
DEFINE_DRM_GEM_FOPS_COMPAT(abf_fops_compat, drm_gem_cma_mmap);

int main(void)
{
	int fails = 0;
	void __iomem *reg = (void __iomem *)0x11400000UL;
	unsigned int val = 1;
	int pr;

#define SMOKE_CHECK(cond) do { \
	if (!(cond)) { \
		printf("SMOKE-FAIL %d: %s\n", __LINE__, #cond); \
		fails++; \
	} \
} while (0)

	/* Fops entries wired identically on both macros. */
	SMOKE_CHECK(abf_fops.open == drm_open);
	SMOKE_CHECK(abf_fops.release == drm_release);
	SMOKE_CHECK(abf_fops.unlocked_ioctl == drm_ioctl);
	SMOKE_CHECK(abf_fops.mmap == drm_gem_cma_mmap);
	SMOKE_CHECK(abf_fops.poll == drm_poll);
	SMOKE_CHECK(abf_fops.read == drm_read);
	SMOKE_CHECK(abf_fops.llseek == noop_llseek);
	SMOKE_CHECK(abf_fops_compat.open == drm_open);
	SMOKE_CHECK(abf_fops_compat.unlocked_ioctl == drm_ioctl);
	SMOKE_CHECK(abf_fops_compat.mmap == drm_gem_cma_mmap);

	/* compat_ioctl gap: neither macro wires drm_compat_ioctl. */
	SMOKE_CHECK(abf_fops.compat_ioctl != drm_compat_ioctl);
	SMOKE_CHECK(abf_fops_compat.compat_ioctl != drm_compat_ioctl);

	/* Flag mapping (real 3.10 gates from stub/drm/drmP.h). */
	SMOKE_CHECK((unsigned)DRM_RENDER_ALLOW == (unsigned)DRM_UNLOCKED);
	SMOKE_CHECK((unsigned)DRM_UNLOCKED == 0x10u);
	SMOKE_CHECK((unsigned)DRM_AUTH == 0x1u);

	/* Poll helper: immediately-true condition returns 0. */
	pr = readl_poll_timeout(reg, val, (val != 0), 10, 100000);
	SMOKE_CHECK(pr == 0);

	if (!fails)
		printf("compat_smoke: all checks passed\n");
	else
		printf("compat_smoke: %d FAILURE(S)\n", fails);
	return fails ? 1 : 0;
}
