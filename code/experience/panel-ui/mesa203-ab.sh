#!/bin/sh
# Run the smoke probe against the *build tree* (stock or patched panfrost)
# via dri-test/panfrost_dri.so, 4 runs, reporting fault classes and VAs.
B=/data/mesa203-build
export LD_LIBRARY_PATH="$B/build/src/egl:$B/build/src/mapi/es2api:$B/build/src/gbm:$B/build/src/mapi/shared-glapi:$B/build/src/glx"
export LIBGL_DRIVERS_PATH="$B/dri-test"
export MESA_LOADER_DRIVER_OVERRIDE=panfrost
export EGL_PLATFORM=surfaceless
i=0
while [ "$i" -lt 4 ]; do
	rootshell /bin/sh -c 'dmesg -c >/dev/null'
	SMOKE_W=64 SMOKE_H=64 timeout 25 "$B/mesa203-smoke" surfaceless >/dev/null 2>&1
	d=$(rootshell /bin/sh -c 'dmesg')
	data=$(printf '%s\n' "$d" | grep -c 'stat 81000')
	desc=$(printf '%s\n' "$d" | grep -c 'stat 80000')
	to=$(printf '%s\n' "$d" | grep -c 'p7580_job: TIMEOUT slot')
	va=$(printf '%s\n' "$d" | sed -n 's/.*MMU fault AS0: [A-Z_]*\/[A-Z]* stat [0-9a-f]* addr 0*\([0-9a-f]*\).*/\1/p' | tr '\n' ' ')
	printf 'run%s: data=%s desc=%s timeouts=%s va=%s\n' "$i" "$data" "$desc" "$to" "$va"
	i=$((i + 1))
done
