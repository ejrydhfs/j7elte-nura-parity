# BrcmGpsHalGpio{Sysfs,Ioctl} — recovered bring-up sequence (stock gpsd, j7elte)

Target: `/tmp/stock_gpsd` (ELF32 ARM, Android 23, stripped, prelinked PIC).
Goal: exact GPIO bring-up the stock Broadcom HAL performs, at syscall level, so a C
program can reproduce it byte for byte.

Everything below is read out of the binary. Items that are *not* literally in the
disassembly are tagged `[INFERENCE]`.

---

## 0. How the code was located (so the addresses can be re-checked)

* `.text`  = vaddr 0x18990..0x2dda9e (file offset == vaddr for .text/.rodata).
* The binary is PRELINKED PIC: **no relocation entries exist in .text**. All
  `.rodata`/`.data` references are `ldr rX,[pc,#imm]` + `add rX,pc` pairs whose
  literal pool word is the prelinked delta `string_addr - (add_addr + 4)`.
  (GOT slots in `.got` 0x3d8b70..0x3d9fff are relocated with `R_ARM_RELATIVE`;
  config *key* strings are reached through those slots.)
* PLT map used below (decoded from `.rel.plt`):
  `0x17a9c strcmp, 0x17aa8 open, 0x17ab4 write, 0x17ac0 __errno, 0x17acc strerror,
   0x17ad8 close, 0x17ae4 usleep, 0x17afc strncpy, 0x17b08 opendir, 0x17b14 readdir,
   0x17b20 strncmp, 0x17b2c snprintf, 0x17b38 read, 0x17b44 strlen, 0x17b50 closedir,
   0x17b68 __strlen_chk, 0x17b74 ioctl`.
* Every GPIO function of both classes lives in the single cluster
  **0x199d8 .. 0x1a3a0**. Xrefs (add-pc literals) pinning each format string:

| string | vaddr | referencing `add rX,pc` |
|---|---|---|
| `BrcmGpsHalGpio::%s\n` | 0x2f065c | 0x19a46, 0x19c9a, 0x19e32 |
| `gpioDelayMs` | 0x2f0670 | 0x19a54 |
| `ioctlNStdby` | 0x2f067c | 0x19a86 |
| `LPmode` / `true` | 0x2f0688 / 0x2f0690 | 0x19b8a / 0x19b9c |
| `/sys/class/power_supply/battery/gps` | 0x2f0698 | 0x19bcc, 0x19cfe, 0x19d16, 0x19d64, 0x19e56, 0x19e6e |
| `opening file: %s (%d) \n` | 0x2f06dc | 0x19be8, 0x19d80 |
| `writting to GPIO %s: (%s) \n` | 0x2f072c | 0x19c08, 0x19da0 |
| `Power enable delay: %dms\n` | 0x2f0748 | **0x19cca** |
| `GpsHalGpio{Set,Clear} LPmode set {start,stop,none} %s` | 0x2f0764/88/b0/d4 | 0x19d14, 0x19cfc, 0x19e6c, 0x19e54 |
| `rfkillNStdby` `gpioNResetPath` `gpioNStdbyPath` | 0x2f0800/10/20 | 0x19ea6, 0x19eb2, 0x19ebe |
| `/sys/class/rfkill`, `rfkill`, `%s/%s/type`, `%s/%s/state` | 0x2f0830/44/4c/58 | 0x19f30, 0x19f42, 0x19f44, 0x19fe0 |
| `No GPIO file for %d` | 0x2f0864 | 0x1a046 |
| `Value %c not recognized` | 0x2f0934 | 0x1a0cc |
| `%s pin [%d] value[%s] successful` | 0x2f0958 | 0x1a13c |
| `%s : nStdby value before clearing = %d` | 0x2f09a4 | **0x1a1d2** |
| `acPortName`, `ioctl: %d:%s`, `GPIO %d set to %d` | 0x2f09cc/d8/09e8 | 0x1a20e, 0x1a2ac, 0x1a262 |
| `GPIO states %X (pin %d state %d)` | 0x2f0aa0 | 0x1a312 |

---

## 1. Configuration contract (gps.xml / property table)

Descriptor table at `.got` 0x3d9408, 0x14 bytes per entry:
`{default(0), "Name", "xmlKey", "type", 0, sizeof}`.

| xml key | type | size | default |
|---|---|---|---|
| `gpioNStdbyPath`  | s | 0x100 | "" |
| `gpioNResetPath`  | s | 0x100 | "" |
| `gpioDelayMs`     | l | 4     | 0 |
| `rfkillNStdby`    | s | 0x20  | "" |
| `ioctlNStdby`     | l | 4     | 0 |
| `acPortName`      | s | 0x100 | "" |

No path is hard-coded in the binary: **all sysfs/ioctl identities come from gps.xml.**

---

## 2. Object layout (recovered field stores)

Base class `BrcmGpsHalGpio`, ctor at **0x19a38**:

```
019a38 push {r3,r4,r5,lr}; r5=r0
019a40..019a4c  log(3,"BrcmGpsHalGpio::%s\n", 0x2f06?? "Init")   ; r3 -> pool -> "Init"
019a50 ldr r1,[pc,#0x18] ; add r1,pc   ; "gpioDelayMs"
019a52 adds r0,r5,#4                   ; &this->gpioDelayMs
019a56 pop {r3,r4,r5,lr}; b.w 0x43598  ; cfg_get_string(&this->+4, "gpioDelayMs")
```

| off | base | Sysfs (0x19e98 tail) | Ioctl (0x1a1fc) |
|---|---|---|---|
| +0x00 | vtable/table ptr | — | — |
| +0x04 | `gpioDelayMs` (ASCII string, later `atoi()`d) | — | — |
| +0x0c | — | `rfkillNStdby` (rfkill *type* to match) | `ioctlNStdby` (pin index, long) `[INFERENCE]` |
| +0x10 | — | — | `acPortName` (device node) |
| +0x14 | — | `gpioNResetPath` (path pointer) | — |
| +0x1c | — | `gpioNStdbyPath` (default path) | — |
| +0x24 | — | 256-byte buffer: resolved nStdby path | — |

Sysfs ctor tail (0x19e98):

```
019e9c bl 0x19a38                    ; base ctor
019ea0 ldr r1,[pc,#0x34] / 019ea6 add r1,pc -> 0x2f0800 "rfkillNStdby"
019ea2 add.w r0,r4,#0xc  ; 019ea8 bl 0x43414   ; cfg_get_string(&this->+0xc, "rfkillNStdby")
019eac add r1,pc -> 0x2f0810 "gpioNResetPath"
019eae add.w r0,r4,#0x14 ; 019eb4 bl 0x43414   ; cfg_get_string(&this->+0x14, ...)
019eb8 add r1,pc -> 0x2f0820 "gpioNStdbyPath"
019eba add.w r0,r4,#0x1c ; 019ec0 bl 0x43414   ; cfg_get_string(&this->+0x1c, ...)
019ec4 add.w r0,r4,#0x24 ; 019ec8 movs r1,#0 ; 019eca mov.w r2,#0x100
019ed2 b.w 0x2dd84c                          ; memset(&this->+0x24, 0, 256)
```

---

## 3. Backend A — `BrcmGpsHalGpioSysfs` (paths, not ioctls)

### 3.1 `openPin(pin)` — 0x1a00c

```
019a12 cbz r1,#0x1a032          ; pin == 0  -> nReset path
019a14 cmp r1,#1 ; 019a16 bne 0x1a03c      ; anything else -> "No GPIO file for %d"
; pin == 1 (nStdby):
019a18 ldrb.w r3,[r0,#0x24]     ; resolved path buffer
019a1c adds r4,r0,#0x24
019a1e cbz r3,#0x1a072          ; empty -> run rfkill discovery (0x19ee4)
019a20 mov r0,r4 ; 019a22 movs r1,#2        ; O_RDWR
019a24 blx open                            ; open(path, O_RDWR)
...
019a32 ldr r4,[r0,#0x14]        ; pin 0: path = this->+0x14 (gpioNResetPath)
019a34 cbz r4,#0x1a03c          ; NULL -> "No GPIO file for %d"
019a36 ldrb r0,[r4] ; 019a3a bne 0x1a020   ; non-empty -> open(path, O_RDWR)
019a3c->0x1a046  log(3,"No GPIO file for %d\n", pin); return -1
```

Return value = fd (`>=0`) or -1.

### 3.2 `getPin(pin)` — 0x1a08c (only ever called with pin = 1)

```
01a092 bl 0x1a00c                 ; fd = openPin(1)
01a098 blt  -> return -1
01a09e movs r2,#1 ; 01a0a0 blx read(fd,&c,1)
01a0a4 cmp r0,#0 ; 01a0a6 ble -> "Error reading from GPIO: %d (%s)" -> -1
01a0ac r3 = c - '0' ; 01a0b0 cmp r3,#1 ; 01a0b4 movls r4,r3   ; accept only '0'/'1'
01a0b8 blx close(fd)                                          ; 0x17ad8
01a0c4.. log(3,"Value %c not recognized\n", c) / return -1
```
Returns 0 or 1.

### 3.3 `setPin(pin, value)` — 0x1a100

```
01a108 bl 0x1a00c                 ; fd = openPin(pin)   (O_RDWR)
01a10e ble -> return fd (<=0)
01a110 cbz r5,#0x1a152            ; value == 0 branch
; value != 0:
01a112 ldr r5,[pc] ; 01a118 add r5,pc        ; buffer in .data @ 0x3da008
01a116 movs r2,#0x31 ; 01a11c strb r2,[r5]   ; buffer[0] = '1'
01a11e blx __strlen_chk(buf,3)               ; 0x17b68  -> len = 1
01a128 blx write(fd, buf, len)
01a12c cmp r0,#0 ; 01a12e ble -> error path
01a13c log(3,"%s pin [%d] value[%s] successful\n", "WriteSysfs", pin, buf)
01a14e b.w 0x2dd83c                          ; close(fd)
; value == 0:
01a156 movs r3,#0x30 ; 01a15c strb r3,[r5]   ; buffer[0] = '0'  (same path)
```
So a pin write is exactly **one byte, `'1'` or `'0'`, no newline**.

### 3.4 `SetNStdby(1)` — 0x1a1f4 (8 bytes) / `ClearNstdbyPin()` — 0x1a1c4

```
01a1c4 push {r4,lr}; r4=r0
01a1c8 ldr r3,[r0] ; 01a1ca ldr r1,[r3,#0x14] ; 01a1cc blx r1   ; v[0x14]() == getPin(1)
01a1ce.. log(3,"%s : nStdby value before clearing = %d\n",
                "ClearNstdbyPin", value)
01a1de movs r1,#1 ; 01a1e2 movs r2,#0
01a1e8 b.w 0x1a100                    ; return setPin(1, 0)

01a1f4 movs r1,#1                     ; 0x1a1f4 (separate entry point)
01a1f6 mov   r2,r1
01a1f8 b.w   0x1a100                  ; return setPin(1, 1)
```

### 3.5 rfkill nStdby path discovery — 0x19ee4

```
019ef4 ldr r1,[r0,#0x1c]              ; this->+0x1c = gpioNStdbyPath
019f00 mov r0,sl (=this+0x24) ; 019efc mov.w r2,#0x100
019f08 blx strncpy                    ; default path copied into this->+0x24
019f0c ldr.w sb,[r4,#0xc]             ; sb = this->+0xc = rfkillNStdby (type)
019f16 ldrb.w r1,[sb] ; 019f1a cbnz r1,#0x19f2e    ; empty -> keep default, return
019f2e -> "/sys/class/rfkill" ; 019f32 blx opendir
loop readdir(dir):
019f5a strncmp(name, "rfkill", 6) != 0        -> next
019f6a snprintf(tmp, 0xff, "%s/%s/type", "/sys/class/rfkill", name)
019f7c fd = open(tmp, O_RDONLY)               ; 019f7e r1 = 0
019f96 read(fd, buf32, 32)                    ; 019f9a..0x19fa6
019fb0 close(fd)
019fba len = strlen(rfkillNStdby) ; min(len,32)
019fcc strncmp(typeBuf, rfkillNStdby, min) != 0 -> next
019fd4 snprintf(this->+0x24, 0x100, "%s/%s/state", "/sys/class/rfkill", name)
019fe6 closedir(dir)
```
So the nStdby pin is *not* a GPIO number in the sysfs backend: it is the
`state` file of the rfkill device whose `type` file equals `rfkillNStdby`
(e.g. `gps`). If `rfkillNStdby` is empty, `gpioNStdbyPath` is used verbatim.

### 3.6 Never touched: nReset (pin 0)

`openPin(0)`/`setPin(0,*)` are reachable but **have no caller in this build** —
the only `b.w/bl` to 0x1a100 are from 0x1a1e8 and 0x1a1f8 (both pin = 1), and the
only callers of 0x1a00c are 0x1a092 (pin 1) and 0x1a108 (pin from setPin).
`gpioNResetPath` is therefore dead configuration in the sysfs path.

---

## 4. Backend B — `BrcmGpsHalGpioIoctl`

### 4.1 `setPin(pin, value)` — 0x1a218

```
01a21e ldr r4,[r0,#0x10]              ; this->+0x10 = acPortName
01a220 movs r1,#0 ; 01a22a blx open(path, O_RDONLY)
01a232 ble -> log "%s %d Error opening %s (%d:%s)"
01a234 and r4,r7,#1                   ; value & 1
01a238 add.w r3,r6,#8                 ; pin + 8
01a240 lsls r4,r3                     ; (value&1) << (pin+8)
01a242 lsls r5,r6                     ; 1 << pin
01a248 orrs r4,r5                     ; arg = (value&1)<<(pin+8) | 1<<pin
01a244 movw r1,#0x4302 ; 01a24a movt r1,#0x4002   ; req = 0x40024302
01a24e str r4,[r2,#-4]!               ; arg on stack
01a252 blx ioctl(fd, 0x40024302, &arg)
01a258 blt -> log "ioctl: %d:%s" (fd, strerror)
01a262 log(3,"GPIO %d set to %d\n", pin, value)
01a268 close(fd)
```

### 4.2 `getPinState()` — 0x1a2d8

```
01a2dc ldr r5,[r0,#0x10]                       ; acPortName
01a2e2 mov.w r3,#-1 ; 01a2e8 str r3,[sp,#0x14] ; state = -1
01a2e0 blx open(path, O_RDONLY)
01a2f4 movw r1,#0x4301 ; 01a2fa movt r1,#0x8001 ; req = 0x80014301
01a2fe blx ioctl(fd, 0x80014301, &state)
01a304 blt -> log "%s %d: error [%s]"
01a306.. log(3,"GPIO states %X (pin %d state %d)\n", state, pin, (state>>pin)&1)
01a31e return (state >> pin) & 1 ; close(fd)
```
`pin` here is `this->+0x0c` = `ioctlNStdby` (see 0x1a2c8 `ldr r1,[r0,#0xc]`, 0x1a2d0).
ioctl numbers: **set = 0x40024302, get = 0x80014301**, direction bits in the
`_IOR/_IOW` style (`0x4002` = _IOW('B',2), `0x8001` = _IOR('B',1), size 4 bytes).

Note: the ioctl backend opens the port **O_RDONLY** even for the set ioctl.

---

## 5. The power state machine (the deliverable)

`GpsHalGpioSet` — **0x19c8c**. Its table slots are dispatched as
`ldr r3,[this]; ldr rX,[r3,#slot]; blx rX` with `this` only (no extra args), so
the three one-argument slots are exactly the three one-argument methods:
slot 0x14 = `getPinState()` (0x1a08c / 0x1a2d8-variant), slot 0x18 = "nStdby on"
(0x1a1f4), slot 0x1c = "ClearNstdbyPin" (0x1a1c4).

```
019c8c push {r4,r5,r6,lr}
019c90..019ca0 log(3,"BrcmGpsHalGpio::%s\n", "Normal")
019c9a add r2,pc -> 0x2f065c           ; "BrcmGpsHalGpio::%s\n"
019c9c add r3,pc -> "Normal"
019ca4 ldr r3,[r4] ; 019ca8 ldr r1,[r3,#0x14] ; 019caa blx r1     ; s = getPinState()
019cac cmp r0,#1
019cae beq 0x19d20                     ; already ON
019cb0 ldr r3,[r4] ; 019cb8 add r5,pc...
019cb2 adds r6,r4,#4                   ; &gpioDelayMs
019cba ldr r4,[r3,#0x18] ; 019cbc blx r4                            ; nStdby = 1
019cbe mov r0,r6 ; 019cc2 bl atoi                                   ; d = atoi(gpioDelayMs)
019cc6 ldr r2,[pc] ; 019cca add r2,pc -> 0x2f0748                   ; "Power enable delay: %dms\n"
019ccc mov r3,r0 ; 019cd0 bl log
019cd4 mov r0,r6 ; 019cd6 bl atoi                                   ; d again
019cda rsb r1,r0,r0,lsl #5    ; 31*d
019cde add.w r0,r0,r1,lsl #2  ; d + 124d = 125d
019ce2 lsls r0,r0,#3          ; 1000*d
019ce4 blx usleep             ; usleep(1000*d)  == d milliseconds
019ce8 ldr.w ip,[pc] ; 019cec add ip,pc -> 0x3dfabc                 ; LPmode flag
019cee ldr.w r2,[ip] ; 019cf2 cbnz r2,#0x19d08
019cf4 ldr r2,[pc] ; 019cfc add r2,pc -> 0x2f0788   ; "...LPmode set none %s"
019cfe add r3,pc -> 0x2f0698                        ; battery/gps path
019d04 b.w 0x19a18                                  ; log only
019d08 bl 0x19bc4                                   ; battery/gps := "1\n"
019d0c ldr r2,[pc] ; 019d14 add r2,pc -> 0x2f0764   ; "...LPmode set start %s"
019d16 add r3,pc -> 0x2f0698
019d1c b.w 0x19a18
; --- already-ON branch ---
019d20 ldr r2,[r4] ; 019d24 ldr r6,[r2,#0x1c] ; 019d26 blx r6       ; nStdby = 0 (+print old)
019d28 movw r0,#0xc350
019d2c blx usleep                                                  ; 50000 us = 50 ms
019d30 b 0x19cb0
```

`GpsHalGpioClear` — **0x19e24**:

```
019e40 ldr r1,[r3,#0x1c] ; 019e42 blx r1      ; ClearNstdbyPin(): log old value, nStdby = 0
; then LP-mode branch exactly as above, using 0x19d5c ("0\n") and
; "GpsHalGpioClear LPmode set stop %s" / "...set none %s"
```

### 5.1 Clock-accurate power-ON sequence

`d = atoi(config gpioDelayMs)` (defaults 0 if unset; gps.xml supplies the real value).

| # | t | operation | path / arg | value | delay |
|---|---|---|---|---|---|
| 0 | 0 | log only | `BrcmGpsHalGpio::Normal` | — | 0 |
| 1 | 0 | open+read+close | nStdby path (see §3.5) | 1 byte, `'0'`/`'1'` | 0 |
| 2a | 0 | *(only if read == 1)* log+open | nStdby path, `O_RDWR` | write `'0'` (1 byte) | 0 |
| 2b | ~0 | usleep | — | — | **50 000 µs** |
| 3 | +50 ms | open | nStdby path, `O_RDWR` | write `'1'` (1 byte) | 0 |
| 3b | +50 ms | close(fd) | — | — | 0 |
| 4 | +50 ms | log `Power enable delay: %dms` | d | — | 0 |
| 5 | +50 ms | usleep | — | — | **d × 1000 µs** |
| 6 | +50+d ms | *(if LPmode)* open | `/sys/class/power_supply/battery/gps`, `O_RDWR` | write `"1\n"`, **6 bytes** | 0 |
| 6b | | close(fd) | — | — | 0 |

Notes that matter for our bring-up:
* The **"Power enable delay" is placed AFTER the nStdby pin is driven high** and
  after its `close()`; it is logged immediately before the `usleep`.
* The chip is taken out of standby by asserting **nStdby = 1**; the 50 ms
  low-pulse in step 2 only happens when the pin was *already* high (it is a
  "re-arm the chip" path, not a hard reset).
* `nReset` is never driven (see §3.6).
* LP-mode is an independent, second write to the battery power-supply node.

### 5.2 Clock-accurate power-OFF sequence

| # | operation | path | value |
|---|---|---|---|
| 1 | read old value (log `nStdby value before clearing = %d`) | nStdby path | via read |
| 2 | open/write/close | nStdby path, `O_RDWR` | `'0'`, 1 byte |
| 3 | *(if LPmode)* open/write/close | `/sys/class/power_supply/battery/gps` | `"0\n"`, **5 bytes** |
| 4 | log | `GpsHalGpioClear LPmode set {stop,none}` | — |

No delays on the power-down path.

### 5.3 LP-mode writers (0x19bc4 set / 0x19d5c clear)

```
019bcc path = "/sys/class/power_supply/battery/gps"
019bd0 open(path, O_RDWR)   ; 019bd6 blt -> "Error opening file: %s (%s) \n"
019bec log(3,"opening file: %s (%d) \n", path, fd)
019be4 r7 = 0x2f06f4 ("1\n")   / clear: 0x2f07ac ("0\n")
019bf4 movs r2,#6              ; clear: 0x19d8c movs r2,#5
019bf6 write(fd, r7, len)
019bfc blt -> log "Error writting to GPIO %s: (%s) \n" with "LP_MODE_PATH"
019c08 log(3,"writting to GPIO %s: (%s) \n", path, r7)
019c16 close(fd)
```
The buffer holds `"1\n"`/`"0\n"`; the count is 6/5 because the literals were laid
out in a 4-byte aligned pool (the extra bytes are `\0`). Send at least the
first byte; to be clock/byte-identical use 6 bytes `31 0a 00 00 00` and 5 bytes
`30 0a 00 00 00`.

---

## 6. C implementation of the recovered sequence

```c
/* config: string gpioNStdbyPath, string gpioNResetPath (unused),
   long gpioDelayMs, string rfkillNStdby (e.g. "gps"),
   long ioctlNStdby, string acPortName;  bool LPmode ("LPmode"=="true") */

static int sysfs_write_byte(const char *path, char c)   /* BrcmGpsHalGpioSysfs::setPin */
{
    int fd = open(path, O_RDWR);
    if (fd < 0) return -1;
    if (write(fd, &c, 1) != 1) { close(fd); return -1; }
    close(fd);
    return 0;
}

static int sysfs_read_byte(const char *path)            /* ...::getPin */
{
    char c; int fd = open(path, O_RDWR);
    if (fd < 0) return -1;
    if (read(fd, &c, 1) != 1) { close(fd); return -1; }
    close(fd);
    if (c < '0' || c > '1') return -1;
    return c - '0';
}

/* BrcmGpsHalGpioSysfs::rfkill discovery (only when rfkillNStdby is non-empty) */
static void resolve_nstdby_path(char out[256], const char *rfkill_type,
                                const char *default_path)
{
    strncpy(out, default_path, 256); out[255] = 0;
    if (!rfkill_type || !rfkill_type[0]) return;
    DIR *d = opendir("/sys/class/rfkill");
    if (!d) return;
    struct dirent *de;
    char tmp[256], tbuf[32];
    while ((de = readdir(d))) {
        if (strncmp(de->d_name, "rfkill", 6)) continue;
        snprintf(tmp, sizeof tmp, "%s/%s/type", "/sys/class/rfkill", de->d_name);
        int fd = open(tmp, O_RDONLY);
        if (fd < 0) continue;
        ssize_t n = read(fd, tbuf, 32);
        close(fd);
        if (n <= 0) continue;
        size_t l = strlen(rfkill_type); if (l > 32) l = 32;
        if (strncmp(tbuf, rfkill_type, l)) continue;
        snprintf(out, 256, "%s/%s/state", "/sys/class/rfkill", de->d_name);
        break;
    }
    closedir(d);
}

static void gps_power_on(const struct cfg *c)
{
    char path[256];
    resolve_nstdby_path(path, c->rfkillNStdby, c->gpioNStdbyPath);

    if (sysfs_read_byte(path) == 1) {           /* 0x19cae */
        sysfs_write_byte(path, '0');            /* ClearNstdbyPin -> setPin(1,0) */
        usleep(50000);                          /* 0x1d28 movw r0,#0xc350 */
    }
    sysfs_write_byte(path, '1');                /* slot 0x18 -> setPin(1,1) */

    usleep((useconds_t)c->gpioDelayMs * 1000);  /* 0x19cda..0x19ce4, after the write */

    if (c->LPmode) {                            /* 0x19bc4 */
        int fd = open("/sys/class/power_supply/battery/gps", O_RDWR);
        if (fd >= 0) { write(fd, "1\n", 6); close(fd); }
    }
}

static void gps_power_off(const struct cfg *c)
{
    char path[256];
    resolve_nstdby_path(path, c->rfkillNStdby, c->gpioNStdbyPath);
    (void)sysfs_read_byte(path);                /* logged before clearing */
    sysfs_write_byte(path, '0');
    if (c->LPmode) {                            /* 0x19d5c */
        int fd = open("/sys/class/power_supply/battery/gps", O_RDWR);
        if (fd >= 0) { write(fd, "0\n", 5); close(fd); }
    }
}
```

Ioctl backend equivalent (when `acPortName` is used instead of sysfs paths):

```c
int fd = open(acPortName, O_RDONLY);
unsigned arg = ((value & 1) << (ioctlNStdby + 8)) | (1u << ioctlNStdby);
ioctl(fd, 0x40024302, arg);                     /* set   */
int st = -1; ioctl(fd, 0x80014301, &st);        /* get   */
close(fd);                                      /* bit = (st >> ioctlNStdby) & 1 */
```

---

## 7. Consequences for j7elte bring-up

1. The stock HAL only exposes **nStdby**, and in the sysfs backend nStdby is the
   **rfkill `state` file of the device whose `type` == `rfkillNStdby`**
   (typically `gps`), not a `/sys/class/gpio` line. Our bridge must use the same
   node if it wants the same effect; `GPS_PWR_EN` (kernel `sec_gps` driver) is an
   *additional* rail that the stock HAL never touches — it is driven before the
   HAL ever runs.
2. After nStdby is asserted, the stock HAL waits `gpioDelayMs` **milliseconds**
   (0 if the key is absent) and only then opens the UART. Any host-side bring-up
   that opens `/dev/ttySAC0` earlier than that can still be correct, but the
   chip is guaranteed to be out of standby only after this delay.
3. The 50 ms low pulse only applies when the HAL already saw nStdby == 1
   (warm restart); a cold start drives nStdby high immediately.
4. LP-mode (`/sys/class/power_supply/battery/gps`) is written **after** the
   power delay; if `LPmode` is false the HAL still logs but writes nothing.

---

## 8. Evidence appendix (verbatim, capstone Thumb-2)

SetPin writes exactly one byte:
```
01a112  ldr   r5,[pc,#0x90]        01a118 add r5,pc      ; buf = 0x3da008
01a116  movs  r2,#0x31             01a11c strb r2,[r5]   ; '1'
01a11e  blx   #0x17b68                                   ; __strlen_chk(buf,3) == 1
01a128  blx   #0x17ab4                                   ; write(fd, buf, 1)
01a12c  cmp   r0,#0                01a12e ble  #0x1a17a  ; error -> "%s %d Error writting to GPIO %d: (%s)"
01a13c  add   r1,pc                -> 0x2f0958          ; "%s pin [%d] value[%s] successful"
01a14e  b.w   #0x2dd83c                                  ; close(fd)
```

Ioctl set — mask and request:
```
01a234  and   r4,r7,#1               ; value & 1
01a238  add.w r3,r6,#8               ; pin + 8
01a240  lsls  r4,r3                  ; (value&1) << (pin+8)
01a242  lsls  r5,r6                  ; 1 << pin
01a248  orrs  r4,r5
01a244  movw  r1,#0x4302
01a24a  movt  r1,#0x4002             ; 0x40024302
01a252  blx   #0x17b74               ; ioctl(fd, 0x40024302, arg)
01a262  add   r1,pc -> 0x2f09e8      ; "GPIO %d set to %d\n"
```

Ioctl get:
```
01a2e2  mov.w r3,#-1 ; 01a2e8 str r3,[sp,#0x14]
01a2f4  movw  r1,#0x4301
01a2fa  movt  r1,#0x8001             ; 0x80014301
01a2fe  blx   #0x17b74               ; ioctl(fd, 0x80014301, &state)
01a312  add   r1,pc -> 0x2f0aa0      ; "GPIO states %X (pin %d state %d)"
01a31e  asrs r2,r4 ; 01a322 and r0,r2,#1
```

Power-on delay maths:
```
019cd4  mov   r0,r6 ; 019cd6 bl #0x4370c   ; atoi(&gpioDelayMs)
019cda  rsb   r1,r0,r0,lsl #5              ; 31*d
019cde  add.w r0,r0,r1,lsl #2              ; 125*d
019ce2  lsls  r0,r0,#3                     ; 1000*d
019ce4  blx   #0x17ae4                     ; usleep(1000*d)
```

Log of the delay (immediately before the usleep):
```
019cbe  mov   r0,r6 ; 019cc2 bl #0x4370c
019cc6  ldr   r2,[pc,#0x7c]
019cca  add   r2,pc          -> 0x2f0748   ; "Power enable delay: %dms\n"
019ccc  mov   r3,r0
019cd0  bl    #0x19a18
```

Warm-restart pulse:
```
019d20  ldr r2,[r4] ; 019d24 ldr r6,[r2,#0x1c] ; 019d26 blx r6   ; nStdby := 0
019d28  movw r0,#0xc350                                          ; 50000
019d2c  blx #0x17ae4                                             ; usleep(50000)
019d30  b   0x19cb0
```

---

## 9. Delta to the in-tree daemon (`code/pmaports/device/archived/j7elte-gpsd`)

What the daemon already does (`src/j7elte-gpsd.c`): `power_set(1)` writes
`/sys/class/sec/gps/GPS_PWR_EN/value`, then `nanosleep(power_delay_ms)` (default
250 ms), then opens the UART at 921600. The **ordering matches** the stock HAL
(pin first, delay second) and the kernel `GPS_PWR_EN` rail is something the stock
HAL never touches, so that part is additive and correct.

Gaps versus the stock sequence documented above:

1. **No nStdby.** The stock HAL's only chip-side pin is nStdby, driven through
   `/sys/class/rfkill/<dev>/state` where `<dev>` is the entry whose `type` file
   equals `rfkillNStdby` (typically `gps`). Without it the chip can stay in
   standby even with the rail up — consistent with "rail on, UART silent".
   Add after `power_set(1)`: resolve the rfkill node, `write("1",1)`, then sleep.
2. **No warm-restart branch.** If the state file already reads `1`, the stock HAL
   writes `0`, waits 50 ms, then writes `1`.
3. **No LP-mode write.** `/sys/class/power_supply/battery/gps` = `"1\n"`
   (6 bytes) on power-up and `"0\n"` (5 bytes) on power-down, only when the
   `LPmode` config key is `true`.
4. **Delay value.** Stock uses `gpioDelayMs` from gps.xml (`usleep(ms*1000)`,
   0 when unset); the daemon hardcodes 250 ms. Either value is fine for the UART,
   but the *order* (nStdby write → delay → open UART) should be preserved when
   the nStdby step is added.
