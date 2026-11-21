# Session state (survives context compaction)

Updated: 2026-11-19 (GPS GLME hand-off + ESW on hardware; modem CP power proven secure-only; GPU M1 built; USB link down pending physical re-plug). Target: **samsung-j7elte**, SM-J700M, Nura (pmOS) 3.10.108.

## Audio / UCM (2026-11-18)
- ADMA prealloc: `sound/soc/samsung/dma.c` `preallocate_dma_buffer_of()`
  ignored a failed `ioremap()` of the DT `samsung,tx-buf`; it now falls back to
  `dma_alloc_coherent()`. Built image `/tmp/kernel-boot.img`; auto-flash TWRP
  zip `/home/asdf/kernel-audiofix.zip` (dd p10 + reboot).
- UCM: playback verbs only disabled the `ADCL/ADCR` analog-bypass mixers and
  never enabled the `DACL/DACR` DAC mixers. Speaker also required PCM 1
  (`hw:${CardId},1`, the secondary DAI), S2801 `CH1 DOUT Select=AIF4IN`,
  `SRC1/MCKO/HQ En`, `MIX1_LVL=3`, and DAC soft-mute off. Fixed in
  `code/userspace/audio/ucm2` and the pmaports package; speaker and earpiece
  confirmed on hardware.
- Camera RE: dropped per user; no further camera work.

## Device access
- USB net: host `172.16.42.2/24`, device `172.16.42.1`; SSH `user@172.16.42.1` (password `nura`).
- Host tools: `SSH_ASKPASS=/tmp/ap.sh` (echoes `nura`), `setsid -w ssh ...`.
- Reboot: `echo 1 > /proc/sys/kernel/sysrq; echo b > /proc/sysrq-trigger` (**sync first** — sysrq-b does not sync; unsynced writes were lost once).
- Recovery: TWRP = Power 20 s, then Vol Up+Home+Power; then
  `adb push boot.img /data/ && adb shell 'dd if=/data/boot.img of=/dev/block/mmcblk0p10 bs=512 conv=fsync; sync; reboot'`.
  Known-good boot image: `/tmp/bindboot.img` (host).

## Working state on the phone
- Kernel: our build of `LineageOS/android_kernel_samsung_universal7580@2a746c1b`
  with patches `patches/camera/0001` (fw fallback), `0002` (FD optional),
  `0003` (ISP pin handover + rails re-enable at `fimc_is_ischain_power`).
  Rebuild: `code/kernel/build.sh` (GCC 16.2, `KCFLAGS=-Wno-error`), then
  `mkboot.py Image /tmp/bimg/ramdisk-new.gz /tmp/bimg/dt.img out.img "$(cat /tmp/bimg/cmdline)"`.
  **Never change the DTB payloads** — S-Boot hangs.
- `/usr` is bind-mounted from USERDATA in the initramfs (`patches/initramfs/0002`).
- Session: `rc-service xsession` -> `start-ui.sh` -> Xorg (with
  `LD_PRELOAD=/usr/local/lib/shadowfix.so`) -> `nura-shell` (phone UI).
- Firmware blobs in `/lib/firmware` (from stock J700MUBU2BRC2).
- `udev` v4l probe disabled: `/lib/udev/v4l_id.disabled` and
  `/etc/udev/rules.d/60-persistent-v4l.rules -> /dev/null`. **Re-apply after any
  apk transaction that reinstalls v4l-utils/udev.**

## Camera status (2026-11-18)
Green: sensor power sequencing, rails at voltage, reset high, MCLK 26 MHz
toggling at the pad, module EEPROM+cal CRC over I2C, signed firmware load,
ischain open, FD skipped.

Red: **both sensors are electrically silent on I2C.** A bit-bang master in
`fimc_is_ischain_power` (AP GPIOs gpf0-0/1 for ch0, gpf0-2/3 for ch1) scanned
0x08-0x77 at 100 kHz while the module was powered:
- rear: `VDD_CAM_SENSOR_A2P8 2.8 V, VDD_CAM_1P2 1.2 V, VDDIO 1.8 V,
  VDDAF 2.8 V`, reset gpe0-4 high, MCLK gate/rate/pad verified, full scan
  `none`;
- front: `VDD_VTCAM_A2P8 2.8 V, VDD_VTCAM_1P2 1.2 V, VDDIO_1.8V_VT 1.8 V`,
  `gpios_cam_en` toggling, reset gpe0-3 high, full scan finds **only the
  EEPROM at 0x10**, sensor at 0x20/0x5a never ACKs;
- rail power-cycle + 9-clock bus recovery + 100 ms reset delay changed
  nothing.
- The same NACK occurs with the **stock 3.10.61 vendor kernel** (boots the
  Nura rootfs), so this is not our kernel build.

Conclusion: the ISP-I2C/pin-mux theory is dead; the buses, EEPROMs, rails,
reset and MCLK are all verified good. The sensor dies themselves do not
respond, on both modules. Next step is hardware: reseat/measure the camera
connectors and the sensor core rails at the module.

### HIC_ISP_I2C_CONTROL (corrected)
Firmware command 28 is `HIC_SET_ISP_I2C_CONTROL` (index 28, not 27; kernel
enum agrees). Sending it with `param1=ch` is accepted but does not change the
NACK. Firmware uses fixed `g_I2C_SCLK=26 MHz`; I2CHAL computes
406 kHz/32 prescaler. Command 28's handler copies host data into a per-sensor
firmware buffer (AP->ISP I2C shadow), it does not initialise the ISP I2C block.

### Front flash + proximity (2026-11-18)
- Front flash (KTD2692, `front_torch_flash`) works. Patched for 5 levels
  (`patches/camera/0004`): laptop camera means 118.7/123.3/128.1/133.8/
  136.5/138.6 for off/L1..L5, p99 158->214. Confirms the front module is
  seated and powered.
- Proximity STK3013 on i2c-0 0x48 works: after
  `echo 1 > /sys/class/input/event4/device/enable`, `raw_data`/`state` read
  2 (far) and drop to 1 when covered. The input event only reports on
  transitions; polling the sysfs node is reliable.
- The camera sensors themselves still do not ACK; see Camera status above.

## Test loop (one attempt per boot!)
```
ssh user@172.16.42.1 'sudo -n sh -c "dmesg -c >/dev/null; timeout -s KILL 40 /usr/local/bin/j7elte-cam6 -t 25 -o /tmp/x.ppm; dmesg | grep -iE \"Sensor I2C|NACK|OpenSensor|handed to ISP|rails on\" | tail -15"'
```
Front camera: `/usr/local/bin/j7cam7 -f` (uses ss1 with the shared 30s/30c group).

## Key files
- `code/kernel/build.sh`, `code/userspace/camera/j7elte-cam/`,
  `code/experience/{nura-shell.c,shadowfix.c,start-ui.sh,...}`
- `patches/{camera,initramfs,xorg-fbdev,pmaports}/`
- `docs/HARDWARE-BRINGUP-2026-11-16.md` §1-23 (all evidence),
  `docs/SESSION-STATE.md` (this file).
- Host scratch: `/tmp/bimg/` (kernel, ramdisk, dt, cmdline), `/tmp/rom.zip`
  (stock ROM), `/tmp/fw.bin` (ISP firmware), `/tmp/cam2/` (tool source tree),
  kernel tree on `/run/media/asdf/db69437b-.../j7k/`.

### Latest firmware RE (2026-11-17, continued)
- `fimc_is_fw2_3l2.bin` stores its message-name table as **file offsets** (base 0),
  e.g. `HIC_OPEN_SENSOR` string at `0x2a293e`, referenced at `0x2acd48`;
  `HIC_SET_ISP_I2C_CONTROL` at `0x2a2841`, referenced at `0x2acd70`.
- Index distance between them is 10 entries = exactly the kernel enum
  (`HIC_OPEN_SENSOR` 17 -> `HIC_ISP_I2C_CONTROL` 27), so **firmware and kernel
  agree on command numbering**; no version mismatch. The command is simply
  never sent by the host.
- Next: recover the parameter block for command 27 (handler code near the
  table users), then emit it from `fimc_is_ischain_power` before the firmware's
  `OPEN_SENSOR`, mirroring `HIC_I2C_CONTROL_LOCK`
  (`fimc-is-interface.c:3418`).

### HIC_ISP_I2C_CONTROL sent (2026-11-17)
Implemented `fimc_is_hw_isp_i2c_control()` + call in `fimc_is_ischain_power`
right after `set_bit(FIMC_IS_ISCHAIN_LOADED)` (channel from the module pdata).

Result on hardware: `isp i2c control(instance 0, ch 0)` -> `-> 0`, firmware
accepts it and advances (`OpenStream`, `MAP VERSION check: SUCCESS`,
`CRC error check: SUCCESS`), but `OPEN_SENSOR` **still NACKs** on 0x5a
(`PushData I2C NACK ... ERROR_SENSOR_I2C_FAIL`).
So the command id is right and the command is accepted, but the payload
(param1=ch, rest 0) is not what configures the ISP I2C. Next: recover the real
parameter block for command 27 from the firmware handler, or try
param1 = ch|0x0100-style encodings used elsewhere in this driver.

## GPU memory writes proven; Mesa output still zero (2026-11-20, int19)
- `--pattern-test` PASS: page pre-filled 0xA5A5A5A5, Zero-fill job executed, **both USER and KERN
  views read 0x00000000** -> GPU writes memory and the coherency model is correct.
- Mesa 20.3.5 still yields zero pixels (FPS 690) while submitting many jobs that complete with
  `js_status=0`. So the defect is Mesa-batch-specific (job types/FBD/heap or dropped batches),
  not coherency or encoding.
- DEBUG_READ_BO ioctl (nr 0xCF) live for kernel-side BO inspection; strip before cutover.

## GPU executes, writes not visible (2026-11-20, int18)
- int18 has the coherency model (dma_map BIDIR at bind, sync_for_device at submit, sync_for_cpu at
  free_job, unmap at unbind). Jobs still complete cleanly (`js_status=0`, fence signalled) and Mesa
  submits many jobs (FPS 688), but both our WRITE_VALUE pattern test and `egltest` read zeros.
- Split required: does the GPU write the physical page at all (descriptor encoding), or is only the
  user alias stale? A kernel-side BO dump + descriptor re-derivation is in progress
  (`gpu31-gpuwrite.md`).

## Mesa drives the GPU; readback stale (2026-11-20, int17)
- After the ABI fix (tolerate SYNC_FILE on syncobj handle_to_fd; cap ioctl copies), Mesa 20.3.5
  submits real jobs: `p7580_sched: ioctl` x2, dispatched `jc=0x21c5480/0x21c5600/0x22ea480/0x22ea600`,
  `panfrost7580-job` IRQ count 96, FPS 2867 -> 1407 (work is happening).
- Remaining: `egltest` readback still all-zero -> GPU DMA writes not visible to CPU under
  COHERENCY_NONE (completion hook only does flush_dcache_page on the kernel alias; arm64, no HIGHMEM).
  Fix in progress (`gpu31-coherency.md`): proper dma_map/sync or dcache invalidate by vaddr.

## Completion IRQ works; Mesa ABI mismatch is the remaining blocker (int16)
- int16: job goes to **slot 0**, `post-start js_status=0x8 (NO_EXCEPTION)`, IRQ fires
  (`irq-enter rawstat=0x1`), fence signals immediately (`panfrost7580-job` count increments).
  Hardware completion path is therefore correct.
- Mesa 20.3.5 still renders nothing: strace shows **no `PANFROST_SUBMIT` at all**, plus
  `GET_PARAM` -> EINVAL (3x), `SYNCOBJ_HANDLE_TO_FD` -> EINVAL (3x), and 385 rejected
  MADVISE-class calls. Draws are no-ops because the UAPI handshake fails before any submit.
- Next: struct/flag alignment between our kernel headers and Mesa 20.3's drm-uapi
  (`gpu31-abialign.md`).

## Panfrost dispatches to hardware; completion IRQ missing (2026-11-20, int15)
- After fixing the scheduler (pump now calls run_job) and stamping file/drv at SUBMIT,
  one NULL job reaches hardware:
  `p7580_job: dispatch slot=1 jc=0x2000000 cfg=0x00083700 as=0 aff=0x3 seq=0`.
- It does **not** complete by interrupt: `panfrost7580-job` IRQ count stays 0
  (`panfrost7580-gpu` fired once), so the fence retires on the 5 s timeout.
- Note: submit selected slot 1; panfrost normally uses slot 0 for fragment jobs, and
  JOB_INT_MASK/RAWSTAT handling is unverified. Fix in progress (`gpu31-irq.md`).

## Why the GPU never ran (2026-11-20, int12 trace)
On-silicon trace of one NULL-job submit:
```
p7580_sched: ioctl jc=0x2000000 req=0x0 bo=1 in=0 out=1
p7580_sched: push job_count=1
p7580_sched: queued seq=0 slot=1 submit_rc=0
p7580_sched: pump job_count=1
p7580_sched: retire seq=0 slot=0
```
`run_job` is never called and the slot mutates 1->0: the 0004 `drm_sched` shim retires a dequeued job
without executing it. Hence fence signalled instantly, Mesa reports 2700 FPS, readback all zeros.
Fix in progress (`docs/reports/gpu31-pumpfix.md`): pump must call `sched_ops->run_job`, install its
fence, and retire only after completion.

## Hardware GL via panfrost (2026-11-20, int9)
- Full ioctl lifecycle PASS on silicon, including clean close: GET_PARAM 0x720, CREATE_BO,
  MMAP, SUBMIT rc=0, syncobj create/wait, `CLOSE: returned`, exit 0, watchdog disarmed.
- Mesa 20.3.5 (on-device build) + `MESA_LOADER_DRIVER_OVERRIDE=panfrost`:
  `GL vendor=Panfrost`, `renderer=Mali T720 (Panfrost)`, `GLES 3.0 Mesa 20.3.5`.
- egltest offscreen: 60 draws in 0.021 s (2889 FPS) **but readback all-zero** (RENDER_MISMATCH):
  either jobs retire without GPU execution, or GPU writes are invisible under COHERENCY_NONE
  (needs explicit dma_sync on completion). Tracing patch in progress (`gpu31-jobtrace.md`).

## Panfrost submit works on silicon (2026-11-20, int7)
- int7 (render-only + shmem BOs + job-lifetime PM + syncobj) boots with `renderD128`, X healthy.
- `--submit-test` on the real node: NULL job DESC written, **SUBMIT rc=0**, **WAIT_BO rc=0**
  (0 ms, 8 s deadline), `POST GET_PARAM rc=0 prod=0x720` — the GPU executed a job and the
  fence completed. This is the first hardware GPU job through our panfrost port.
- Remaining hang is **teardown**: fd close -> GEM free / MMU unmap / entity fini touches MMIO
  while the GPU is runtime-suspended, wedging the SoC until the watchdog resets (~14 s).
  Fix in progress (PM guards on teardown paths).
- Watchdog cap is ~19 s (SETTIMEOUT 60 -> EINVAL); `/data/wdarm 15 <cmd>` is the retry form.

## Panfrost submit hard-hang (2026-11-20)
- A single NULL-job submit **hard-hung the SoC** and took the USB gadget with it (no device on
  the bus). Safe ioctls are unaffected: after reset, `--probe-only` still PASSes
  (open, GET_PARAM 0x720, CREATE_BO gpu_offset 0x2000000, MMAP_BO, GET_BO_OFFSET).
- Prime suspect `F-SUB1`: `p7580_drv_submit -> p7580_drv_map_bo -> p7580_mmu_invalidate`
  issues MMIO before runtime-PM is guaranteed -> bus-level hang. Fix in progress
  (`docs/reports/gpu31-pm-order.md`).
- Safety net prepared: `code/gpu31/client/wdarm` (arms `/dev/watchdog`, 30 s) so a hang
  self-resets. No pstore backend on this kernel (no ramoops node).
- Mesa: installed 26.3 cannot bind a v5.2 UAPI node; a Mesa 20.3.5 build on device
  (`/data/mesabuild`) is the pairing candidate.

## Panfrost on silicon — int5 (2026-11-20)
- int5 (`dc892874…`) boots with **renderD128 only** (no `card0`), Xorg healthy, shell up;
  the earlier blank-screen/X-hang is gone by construction.
- Real-node ioctls PASS with `code/gpu31/client` selftest:
  open OK, `GET_PARAM` -> GPU_PROD_ID `0x720`, `CREATE_BO(4K)` -> handle 1 / gpu_offset
  `0x2000000000`, `MMAP_BO` -> `0x100000000`, `GET_BO_OFFSET` OK.
- Next failure is CPU map of the BO ("FAIL cpu map", no kernel trace, no hang); submit not
  reached yet. Fixed deadlocks: scheduler init at open, resv self-deadlock, bounded waits.

## Panfrost driver — first silicon boot (2026-11-19)
- int4 image (sha256 5678dca4…, `p7580_drv_probe`) **boots**: `/dev/dri/card0` + `renderD128`
  appear, fbdev stays intact, kbase `/dev/mali0` is gone (d4 cutover).
- Two defects found on silicon:
  1. **card0 must not exist** — Xorg auto-probes it and hangs in `D` state (blank screen,
     cursor dot). Render-only registration is the fix (in progress).
  2. **SUBMIT deadlocks uninterruptibly** — `pf7580-selftest` and Xorg stuck in `D` until reboot.
- Reverted to the M1 image (`cf25ed70…`) for a working UI; no `/dev/dri`, Xorg `S`, shell up.

## FM radio (2026-11-19)
- HCI path works: enable+tune/commit+RSSI all ACK (`status=0x00`) at 94.5 MHz, repeatable across
  fresh boots; mixer route applied (SRC3, CH3Rec, DOUT=RMIX_OUT, LINEIN on) — see `docs/reports/fm-audio.md`.
- Audio is blocked **below FM**: `hw:0,0` capture and `hw:0,0`/`hw:0,4` playback hang at the PCM
  layer (hw_ptr 0, avail 0, 0 bytes), while `hw:0,2` voice works — kernel ABOX/i2s0-pri stall.
- R(0x0F) CC is dataless on both HCI sockets, so RSSI/SNR values are not recoverable from userspace.

## GPU M1 boot result (2026-11-19)
- M1 image (DRM core built, sha256 cf25ed70…) flashed and booted as **#37**:
  USB-net, fbdev X 720x1280, /dev/mali0 all fine; **no /dev/dri** — dmesg has zero
  `drm` lines and /sys/class/drm holds only `version`, i.e. DRM core is present but
  no driver bound (vendor 3.10 has no exynos-DRM match for this SoC).
- Consequence: the render node must come from panfrost probing the stock
  `mali@0x11400000` node, not from exynos KMS. Driver assembly is in progress
  (code/gpu31/driver). Fallback image `/tmp/bindboot.img` verified.
- Reboot from the shell works; the launcher tile now uses the sysrq helper.

## Modem / CP power (2026-11-19) — impossible without stock TrustZone
Full evidence: `docs/reports/modem-tz.md`.
- Userspace (`j7elte-cbd` + `j7elte-ril`) is deployed and replays the stock SIPC/ioctl
  order (`MODEM_RESET 0x6f21` -> `SECURITY_REQ` mode 2/0 -> `XMIT_BOOT` -> `MODEM_ON`),
  reaching `INIT_END` on the SIPC link.
- CP power is a **secure-only** switch: `exynos_smc(0x82000700, ...)` READ returns `8`,
  WRITE (`0xA = CP_PWRON|CP_START`) is refused with `8`; `CP_CTRL` at PA `0x105C0030`
  is secure-only (normal-world reads return `+0x34/38/40/6C` only).
- Kinibi t-base-EXYNOS64-V015 has **no modem TA** (no .tlbin/mcRegistry entry), and
  `libsec-ril` has no TEE path; bootmode/cmdline/secboot/sys_rev/CP_RST_N/PMIC gates
  were all excluded by evidence.
- Verdict: CP cannot be powered from any non-Android kernel; needs the stock Android TZ
  stack or a bootloader-chain replacement.

## GPS / BCM4752 (2026-11-19) — chip answers BRI; NMEA not obtainable from it
Full evidence: `docs/reports/gps-bringup.md`, tool
`code/userspace/gps/src/gpsprobe.c`.

- The chip is powered and the UART works. The old "chip silent" result was an
  **ordering bug in `--init`**, not hardware: `RequestHwVersion` is ignored
  until the NVRAM exchange has run.
- Verified prologue on `/dev/ttySAC0` 921600 8N1, no flow control, after
  `GPS_PWR_EN` 0 (1 s) -> 1 (0.25 s):
  1. `80` x16 autobaud -> no announce (normal);
  2. `fe00fd001b07000000fc` -> no reply, but stores the word;
  3. `ff00fd001bfc` -> `fe00fd001b 07000000 fc`;
  4. `ff00fd401afc` -> `fe00fd401a 00000000 fc` (`020010a4` on a cold boot);
  5. `ff00fd0000fc` -> `fe00fd0000 05000230 fc` (version `0x30020005`);
  6. `ff00fd1f95fc` -> `fe00fd1f95 00000000 fc` (sub 0x95 = asic-ready ACK).
  `GetOutOfLegacy` is **not** answered here; after step 6 the chip is silent
  for 6 s and polls stay constant — no measurement stream, no NMEA.
- Flow control is irrelevant: `CRTSCTS`, manual RTS, both, and both after a
  power cycle gave zero rx over 10 s x3. 14-baud sweep (4800..4M) after a power
  pulse: zero rx. Pinmux pins 87-90 (gpb0-4..7) -> `13800000.uart`; gpio137
  writes move the pin `out lo`/`out hi`.
- NMEA is produced by `glgps`, never by the chip; the remaining vendor stages
  (flow-control search, ESW upload, RF/clock plan, measurement start) are
  run-time computed and the ROM has no ESW blob.
- `j7elte-gpsd --init` now sends the NVRAM pair before `RequestHwVersion` and
  no longer aborts on the unannounced autobaud; verified on hardware
  (`gps-toolchain.md`), patch regenerated in `patches/userspace/gps/`.
- Kernel is back to **stock parity**: `sec_gps_bcm4752.c` is the vendor file
  again (GPIO only, `gpio_direction_output(..., 0)`, no regulator/regmap
  writes). Built `#33`, flashed, and the full prologue above reproduces
  unchanged — so no PMIC deviation is needed or wanted.
- **GLME hand-off verified on hardware (2026-11-19)**: cold boot, vendor-parity
  prologue, ESW blocks 0-2 (skipping the synthetic 0x7fd2 record), CRC acks
  `f107730c/ec000075/8109aed2/0b05ed95`, then `fe00fd000055aa00fffc` ->
  `e0 00 00 00 07 10 02 01 72 06 30 02 00 05 5f e0 01`. `gpsesw --golden`
  exits 0; only one golden run per boot (power gating does not reset the chip).
- ESW: the chip is **not** hostless; `gpsd` carries the 8051 ESW/RAM patch in
  `.rodata` and uploads it (`Bri_EswLoadBlockAndRequestCrc`, 24-byte records).
  Public hardware-verified spec: github.com/mbicz/bcm4752-init (Lenovo Yoga
  Book). Remaining work: implement the escape-pair
  search (`Bri_ProgramSwFlowControl` 0x2a6aa0), RF plan, then parse GLME and
  synthesise NMEA host-side (chip never emits NMEA).
- **ESW upload verified** (2026-11-19): the four 4752_B2 records go up as
  `fe <nwords> fd 61 17 de c0 <addr> … fc` (1020-byte frames, 4096-byte CRC
  pieces) and all five `21 18` requests are acked with `2f15`
  (`0x7fec/4` matches the public capture byte for byte). The patch bank is
  item `0x16`, read back as item `0x94`. The GLME switch stops BRI replies
  but yields no `e0` banner, so the post-patch jump is the open step.
  Tools: `j7elte-gpsd --esw DIR [--glme]`, `gpsesw`; evidence in
  `docs/reports/gps-esw-upload.md`.
