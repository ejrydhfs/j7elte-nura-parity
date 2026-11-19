# Bluetooth deploy — BCM43438 on j7elte (hciattach + EFS address)

Device: SM-J700M `j7elte`, Nura (Alpine aarch64/musl), kernel
`3.10.108 #33 SMP PREEMPT Wed Nov 18 21:03:07 EST 2026 aarch64`.
Root via `rootshell`; no reboot and no flash were performed.

Source of truth: `code/pmaports/device/archived/j7elte-bt/` (+
`firmware-samsung-j7elte/APKBUILD`), `patches/bluetooth/0001-j7elte-bt-pinctrl.patch`,
`patches/bluetooth/0002-j7elte-bt-hci-config.patch`.

**Verdict: PASS for the userspace BT path.** The kernel has the H4 transport built in,
`hciattach` powered the controller, downloaded the patch RAM (logged by filename),
`hci0` came up with the EFS address, and a real discovery run listed 6 devices.
One item remains open: the `0001` pinctrl/DT hunk is **not** in the running `dt.img`
(attach and discovery do not need it; see §7).

---

## 1. Kernel support (config patch 0002 is live)

The running kernel already contains the HCI UART/H4 transport and the Samsung
`bcm43xx` platform driver; all are built in (no modules on this image):

```
# grep -E ' (hci_uart_init|h4_init|bcm43xx_bluetooth_probe)$' /proc/kallsyms
0000000000000000 t bcm43xx_bluetooth_probe
0000000000000000 t hci_uart_init
0000000000000000 T h4_init
```

The driver is bound to the platform device created from the DT `bluetooth` node:

```
# ls -l /sys/bus/platform/drivers/bcm43xx_bluetooth/
bluetooth.37 -> ../../../../devices/bluetooth.37
```

DT node (running tree):

```
# ls /proc/device-tree/bluetooth
compatible gpios name pinctrl-0 pinctrl-names status
# od -An -tx1 /proc/device-tree/bluetooth/gpios
 00 00 00 30 00 00 00 02 00 00 00 01  00 00 00 48 00 00 00 02 00 00 00 01
 00 00 00 48 00 00 00 06 00 00 00 0f
```

`gpios = <&gpc4 2 1  &gpa2 2 1  &gpa2 6 0xf>` → BT_EN on **gpc4-2**, BT_WAKE on
gpa2-2, BT_HOST_WAKE on gpa2-6, matching the ROM. The driver registers the rfkill
switch; after the service power-cycles it:

```
# rfkill list bluetooth
0: bcm43xx Bluetooth: Bluetooth
        Soft blocked: no
        Hard blocked: no
14: hci0: Bluetooth
        Soft blocked: no
        Hard blocked: no
```

Attach-side driver messages (the driver has no firmware consumer; the patch RAM is a
userspace/hciattach step):

```
# dmesg | grep '\[BT\]'
[ 1438.127922] [BT] set_wake_locked value = 0
[ 1438.329903] [BT] set_wake_locked value = 1
...
```

`CONFIG_BT_HCIUART=y` / `CONFIG_BT_HCIUART_H4=y`
(`patches/bluetooth/0002-j7elte-bt-hci-config.patch`) are therefore in effect at
runtime. No kernel rebuild was done for this deploy.

## 2. Firmware: path and download proof

The ROM blob (35 307 B, sha512
`5d563e9d6cc34efc4ae82e6b46b016daca04530243f3351373b528f82865eb95973154665e63f9694785be7bb40465b6577d5a4d021f854caba63941c8440365`)
is installed as:

```
/lib/firmware/postmarketos/samsung-j7elte/BCM43430A1.hcd
/lib/firmware/postmarketos/samsung-j7elte/bcm43438A1_V0051.0230.hcd   (ROM name, reference)
```

The name is not the ROM's: after a power cycle the controller's `Read Local Name`
returns **`BCM43430A1`**, and `hciattach bcm43xx` matches `<local name>*.hcd`
case-sensitively. `firmware-samsung-j7elte/APKBUILD` was corrected to install
`BCM43430A1.hcd` (previously `BCM43438A1.hcd`, which never matched; pkgrel 1).

The attach log kept by the service (`/var/log/j7elte-bt.log`) shows the download with
the exact path:

```
Flash firmware /lib/firmware/postmarketos/samsung-j7elte/BCM43430A1.hcd
Set Controller UART speed to 115200 bit/s
Device setup complete
bcm43xx_init
```

A/B check (same service, blob temporarily moved away and restored):

```
# mv BCM43430A1.hcd /tmp/ && rc-service j7elte-bt restart
/var/log/j7elte-bt.err:
Patch not found for BCM43430A1, continue anyway
# mv /tmp/BCM43430A1.hcd back && rc-service j7elte-bt restart
/var/log/j7elte-bt.log:
Flash firmware /lib/firmware/postmarketos/samsung-j7elte/BCM43430A1.hcd
Set Controller UART speed to 115200 bit/s
Device setup complete
```

After the download the controller advertises the firmware's identity:

```
Name: 'BCM43438A1 37.4MHz J7-LTE BT4.0-0051'
HCI Version: 4.0 (0x6)  Revision: 0xe6
Manufacturer: Broadcom Corporation (15)
```

(An unpatched attach earlier in the session reported `HCI Version: 4.1 (0x7)
Revision: 0x0` and the factory address `AA:AA:AA:AA:AA:AA`.)

Two controller quirks made this deterministic:

* bluetoothd writes the host name (`Samsung Galaxy J7`) into the controller, and it
  survives `HCI Reset`; a later attach then searches for `Samsung Galax*.hcd` and logs
  `Patch not found`. Toggling rfkill (`rfkill block; rfkill unblock`) power-cycles the
  chip (BT_EN) and restores `BCM43430A1`.
* `hciattach` lives at **`/usr/sbin/hciattach`** on this image (not `/usr/bin`); the
  service resolves it with `command -v`.

## 3. Service deployment

The package is files-only (`noarch`), so — following the `j7elte-wifi` precedent — it
was installed file-by-file as root, matching `package()`:

| path | mode | sha512 (matches APKBUILD) |
|---|---|---|
| `/etc/init.d/j7elte-bt` | 755 | `1fc268d5…d7bc518` |
| `/etc/conf.d/j7elte-bt` | 644 | `1d9ccebe…c45ac0` |
| `/usr/libexec/j7elte-bt-addr` | 755 | `4aa9a84b…18dd15` |
| `/usr/lib/systemd/system/j7elte-bt.service` | 644 | `f62f6a03…b97c16` |

`j7elte-bt` was added to the default runlevel (`rc-update add j7elte-bt default`);
`bluetooth` is already there (`bluez-openrc`). `j7elte-bt-addr` is new and implements
the EFS address step; the 0002 package patch was regenerated for the six files.

Startup sequence now (OpenRC):

1. stop `bluetoothd` if running (it caches the controller address),
2. `rfkill block bluetooth; rfkill unblock bluetooth` (power-cycle, deterministic
   firmware name),
3. `hciattach -n -t 30 -s 115200 /dev/ttySAC3 bcm43xx 115200 flow` (retries, output in
   `/var/log/j7elte-bt.log` / `.err`),
4. `j7elte-bt-addr /efs/bluetooth/bt_addr`,
5. `rc-service bluetooth start`.

The `before bluetooth` dependency was removed: OpenRC then makes the in-script
`rc-service bluetooth start` wait for `j7elte-bt` to finish starting, which timed out
after 60 s on every start. With the dependency removed the service completes in ~10 s:

```
# rc-service j7elte-bt restart
 * Detaching Bluetooth HCI transport from /dev/ttySAC3 ... [ ok ]
 * Stopping bluetoothd for the transport reset ...
 * Stopping Bluetooth ... [ ok ]
 [ ok ]
 * Power-cycling the Bluetooth controller ... [ ok ]
 * Attaching bcm43xx on /dev/ttySAC3 at 115200 (attempt 1/3) ... [ ok ]
 * Programming the Bluetooth public address from /efs/bluetooth/bt_addr ...j7elte-bt-addr: public address 34:14:5F:5D:E9:BC
 [ ok ]
 * Starting bluetoothd ...
 * Starting Bluetooth ... [ ok ]
 [ ok ]
# echo $?
0
```

## 4. hci0 and the controller address

```
# readlink -f /sys/class/bluetooth/hci0
/sys/devices/11060000.uart/tty/ttySAC3/hci0

# hciconfig -a
hci0:   Type: Primary  Bus: UART
        BD Address: 34:14:5F:5D:E9:BC  ACL MTU: 1021:8  SCO MTU: 64:1
        UP RUNNING
        RX bytes:1155 acl:0 sco:0 events:55 errors:0
        TX bytes:839 acl:0 sco:0 commands:56 errors:0
        Features: 0xbf 0xfe 0xcf 0xfe 0xdb 0xff 0x7b 0x87
        Packet type: DM1 DM3 DM5 DH1 DH3 DH5 HV1 HV2 HV3
        Link policy: RSWITCH SNIFF
        Link mode: PERIPHERAL ACCEPT
        Name: 'BCM43438A1 37.4MHz J7-LTE BT4.0-0051'
        HCI Version: 4.0 (0x6)  Revision: 0xe6
        LMP Version: 4.0 (0x6)  Subversion: 0x2209
        Manufacturer: Broadcom Corporation (15)
```

`bluetoothctl show`:

```
Controller 34:14:5F:5D:E9:BC (public)
        Manufacturer: 0x000f (15)
        Version: 0x06 (6)
        Name: Samsung Galaxy J7
        Alias: Samsung Galaxy J7
        Class: 0x0000020c (524)
        Powered: yes
        PowerState: on
        ...
        Roles: central
```

The address comes from EFS, exactly as on the stock ROM:

```
# cat /efs/bluetooth/bt_addr
34:14:5F:5D:E9:BC
```

`j7elte-bt-addr` brings `hci0` up, sends Broadcom `Write_BD_ADDR` (OGF `0x3f`,
OCF `0x0001`) with the bytes **reversed** (HCI sends BD_ADDR little-endian):

```
# hcitool cmd 0x3f 0x0001 0xBC 0xE9 0x5D 0x5F 0x14 0x34
< HCI Command: ogf 0x3f, ocf 0x0001, plen 6
  BC E9 5D 5F 14 34
> HCI Event: 0x0e plen 4
  01 01 FC 00
```

then `hciconfig hci0 reset` makes the kernel re-read it (addresses are only in
controller RAM; the service re-applies them on every start). Without the step the
controller keeps `43:43:A1:12:1F:AC`, and bluetoothd caches whatever exists at power-on.

## 5. Discovery (real output)

```
# bluetoothctl --timeout 20 scan on
SetDiscoveryFilter failed: org.bluez.Error.NotSupported
[CHG] Controller 34:14:5F:5D:E9:BC Alias: Samsung Galaxy J7
[CHG] Controller 34:14:5F:5D:E9:BC Class: 0x0000020c (524)
Discovery started
[CHG] Controller 34:14:5F:5D:E9:BC Discovering: yes
[NEW] Device 4D:C8:D3:87:2C:36 4D-C8-D3-87-2C-36
[NEW] Device 16:5D:88:75:A4:BC 16-5D-88-75-A4-BC
[NEW] Device 08:EF:3B:BE:33:7C 08-EF-3B-BE-33-7C
[NEW] Device 4E:C4:68:6A:AD:E8 4E-C4-68-6A-AD-E8
[NEW] Device 45:D3:0E:D3:11:64 45-D3-0E-D3-11-64
[NEW] Device 52:3A:53:29:65:3A 52-3A-53-29-65-3A
[CHG] Device 45:D3:0E:D3:11:64 RSSI: 0xffffffa8 (-88)
```

`bluetoothctl devices` afterwards lists the same 6 addresses; one of them
(`08:EF:3B:BE:33:7C`, `LG CM8450(7C)`) resolved its name from an earlier run, all
others are random/unknown LE addresses. `SetDiscoveryFilter failed:
org.bluez.Error.NotSupported` is expected: the 3.10 kernel has no
`HCI_OP_LE_SET_SCAN_PARAMS` discovery-filter support; discovery itself runs.

## 6. Why the address must be set before bluetoothd

Measured behaviour on this unit:

1. after attach + `hciconfig hci0 up`, the kernel reports the controller default
   `43:43:A1:12:1F:AC`;
2. `hcitool cmd 0x3f 0x0001 …` + `hciconfig hci0 reset` → hdev shows
   `34:14:5F:5D:E9:BC`;
3. `bluetoothd` reads and caches the address when it powers the adapter: starting it
   before step 2 leaves `bluetoothctl show` reporting the factory address even after
   the reset.

The helper therefore runs before `rc-service bluetooth start`, and the service stops
bluetoothd while the transport is recreated.

## 7. The 0001 pinctrl/DT hunk is not in the running DT

`patches/bluetooth/0001-j7elte-bt-pinctrl.patch` is listed in the kernel APKBUILD
(`linux-samsung-j7elte/APKBUILD:36`) and the repo's compiled DTBs in `build/dtbs/`
contain it (they carry both `gpa1-6` and `gpc4-2`), but the DT actually loaded by the
bootloader predates it:

```
# find /proc/device-tree -name 'bt-en'
/proc/device-tree/pinctrl@139B0000/bt-en      ← shared gpa1-6 group
# ls /proc/device-tree/pinctrl@138D0000/
compatible ese-cs-func gpc4 i2c-pdn_off i2c-touch-bus interrupts name
pinctrl-0 pinctrl-names reg sleep-state        ← no bt-en
# od -An -tx1 /proc/device-tree/bluetooth/pinctrl-0
 00 00 00 95 00 00 00 96 00 00 00 97           ← bt-hostwake, bt-btwake, bt_en(gpa1-6)
```

So `bluetooth` still references the gpa1-6 `bt-en` group. It does **not** block
Bluetooth: `bcm43xx` requests BT_EN as a GPIO and `gpio_direction_output()` muxes
gpc4-2 itself. Runtime pin state:

```
# grep -A3 gpc4 /sys/kernel/debug/gpio
GPIOs 172-174, platform/138d0000.pinctrl, gpc4:
 gpio-172 (melfas_sda_gpio     ) in  hi
 gpio-173 (melfas_scl_gpio     ) in  hi
 gpio-174 (bten_gpio           ) out hi
# grep 'gpc4-2' /sys/kernel/debug/pinctrl/138d0000.pinctrl/pins
pin 174 (gpc4-2)  CON(0x1) DAT(0x1) PUD(0x1) DRV(0x2) CON_PDN(0x0) PUD_PDN(0x0)
```

`CON(0x1)` = output and `DAT(0x1)` = high, i.e. BT_EN is driven correctly, but PUD is
the reset default pull-up and the deep-sleep field is `CON_PDN(0x0)`; the ROM's
patched group specifies `pud = 0` and `con-pdn = 3` (BT_EN low in suspend). That is the
only behavioural difference and it concerns suspend only.

**Not flashed.** Enabling the hunk requires rebuilding the kernel (`build/build.sh`,
which uses the APKBUILD config/patches) and writing the boot image; the running
`/boot/dt.img` is dated Nov 15 09:37 and is older than the running kernel
(`#33`, Nov 18). The reboot/flash procedure was out of scope for this deploy and is
not needed for attach or discovery. `build/dtbs/*.dtb` are already patched, so only
the packaging/flash step is missing.

## 8. Remaining items

* **0001 DT hunk not in the running image** — affects suspend pin power-down only;
  needs a kernel rebuild + boot-image flash (§7).
* **Discovery only.** No pairing/connection, no HID/A2DP/SCO test; the audio path
  (`bt_dummy`/`BT_UART_IN_AUDIO`) was not exercised.
* **Address is volatile.** The vendor write lives in controller RAM and is lost on
  power-off; the service re-applies it on every start, so nothing persists across a
  cold boot without the service running.
* **hciattach is deprecated** (`bluez-deprecated`); a future BlueZ would need
  `btbcm`/`hci_bcm` (not present in 3.10) or an hciattach replacement.
* **No 3 Mbit/s test.** The image runs at 115200 with hardware flow control; the
  higher speed remains an option in `/etc/conf.d/j7elte-bt`.
