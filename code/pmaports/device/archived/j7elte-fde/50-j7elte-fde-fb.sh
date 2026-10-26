#!/bin/sh
# j7elte: make the DECON framebuffer and the touch panel usable for unl0kr.
#
# Runs from /hooks (init_2nd.sh:run_hooks /hooks) after udev has been started
# and before unlock_root_partition(), i.e. immediately before unl0kr is
# exec'd by fde-unlock. It must never fail the boot: every step is best
# effort and the script always exits 0.
#
# Why this is needed on j7elte specifically:
#  - /dev/fb0 is created unconditionally by the DECON driver at probe
#    (drivers/video/exynos/decon_7580/decon_core.c:4591 register_framebuffer),
#    but if anything blanked it (VT console sleep is enabled in this config)
#    unl0kr would draw into a powered-down panel.
#  - the panel backlight is a DECON/panel backlight device
#    (/sys/class/backlight/panel) and a zero brightness here means the unlock
#    UI is invisible even though it renders.
#  - there is no hardware keyboard (CONFIG_KEYBOARD_GPIO is only the volume
#    and power keys), so the touchscreen is the only way to enter the
#    passphrase; log what libinput will see for easier debugging.

LOG_PREFIX="j7elte-fde"
log() { echo "$LOG_PREFIX: $*"; }

# --- framebuffer -----------------------------------------------------------
if [ -e /dev/fb0 ]; then
	model="$(cat /sys/class/graphics/fb0/virtual_size 2>/dev/null)"
	log "framebuffer /dev/fb0 present (virtual_size=${model:-unknown})"

	# Unblank. DECON's fb_blank(FB_BLANK_UNBLANK) re-runs the panel
	# power-on sequence, so only touch it when it is actually blanked.
	blank="/sys/class/graphics/fb0/blank"
	if [ -r "$blank" ] && [ "$(cat "$blank" 2>/dev/null)" != "0" ]; then
		log "unblanking framebuffer (was $(cat "$blank" 2>/dev/null))"
		echo 0 > "$blank" 2>/dev/null
	fi
else
	log "WARNING: /dev/fb0 missing - unl0kr cannot draw the unlock screen"
fi

# --- backlight -------------------------------------------------------------
for bl in /sys/class/backlight/*; do
	[ -r "$bl/brightness" ] || continue
	brightness="$(cat "$bl/brightness" 2>/dev/null)"
	log "backlight ${bl##*/}: brightness=${brightness}"
	if [ "$brightness" = "0" ]; then
		max="$(cat "$bl/max_brightness" 2>/dev/null)"
		if [ -n "$max" ] && [ "$max" != "0" ]; then
			log "backlight ${bl##*/} is off, restoring to max ($max)"
			echo "$max" > "$bl/brightness" 2>/dev/null
		fi
	fi
done

# --- input -----------------------------------------------------------------
found=0
for ev in /dev/input/event*; do
	[ -e "$ev" ] || continue
	found=$((found + 1))
	name="$(cat "/sys/class/input/${ev##*/}/device/name" 2>/dev/null)"
	log "input $ev: ${name:-unknown}"
done
[ "$found" -gt 0 ] || log "WARNING: no /dev/input/event* - cannot enter the passphrase"

exit 0
