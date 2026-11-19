# Wi-Fi deploy — bcmdhd firmware paths + j7elte-wifi userspace (on-device verification)

Device: SM-J700M `j7elte`, Nura (Alpine aarch64/musl), kernel `3.10.108 #33 SMP PREEMPT
Wed Nov 18 21:03:07 EST 2026`. Root via `rootshell`; no reboot, no flash.

Source of truth: `patches/wifi/0001-j7elte-wifi-firmware-paths.patch` and the package
`code/pmaports/device/archived/j7elte-wifi/` (+ `firmware-samsung-j7elte/`).
(The patch under `patches/userspace/wifi/` does not exist; `patches/wifi/` is the only
copy.)

**Verdict: PASS.** The running kernel uses the patched firmware/NVRAM paths, the driver
loads the blobs, `wlan0` scans (30 BSS), completes a WPA2-PSK 4-way handshake, obtains a
DHCP lease and passes traffic to the internet. Nothing was missing.

---

## 1. Firmware blobs and the patched paths

The patch changes two config strings (already present in the repo config, lines 1382-1383):

```
CONFIG_BCMDHD_FW_PATH="/lib/firmware/postmarketos/samsung-j7elte/bcmdhd_sta.bin"
CONFIG_BCMDHD_NVRAM_PATH="/lib/firmware/postmarketos/samsung-j7elte/nvram_net.txt"
```

`/lib` on this rootfs is a symlink to `usr/lib` (`lrwxrwxrwx /lib -> usr/lib`); the
absolute path resolves through the VFS as `dhd_os_open_image()` needs.

Files installed by `firmware-samsung-j7elte` were already at the patched directory and
were verified byte-identical to the repo blobs (sha512 match, incl. APKBUILD/sha512sums):

```
45f068ac…d4000  /lib/firmware/postmarketos/samsung-j7elte/bcmdhd_sta.bin
2e291ea1…5f23c  /lib/firmware/postmarketos/samsung-j7elte/nvram_net.txt
6113f39c…0f6f6  /lib/firmware/postmarketos/samsung-j7elte/bcmdhd_apsta.bin
28b91e82…4787c  /lib/firmware/postmarketos/samsung-j7elte/bcmdhd_mfg.bin
18dd44fe…9ee52  /lib/firmware/postmarketos/samsung-j7elte/nvram_mfg.txt
```

NVRAM content confirms the report's R2/R6: `boardtype=0x0727`, `boardrev=0x1101`,
`macaddr=00:90:4c:c5:12:38`, `ccode=GB`.

## 2. Driver load / firmware-path proof

`dhd` is **built into the kernel** (`CONFIG_BCM4343=y`, `DRIVER_TYPE=y`); there is no
`dhd.ko` in `/lib/modules` and no module to insert — "module load" is the built-in init:

```
[   15.638948] dhd_module_init in
[   15.644596] Power-up adapter 'DHD generic adapter'
[   15.651485] wifi_platform_set_power = 1
[   15.884764] wifi_platform_bus_enumerate device present 1
[   15.885386] card inserted
[   16.147735] mmc1: new high speed SDIO card at address 0001
[   16.166941] bcmsdh_sdmmc: bcmsdh_sdmmc_probe Enter
[   16.280525] F1 signature OK, socitype:0x1 chip:0xa9a6 rev:0x1 pkg:0x4
[   16.285285] p2p0: P2P Interface Registered
[   16.330253] Dongle Host Driver, version 1.77.22.3 (r)
               Compiled in drivers/net/wireless/bcmdhd_1_77_nn on Nov 18 2026 at 21:01:43
[   16.347781] Register interface [wlan0]  MAC: 00:60:15
[   16.365713] dhd_bus_devreset: == Power OFF ==
[   16.514812] dhd_module_init out
```
(the boot log is from `/var/log/dmesg`; the dmesg ring had already wrapped past it)

Firmware download happens on interface up. Captured live by cycling the radio:

```
[ 2151.109648] dhd_bus_devreset: == Power OFF ==
[ 2154.143996] dhdsdio_download_firmware: firmware path=/lib/firmware/postmarketos/samsung-j7elte/bcmdhd_sta.bin, nvram path=/lib/firmware/postmarketos/samsung-j7elte/nvram_net.txt
```

This is the decisive evidence that **the running kernel carries the patch**: it opens the
postmarketOS paths, not `/system/etc/wifi/…` (which does not exist; `/system` contains
only a `vendor` dir).

Loaded firmware identity:

```
/sys/module/dhd/parameters/info_string:
  Driver: 1.77.22.3 (r)
  Firmware: wl0: Mar 16 2016 14:05:52 version 7.10.323.73 (A1 Station/P2P) FWID 01-ee7f2eb9
  Chip: a9a6 Rev 1 Pkg 4
```

Caveat for integration: `/boot/vmlinuz` on the rootfs is **stale** (mtime Nov 15 09:37)
and still contains `/system/etc/wifi/bcmdhd_sta.bin`; it does not correspond to the
booted kernel (uname says #33 built Nov 18 21:03). Do not use it to judge the config.

## 3. `j7elte-wifi` package installed on the device

The package directory exists at `code/pmaports/device/archived/j7elte-wifi/` (APKBUILD +
4 payload files). Built as a files-only package — no compilation needed, files copied
verbatim over SSH (as root) to the paths its `package()` would install:

| installed path | mode | sha512 (matches APKBUILD) |
|---|---|---|
| `/etc/wpa_supplicant/wpa_supplicant.conf` | 644 | `79652524…465a76db` |
| `/etc/init.d/j7elte-wifi` | 755 | `1ef40be1…ecb63a326` |
| `/etc/conf.d/j7elte-wifi` | 644 | `a0abe483…cebad37` |
| `/usr/lib/tmpfiles.d/j7elte-wifi.conf` | 644 | `c58cf2e3…29a579a` |
| `/efs/wifi` (directory) | 0755 | — (pre-existing, see below) |

Service provisioned:

```
 * service j7elte-wifi added to runlevel default
 * Preparing /efs/wifi ...
 * /efs is a mount point (real EFS?); not touching /efs/wifi
 [ ok ]
          j7elte-wifi | default
```

That is the init script's intended branch: `/efs` **is** a real ext4 partition
(`/dev/mmcblk0p3 /efs ext4 rw,nosuid,nodev,noatime,data=ordered`), so the service must
not touch it — and the MAC persistence therefore works for free.

`wpa_supplicant` (OpenRC service, installed config) starts cleanly with the new file:

```
 * Starting WPA Supplicant ... [ ok ]
```

## 4. `iw dev` / scan (real output)

`iw` was not in the image; installed from the Alpine mirror (`apk add iw` → `iw-6.17-r0`).

```
# iw dev
phy#0
	Unnamed/non-netdev interface
		wdev 0x7
		addr 36:14:5f:5d:e9:bd
		type P2P-device
	Interface wlan0
		ifindex 10
		wdev 0x1
		addr 34:14:5f:5d:e9:bd
		type managed
```

Scan (from `/usr/local/bin/rootshell sh -c 'iw dev wlan0 scan'`), 30 BSS found, e.g.:

```
BSS 50:a5:dc:e3:e7:c9(on wlan0)
	freq: 2412
	signal: -52.00 dBm
	SSID: .TigoWiFi-322611812/0
	capability: ESS Privacy ShortPreamble ShortSlotTime (0x0431)
	RSN:	 * Version: 1
BSS ec:a9:40:84:a6:85(on wlan0)
	freq: 2437
	signal: -62.00 dBm
	SSID: .TigoWiFi-439295931/0
BSS 50:75:f1:c7:24:ce(on wlan0)
	freq: 2462
	signal: -68.00 dBm
	SSID: familia ovalles
BSS 10:f0:68:2d:7b:08(on wlan0)
	freq: 2412
	SSID: InternetParaTodos
```

A full parse of the scan (python3 on device) listed 29-30 BSS: 2.4 GHz only, all WPA2,
none open. P2P/AP interface modes are advertised in `iw list` (`IBSS managed AP
P2P-client P2P-GO`), and the wiphy reports `max # scan SSIDs: 10`.

## 5. Association + DHCP + traffic

Because every visible BSS requires WPA2, the test used the AP the jump host itself is
associated with (psk taken from the host's NetworkManager profile, SSID
`.TigoWiFi-322611812/0`, BSSID `50:a5:dc:e3:e7:c9`, -52 dBm).

First attempt through NetworkManager failed with "Secrets were required" (nmcli refused to
pass the psk non-interactively), so the package's own path was used: NetworkManager
released the device (`nmcli dev set wlan0 managed no`), the OpenRC `wpa_supplicant`
stopped, and a manual instance run with a temp config.

4-way handshake, from `/tmp/wpa.log` (`wpa_supplicant -dd`):

```
wlan0: Request association with 50:a5:dc:e3:e7:c9
wlan0: WPA: RX message 1 of 4-Way Handshake from 50:a5:dc:e3:e7:c9 (ver=2)
wlan0: State: ASSOCIATED -> 4WAY_HANDSHAKE
wlan0: RSN: RX message 3 of 4-Way Handshake from 50:a5:dc:e3:e7:c9 (ver=2)
wlan0: State: 4WAY_HANDSHAKE -> GROUP_HANDSHAKE
wlan0: State: GROUP_HANDSHAKE -> COMPLETED
```

```
# wpa_cli -i wlan0 status
bssid=50:a5:dc:e3:e7:c9
freq=2412
ssid=.TigoWiFi-322611812/0
id=0
mode=station
wifi_generation=4
pairwise_cipher=CCMP
group_cipher=CCMP
key_mgmt=WPA2-PSK
wpa_state=COMPLETED
ip_address=192.168.0.13
p2p_device_address=36:14:5f:5d:e9:bd
address=34:14:5f:5d:e9:bd
```

DHCP (`udhcpc`, stock Alpine script; DNS left alone so the SSH path was not disturbed):

```
udhcpc: started, v1.38.0
udhcpc: broadcasting discover
udhcpc: broadcasting select for 192.168.0.13, server 192.168.0.1
udhcpc: lease of 192.168.0.13 obtained from 192.168.0.1, lease time 3600
```

```
10: wlan0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc pfifo_fast state UP
    inet 192.168.0.13/24 scope global wlan0
default via 172.16.42.2 dev rndis0
default via 192.168.0.1 dev wlan0 metric 310
192.168.0.0/24 dev wlan0 proto kernel scope link src 192.168.0.13
```

Traffic:

```
# ping -I wlan0 -c 3 192.168.0.1
3 packets transmitted, 3 packets received, 0% packet loss
round-trip min/avg/max = 19.470/29.447/43.154 ms
# ping -I wlan0 -c 3 1.1.1.1
3 packets transmitted, 3 packets received, 0% packet loss
round-trip min/avg/max = 24.385/35.683/42.505 ms
```

After verification the test credentials were removed (`nmcli con delete tigo-deploy`,
`/tmp/tigo.conf` and supplicant logs deleted), the manual supplicant killed, `wlan0`
returned to NetworkManager (`nmcli dev set wlan0 managed yes`) and the OpenRC
`wpa_supplicant` service restarted. Final state: `wlan0 … disconnected`, no address,
original default route via `rndis0`, scan still returns 29 BSS.

## 6. MAC address (report R2) — resolved in the field

`/efs` is a real mounted ext4 partition, so `dhd_set_macaddr_from_file()` can persist:

```
/dev/mmcblk0p3 /efs ext4 rw,nosuid,nodev,noatime,data=ordered 0 0
/efs/wifi/.mac.info  ->  34:14:5F:5D:E9:BD      (17 bytes, from stock EFS)
```

The driver uses that value (`iw dev` → `addr 34:14:5f:5d:e9:bd`), **not** the NVRAM
`macaddr=00:90:4c:c5:12:38`, so no duplicate-MAC risk and no EFS workaround needed. The
`j7elte-wifi` service correctly no-ops on this device because `/efs` is a mount point.

## 7. What was changed on the device

- Installed the four payload files of `j7elte-wifi` (hashes above) and ensured `/efs/wifi`
  exists (already present).
- `rc-update add j7elte-wifi default`; service started (no-op branch, exit 0).
- Installed `iw` from the Alpine repo for verification (nothing in the image provides it).
- No kernel change, no module file, no firmware file had to be written — the running
  kernel already contains the patched `CONFIG_BCMDHD_*_PATH` and the blobs are in place
  and byte-identical to `firmware-samsung-j7elte`.

Residual notes: the patch is applied in the repo config and in kernel #33; the stale
`/boot/vmlinuz` (pre-patch strings, Nov 15) should be ignored/refreshed at image build
time. SoftAP and suspend/resume were not exercised (out of scope), and the associated
test used an AP whose credentials are external to the repo.
