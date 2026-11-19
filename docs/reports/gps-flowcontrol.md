# GPS transport: post-asic-ready stages, SW/HW flow control, frame templates, RF/clock plan

Target: `/tmp/stock_gpsd` (ELF32 ARM, stripped, PIE, BuildID `d4a2340c8e48ac0c4b805eba36c5ba5b`).
All addresses are **file == vaddr** (first PT_LOAD is identity-mapped, `0x0..0x3c3320`).
Method: capstone Thumb-2 disassembly, `.ARM.exidx` function map (`prel31`, 9562 functions),
PC-relative literal resolution (`ldr rX,[pc,#d]` … `add rX,pc`; the add base is `(addr+4)&~3`),
`.got` slot inspection for cross-object pointers, and the public hardware-verified capture
spec `mbicz/bcm4752-init:SPEC.md` for cross-checking wire behaviour.

Evidence tags: **[CODE]** = read directly from the binary; **[HW]** = reproduced on the
SM-J700M (see repo reports `gps-protocol.md`, `gps-baud-matrix.md`) or in SPEC.md;
**[INFER]** = reconstruction that the bytes support but that was not proven.

---

## 0. Executive summary — what the daemon does after `asic ready`

The BRI/GLME transport object (`GlMeSrdAsic`-side packet manager; fields listed in §9)
implements a strict **frame → reply → next-frame** sequence with a 200–300 ms reply timer
and a retry counter. The stages that matter for bring-up, in order:

1. **Autobaud train** — 16 × `0x80`, then the flow-control frame (§3, §4).
2. **Flow-control programming** — `fe00fd210001330e10fc` (HW flow control) or
   `fe00fd210002 p0 p1 p2 fc` (SW flow control). The chip echoes the frame; the daemon
   verifies the echo and retries on timeout (§4, §5).
3. **Register 0 read / HW version** — `ff00fd0000fc` → `fe00fd000005000230fc` (0x30020005).
4. **Item-0x1f read loop** — `ff00fd1f94fc` / `ff00fd1f95fc` → `fe00fd1fXX…fc`.
5. **Version block** — `fe00fd001b07000000fc`, `ff00fd001bfc`, `ff00fd401afc` (three frames,
   sent twice around an autobaud re-train).
6. **Legacy exit / GLME hand-off** — `fe00fd000055aa00fffc` → `e0…e0 01` GLME banner.
7. **RF / clock plan** — plan record with 12-byte band entries; TCXO 26 MHz → plan id 18,
   52 MHz → plan id 19; anything else is rejected.

**The single most useful actionable fact for our bring-up:** after the 16 × `0x80`
autobaud burst the stock daemon *always* sends a flow-control frame, and nothing else
happens until the chip echoes it. Our probe went silent exactly there.

---

## 1. Wire format (confirmed both ways)

```
host → chip : <soff> 00 fd <opcode…> [payload…] fc     soff ∈ {0xfe, 0xff}
chip → host : fe 00 fd <opcode…> [payload…] fc
```

* `0xfd` at offset 2 separates header from opcode. `0xfc` terminates. **[CODE]** — the
  receive path requires `buf[1..2] == 00 fd` and `buf[9] == 0xfc` (§8).
* `soff` (`0xfe`/`0xff`) meaning is **not** established in the vendor binary either: the
  daemon stores whole frames as byte templates, so the choice is data, not code (§2).
* Frame length in the receive path is **10 bytes** for all replies the daemon accepts
  without extra payload parsing; the daemon also has a 6-byte write form (`ff 00 fd … fc`)
  and long data frames (`fe NN fd 61 17 de c0 …`).
* Only chip→host frames start with `0xfe` *and* are validated as replies; host frames with
  `0xfe` are ordinary requests (e.g. the flow-control frame). **[CODE]**

---

## 2. The template / constant block `0x398afc..0x398b5c` — who sends what

The block is **not** an independent table: it is a set of fixed offsets inside one large
`const` aggregate whose base is **`0x398754`** (call it `bri_tbl`; the RF-plan builder in
`0x2a54d0` materialises the same aggregate as `0x398752`, i.e. two bytes earlier — its own
`+0x344`/`+0x350` offsets are relative to that). The base is materialised
as `ldr rX,[pc,#…]` + `add rX,pc` in every user; each frame is read as `ldr` (4 B) +
`ldrh` (2 B) or `ldr`+`ldr`+`ldrh` for 10-byte frames, then handed to the sender.

| `bri_tbl+` | addr | bytes | len | send site (function) | log/name evidence |
|---|---|---|---|---|---|
| `+0x3a8` | `0x398afc` | `ff 00 fd 1f 94 fc` | 6 | `0x2a5d58` (fn `0x2a5ce4`) | item-0x16 write + item-0x94 poll |
| `+0x3b0` | `0x398b04` | `ff 00 fd 1f 95 fc` | 6 | `0x2a613e` (fn `0x2a6104`) | asic-ready poll |
| `+0x3d8` | `0x398b2c` | `fe 00 fd 00 00 55 aa 00 ff fc` | 10 | `0x2a61d6` (fn `0x2a618c`) | `Bri_GetOutOfLegacy` |
| `+0x3e4` | `0x398b38` | `ff 00 fd 00 00 fc` | 6 | `0x2a67be` (fn `0x2a6784`) | register-0 / HW version |
| `+0x3ec` | `0x398b40` | `fe 00 fd 00 1b 07 00 00 00 fc` | 10 | `0x2a710a` (fn `0x2a70b8`) | version block, frame 1 |
| `+0x3f8` | `0x398b4c` | `ff 00 fd 00 1b fc` | 6 | `0x2a7126` (fn `0x2a70b8`) | version block, frame 2 |
| `+0x3fc` | `0x398b54` | `ff 00 fd 40 1a fc` | 6 | `0x2a7140` (fn `0x2a70b8`) | version block, frame 3 |

Disassembly of the three-frame sender `0x2a70b8` (evidence for the order, **[CODE]**):

```
0x2a70fa  ldr r4,[pc] ; add r4,pc           ; r4 = 0x398b54 = bri_tbl+0x3fc
0x2a7106  add.w r4,r4,#0x400                ; r4 = 0x398b54  (anchor for negative offsets)
0x2a710a  ldr r0,[r4,#-0x14] ; ldr r1,[r4,#-0x10] ; ldrh ip,[r4,#-0xc] ; stm/stmh -> sp+0x10
0x2a7122  bl 0x2a5cd0                       ; send(fe00fd001b07000000fc, 10)
0x2a7126  ldr r0,[r4,#-0x8] ; ldrh ip,[r4,#-0x4] ; str/strh -> sp
0x2a713c  bl 0x2a5cd0                       ; send(ff00fd001bfc, 6)
0x2a7140  ldr r0,[r4]       ; ldrh r6,[r4,#4]    ; str/strh -> sp+8
0x2a7152  bl 0x2a5cd0                       ; send(ff00fd401afc, 6)
```

`0x398b5c` immediately after the last frame is `0xff ff ff ff` — the terminator of the
aggregate; `0x398b60+` is the double-precision constant pool of an unrelated math routine
(reached from `.got` slot `0x3d9a4c`), which is why no code pointer to `0x398afc…0x398b54`
exists anywhere in the image: they are always reached as `base+offset`.

Frames **not** in the table are built on the stack (byte immediates): HW flow control
(`0x2a6cfc`), SW flow control (`0x2a6aa0`), autobaud `0x2a5ae4`, and the patch-data
headers in `0x2a71b0`.

---

## 3. Sender / transport plumbing

```
0x2a5cd0  SendFrame(this, buf, len)            ;  → 0x2a599c
0x2a599c  SendRaw(this, buf, len, port)
            this->[this+0x4c]+4 must == port (SoftAssert line 0xe86)
            if (sw_flowcontrol_enabled)  → escape buf into a 0x140-byte stack buffer
            if (this->[0x121])           → packet trace hook (0x1dbf90)
            this->[0x124] += len ; transport->vtable[8](buf, len)   ; tx byte counter
0x2a5cd0 also does this->[0x128]++             ; tx frame counter
```
Transport object at `this+0x4c`; `[+4]` is the port index (`1` in every path we saw, and
`1` is what the captured HW-flow-control frame carries).

Useful transport accessors (**[CODE]**):

| addr | function |
|---|---|
| `0x1c957c` | `type == 2` (i.e. "software-flow-control-capable transport") |
| `0x1c956c` | `type == 1` |
| `0x1c9528` | `transport[0x18]` — configured clock/TCXO frequency in Hz |
| `0x1c958c` | `*xon = [0x98]; *xoff = [0x99]; return type==2` |
| `0x1c9560` | set `[0x98]=xon, [0x99]=xoff`; **defaults 0x11/0x13** when type != 2 (`0x1c9554`) |
| `0x1c9474` | `GetAsicType()` from the HW-version register (`0x30020005`→`0x30100005`, `0x34000007`, `0x34020007`, …) |

---

## 4. Escape-byte machinery (only when `this->[0x10e] != 0`)

`this->[0x10e]` is `transport->type == 2` (set at `0x2a5c0a`). On a plain UART it is 0 and
**no escaping is applied in either direction** — matching the public capture.

### 4.1 TX escaping — `0x2a599c`, loop at `0x2a59fc`

```
for each byte c in buf[0..len):
    if      (c == 0xe0)         { out[n++]=0xe0; out[n++]=0x02; }
    else if (c == this->[0x10c]){ out[n++]=0xe0; out[n++]=0x03; }   /* xon  */
    else if (c == this->[0x10d]){ out[n++]=0xe0; out[n++]=0x04; }   /* xoff */
    else                        { out[n++] = c; }
    if (n >= 0x140) SoftAssert(line 0xea1)          /* buffer limit, keeps going */
```
Order of the tests is fixed: `0xe0` first, then xon, then xoff (`0x2a5a00`, `0x2a5a08`,
`0x2a5a0c`).

### 4.2 RX un-escaping — `0x2a7bc8`, dispatch at `0x2a7c46`

```
if (this->[0x10e]) {
    if (this->[0x120]) {                    /* previous byte was the escape char */
        this->[0x120] = 0;
        switch (c) { 2: c = 0xe0; 3: c = this->[0x10c]; 4: c = this->[0x10d];
                     default: SoftAssert("received unknown escaped character", line 0xeee);
                              drop byte; }
    } else if (c == 0xe0) { this->[0x120] = 1; continue; }   /* consume escape char */
}
```
The public capture's `e0 02 = 0xe0, e0 03 = 0x00, e0 04 = 0x01` is the *same* table with
xon/xoff configured as `0x00/0x01`; the escape targets are exactly what the
`0x21` flow-control command programs (§5).

`0x2a5ae4` = `Bri_SendAutobaud()`: `memset(buf,0x80,16)` then `SendRaw(buf,16,port)`.

---

## 5. Flow control programming

### 5.1 HW variant — `0x2a6cfc` (`Bri_ProgramHwFlowControl`)

```
if (this->[0x10e]) ...                      ; debug-gated SoftAssert only
if (transport[+4] != 1) SoftAssert(line 0xe06)
buf = { fe,00,fd,21,00, <transport[+4]>, 0x33, 0x0e, 0x10, fc }   ; 10 bytes
SendFrame(this, buf, 10)
```
Byte evidence (`0x2a6d48`–`0x2a6d80`): `0xfe`→`sp+0x18`, `0xfd`→`+0x1a`, `0x21`→`+0x1b`,
`[transport+4]`→`+0x1d`, `0x33`→`+0x1e`, `0x0e`→`+0x1f`, `0x10`→`+0x20`, `0xfc`→`+0x21`;
`sp+0x19`/`+0x1c` are zeroed by a preceding `str` pair.

Wire result (equals the captured frame **[HW]**, SPEC.md step 1):
`fe 00 fd 21 00 01 33 0e 10 fc`.

> Note: the **host** sends the same 10 bytes that the chip echoes. The trace string is
> `GlMeSrdAsicinit::Bri_ProgramHwFlowControl(0x%02X, 0x%02X, 0x%02X, 0x%02X)` (0x34cc44).

### 5.2 SW variant — `0x2a6aa0` (`Bri_ProgramSwFlowControl`), 0x25c bytes

Entry conditions and asserts:

```
A = this->[0x10c] (xon) ; B = this->[0x10d] (xoff) ; F = this->[0x10e]
SoftAssert(line 0xe0f) if (F == 0 || A == B)          ; 0x2a6abe / 0x2a6acc
SoftAssert(line 0xe10) if (A == 0x21 || A == 0xe0 || (u8)(A-5) > 0xf6)   ; 0x2a6ad2..0x2a6aea
SoftAssert(line 0xe11) if (B == 0x21 || B == 0xe0 || (u8)(B-5) > 0xf6)   ; 0x2a6b00..0x2a6b1e
```
i.e. both characters must lie in `0x05..0xfb` and must not be `0x21` or `0xe0`
(`0x21` is the BRI command byte, `0xe0` is the escape byte). Both asserts are
*log-only* (SoftAssert, `0xfc3b0`) — they do not abort.

**Candidate search** (`0x2a6b38`..`0x2a6c20`), 20 iterations, two counters packed into `r4`
(low nibble = `na`, high nibble = `nb`):

```
na = 0; nb = 0; r4 = 0; r5 = A; r6 = B;   count = 20
loop:
  if (r4==A || r4==B || r4==0xe0 || r5==A || r5==B || r5==0xe0)   /* 0x2a6b44..0x2a6b7e */
        { na++; r5 = A - na; r4 = (nb<<4)|na; }                   /* 0x2a6c04 */
  else if (r6==A || r6==B || r6==0xe0)                            /* 0x2a6b80..0x2a6b9e */
        { nb++; r6 = B - nb; r4 = (nb<<4)|na; }                   /* 0x2a6c5e */
  else goto accept;
  if (--count) loop;

accept:                                        /* 0x2a6ba0 */
  SoftAssert(line 0xe2c) if (A != (r4 & 0xf) + r5)
  SoftAssert(line 0xe2c) if (B != r6 + (r4 >> 4))
```
The two assertions are loop invariants (`r5 = A - (r4&0xf)`, `r6 = B - (r4>>4)`); they are
compiler-emitted self-checks and never fire. The functional output is:

```
p0 = r4 = (nb << 4) | na          /* nibble-packed adjustment index */
p1 = A - na                       /* xon  delta */
p2 = B - nb                       /* xoff delta */
```
i.e. the frame carries a *lossy 3-byte encoding* of the pair, chosen so that none of
`p0`,`p1`,`p2` collides with `0xe0`, `xon` or `xoff` (the three bytes the chip must be able
to distinguish on the wire).

Worked example for the UART defaults `xon=0x11`, `xoff=0x13` (`0x1c955e`): iteration 1 →
`na=1`, iteration 2 → `nb=1`, iteration 3 (`r4==0x11==A`) → `na=2`, iteration 4 accepts:
`p0=0x12, p1=0x0f, p2=0x12`. Predicted frame:

```
fe 00 fd 21 00 02 12 0f 12 fc
```

Send path (`0x2a6c8c`..`0x2a6cca`): frame built at `sp+0x10` =
`{fe,00,fd,21,00,02,p0,p1,p2,fc}`, `SendFrame(this, sp+0x10, 10)`, but **only** if
`transport[+4] == 1` (`0x2a6bd6`); otherwise `SoftAssert(line 0xe3d)` and no frame is sent.
`this->[0x10e]` is cleared immediately before the send (`0x2a6bd0`) and restored to 1 after
(`0x2a6bf6`).

### 5.3 Reply verification — handler `0x2a6dd8` (`EswLoadSetAsicData`, 0x2e0 bytes)

The handler is registered in the packet-manager operation list and is called with a 16-byte
message: `msg[0]` = event class, `msg[1]` = sub-type. `msg = {0x0b, 0x0d, …}` means
"10-byte reply arrived, opcode group 0x21" (produced by the parser, §7).

```
msg[0] == 0x0b and msg[1] == 0x0d:                       ; 0x2a6e06
  if (transport->type == 2) {                            ; 0x2a6e18 / 0x2a6e22
      if (this->[0x13c] != 0x02)  -> "wrong reply" path
      if (this->[0x10e] == 0)     -> "wrong reply" path  ; 0x2a6ef4
      p0 = this->[0x13d]; p1 = this->[0x13e]; p2 = this->[0x13f];
      ok = (xon == (p0 & 0xf) + p1) && (xoff == p2 + (p0 >> 4));
  } else {                                               ; 0x2a6ee2
      ok = (*(u32*)&this->[0x13c] == 0x100E3301);        ; i.e. echo 01 33 0e 10
  }
  if (!ok) -> "wrong reply" path
  if (ok)  { timer_stop(this+0xf4); log "%s Flow Control Set"; return 0; }
```
`0x100E3301` is literally the HW-frame echo `01 33 0e 10` read little-endian — the
`movw ip,#0x3301 / movt ip,#0x100e` pair at `0x2a6ee6`. The SW echo check reconstructs
xon/xoff from the 3 payload bytes with the same formula the sender used.

**Start / retry / timeout:**

| site | action |
|---|---|
| `0x2a6e42` (msg class 0x02) | `if (type==2) SwFlowControl else HwFlowControl`; `Timer::Start(this+0xf4, ms=0x12c=300, id=6, retries=0)` |
| `0x2a6f36` (msg class 0x06, `msg[1] <= 1`) | `Bri_SendAutobaud()` → `Sw/HwFlowControl()` → `Timer::Start(ms=0xc8=200, id=6, retries=(u8)(msg[1]+1))` |
| `0x2a6e6c` (msg class 0x05) | `this->[0x11f]++`; first expiry → *"…Flow Control Was not Acked properly. Retrying"*; later → *"…Timer already expired."*; exhausted → *"No correct BRI response after retries. Maybe chipset is in ESW mode. Let's try to set it back to BRI"* (0x34cd34 / 0x34cd78 / 0x34cdcc) |

Timer API: `0x285544(timer, this, id, retries, ms)`; stop: `0x284ba8`. **[INFER]** on the
exact argument order — the call sites are `r0=this+0xf4, r1=this, r2=6, r3=retries, sp[0]=ms`.

### 5.4 Dispatcher table (`0x2a6dd8`) — dumped

`tbb [pc,r0]` at `0x2a6df8`, `r0 = msg[0]-2`, table bytes at `0x2a6dfc`:

| `msg[0]` | table byte | target | behaviour |
|---|---|---|---|
| 2 | `0x23` | `0x2a6e42` | start: `Sw/HwFlowControl()`, timer 300 ms, id 6, 0 retries |
| 3 | `0x5a` | `0x2a6eb0` | `Timer::Cancel(this+0xf4)` (`0x2855f8`), return 0 |
| 4 | `0x61` | `0x2a6ebe` | return the global behind `.got` slot `sb-0xf4` (status/error code) |
| 5 | `0x38` | `0x2a6e6c` | FC timer expiry: `this->[0x11f]++`, retry or give-up logs |
| 6 | `0x4a` | `0x2a6e90` | if `msg[1] <= 1` → `0x2a6f36` (re-autobaud + reprogram, 200 ms, `msg[1]+1` retries) else timer 300 ms id 5 |
| 7,8,9,10 | `0x61` | `0x2a6ebe` | same as class 4 |
| 11 (`0x0b`) | `0x05` | `0x2a6e06` | reply path: `msg[1]` must be `0x0d` (opcode `0x21`), then §5.3 checks |

Table-to-target arithmetic: `target = 0x2a6dfc + 2*byte` (validated against every landing
site above; a plain `+byte` produces odd addresses, which cannot be Thumb instruction
starts). **[CODE + INFER on the scale factor]**

---

## 6. Stage sequence and frames (state machine)

The init driver `0x2a2070` (0x3460 bytes, `INIT_STAGE A/B/C` strings at `0x34c9fc`,
`0x34d06c`, `0x34d0e0`) drives these stage functions; each stage is a *reply handler*
(called with the parsed message) plus a *send* helper. Stage names are attached to a trace
helper at `this+8` via `0x2851f4` and appear in `.rodata`:

`bri_wait4bri` `0x34c42c`, `bri_wait4eswload` `0x34c468`, `bri_wait4patchload` `0x34c4ac`,
`bri_wait4eswbankswitch` `0x34c800`, `bri_wait4patchbankswitch` `0x34c84c`,
`bri_wait4asic_ready` `0x34c8f0`, `bri_wait4version` `0x34cb34`,
`bri_wait4flowcontrol` `0x34cc98`, `bri_wait4eswbankloaded` `0x34ceb0`,
`bri_wait4patchblockload` `0x34cec8`, `esw_wait4initasic` `0x34d058`,
`esw_wait4version` `0x34c9e8`.

| # | state | send (host) | expected reply | decision |
|---|---|---|---|---|
| 0 | autobaud + FC | `80`×16 then `fe00fd210001330e10fc` | identical 10-byte echo | timeout 300 ms, 0 retries; on expiry re-autobaud + resend with 200 ms and `msg[1]+1` retries (§5.3) |
| 1 | HW version | `ff00fd0000fc` (`0x2a6784`, template `+0x3e4`) | `fe00fd0000 <4-byte reg0> fc`; on this device `fe00fd000005000230fc` = `0x30020005` **[HW]** | value → `GetAsicType()` (`0x1c9474`); log *"Got BRI HW version(0x%lx)"* `0x2a6a30` |
| 2 | item 0x16 write → item 0x94/0x95 poll | `fe00fd6116<v>000000fc` then `ff00fd1f94fc` (`0x2a5d58`, `+0x3a8`); `ff00fd1f95fc` (`0x2a613e`, `+0x3b0`) | `fe00fd1fXX 00 00 00 00 fc`; `0x1f95` observed as "asic ready" **[HW]** | `buf[4]` must match the item; `0x94` gates patch loading, `0x95` gates `bri_wait4asic_ready` |
| 3 | legacy exit | `fe00fd000055aa00fffc` (`0x2a618c`, `+0x3d8`) | GLME banner `e0 00 … e0 01` **[HW]** | *"Debbugger detected. Exiting Legacy!"* / *"Debugger not detected"* (`0x34c950`, `0x34c928`) |
| 4 | version block | `fe00fd001b07000000fc`, `ff00fd001bfc`, `ff00fd401afc` (`0x2a70b8`, `+0x3ec/+0x3f8/+0x3fc`), wrapped by `0x2a7198` = `send3(); Bri_SendAutobaud(); send3();` | `fe00fd001b 07 00 00 00 fc` observed when `ff00fd001bfc` was sent **[HW]**; `0x401a` reply unknown | *"INIT_STAGE A: Requesting the Asic Version Number trial %u"*, *"DONE. Version received: Asic 0x%08lX, IFC %lu, Radio 0x%02X"* `0x2a6740` |
| 5 | RF/clock plan | plan record (§8) | — | *"INIT_STAGE B: Programming RF type and switch to high speed clock! If no ACK comes back, check your frequency plan!"* |
| 6 | heartbeat etc. | — | — | *"INIT_STAGE C: Starting Hearbeat, RTC capture, NvRam Read, …"* `0x2a83f0` |

Message/event classes seen in the dispatchers: `0x0b` = "reply parsed" (sub-type `0x0b`
= opcode `0x1f`, `0x0c` = opcode `0x2f15`, `0x0d` = opcode `0x21`), `0x02`/`0x05` = per-stage
timer expiries. **[INFER]** on the timer-id ↔ class mapping; the reply mapping is **[CODE]**.

---

## 7. Reply parser — `Bri_CheckForReceivedPacket_Sardine` `0x2a789c` (0x32c bytes)

Called per byte from the RX path `0x2a7bc8` after un-escaping and append
(`0x2a7c9e`, only when `transport[+4] == 1`).

Assembly buffer: `this+0x10f` .. `this+0x11d`, count `this+0x11e` (≤ 0x0e before parsing).
`this+0x134` counts bytes dropped as garbage.

```
n = this->[0x11e]
if (n == 10) goto parse;
if (n >  0x0e) goto resync;          /* 0x2a78b4..: scan for a fresh 0xfe, drop dups */
return;
```
`resync` (`0x2a78c0`..`0x2a7a56`): keeps `buf[0]`, and for each subsequent byte equal to
`0xfe` compacts the buffer so the duplicate is removed; a stray `0xfe` therefore restarts
the 10-byte window. **[INFER on the exact compaction; the instruction sequence is a
chain of 14 copies at `0x2a7948`…`0x2a7a46`]**

`parse` (`0x2a7a5a`):

```
require buf[0]==0xfe, buf[1..2]==00 fd (ldrh == 0xfd00), buf[9]==0xfc
cmd = buf[3], sub = buf[4]
switch (cmd, sub):
  (0x2f,0x15): this->[0x13c..0x13e] = buf[5..7] (24-bit), this->[0x140] = buf[8]
               msg = {0x0b, 0x0c, 0x0b, 0x0c}   -> notify           /* 0x2a7ad4 */
  (0x00,   *): this->[0x13c..0x13f] = buf[5..8] (32-bit)
               msg = {0x0b, 0x0b}                -> notify          /* 0x2a7b60 */
  (0x1f,   *): this->[0x13c] = buf[4], this->[0x13d] = buf[5]
               msg = {0x0b, 0x0b}                -> notify          /* 0x2a7b96 */
  (0x21,   0): this->[0x13c..0x13f] = buf[5..8]
               msg = {0x0b, 0x0d}                -> notify          /* 0x2a7b2c */
  default:     log "Bri_CheckForReceivedPacket_Sardine unknown command(%u, %u)!"
               this->[0x134]++                                       /* 0x2a7b20 */
count = 0 (this->[0x11e])
```

`notify` = `0x2a1ef0(this, msg)` which walks the registered-operation list
(`this+0x18` / `this+0x1c`) until a handler returns 0.

Observed cross-checks **[HW]**: `fe00fd000005000230fc` → cmd `0x00`, payload
`0x30020005`; `fe00fd1f9500000000fc` → cmd `0x1f`, `buf[4]=0x95`; the flow-control echo
`fe00fd210002…fc` → cmd `0x21`, `buf[5]=0x02`.

---

## 8. RF / clock plan programming

Two functions:

* **`0x2a54d0`** (0x448 bytes) — builds the CW (continuous-wave/test) plan; logs
  `ProgramCwRfFrequencyPlan() - Unsupported TCXO frequency …` (`0x2a5820`, `0x2a57da`).
* **`0x2a2070`** (0x3460 bytes, the init driver) — the production plan, inlined; logs
  `GlMeSrdAsicinit::ProgramRfFrequencyPlan() - Unsupported TCXO frequency! (%lu)` at
  `0x2a5498` and `Frequency plan (%d) not implemented for 4753 A0` (`0x34c5fc`).

Plan construction (evidence `0x2a5616`..`0x2a579c`):

* The record lives on the stack (`sp+0x44` … `sp+0x1c4`), copied from `bri_tbl`
  (`sl+0x344..0x34e`, i.e. `0x398a96..0x398aa0`) for the fixed part, then filled with
  **12-byte band entries**:
  `entry[0]=band/mode`, `entry[4]=freq-word`, `entry[8]=freq-word2`, `entry[+5]=flag`;
  `for (p = base; p != end; p += 12)` at `0x2a578e`.
* Clock comes from `transport->[0x18]` (`0x1c9528`), and is converted to MHz with a
  `vcvt.f64.u32` / `vdiv` by **`1000000.0`** (double at `0x2a58f0`) then to integer
  (`0x1a0fd8`); the result is stored into the plan (`strb r0,[r2]`, `0x2a585e`).

Clock switch (`0x2a5518`, `0x2a57a8`):

| TCXO | compare constant | plan id | path |
|---|---|---|---|
| 26 MHz | `0x18CBA80` | `0x12` (18) | `0x2a58be` → common builder |
| 52 MHz | `0x3197500` | `0x13` (19) | `0x2a5830` → common builder |
| other | — | — | `SoftAssert`-gated log: *"Frequency plan (%d) not implemented for 4753 A0"* / *"Unsupported TCXO frequency"* and return without programming |

`RfType` is a run-time configuration value; its name table is an 8-byte pointer array at
`0x3c4ab0` (`.data.rel.ro.local`), names in `.rodata` at `0x31d920…`:

```
idx 0  GL_RF_UNKNOWN            0x31d920
idx 5  GL_RF_2076_BRCM          ...
idx 26 GL_RF_4752_BRCM          0x31dabc
idx 31 GL_RF_47520_BRCM_EXT_LNA 0x31dbb0   <-- the RfType requested for this platform
```
so **`GL_RF_47520_BRCM_EXT_LNA == 31`**. The related frequency-plan name table starts at
`0x3c50f8` (`FRQ_PLAN_25_31MHZ_2PPM`, `FRQ_PLAN_26MHZ_2PPM*` at `0x31d3e9`, …). The plan
record is handed to the chip through the generic item/register write path — the exact
opcode is **not** determined (§10).

---

## 9. Transport object field map (offsets used by this slice)

```
+0x018  u32  clock/TCXO frequency in Hz          (0x1c9528)
+0x01c  u16  (clock secondary)
+0x044  ptr  transport vtable holder (vtable[8] = write(buf,len))
+0x048  ptr  packet-manager vtable holder  (vtable[8] = SetSwFlowControlXonXoff(xon,xoff))
+0x04c  ptr  transport object:
             +0x004 u32 port index (1)
             +0x094 u32 type  (1 = UART/HW-FC, 2 = SW-FC)
             +0x098 u8  xon  (default 0x11 when type != 2)
             +0x099 u8  xoff (default 0x13 when type != 2)
+0x0f4       reply timer
+0x104  u8   flag consulted on unmatched replies
+0x10c  u8   xon  (copied from transport[0x98])
+0x10d  u8   xoff (copied from transport[0x99])
+0x10e  u8   SW-flow-control/escaping enabled  (== transport[0x94]==2)
+0x10f..0x11d  RX assembly buffer (15 bytes)
+0x11e  u8   RX count
+0x11f  u8   flow-control retry/expiry counter
+0x120  u8   RX escape pending
+0x121  u8   TX packet tracing enabled (default 1)
+0x124  u32  TX byte counter      +0x128 u32 TX frame counter
+0x12c  u32  RX byte counter      +0x134 u32 dropped-byte counter
+0x13c..0x13f  last parsed payload (u32)
+0x140  u8   last parsed payload byte 4
```
Reset function `0x2a1f38` zeroes `0xe0`, `0xe1`, `0x10c`, `0x10d`, `0x10e`, `0x11e`, `0x11f`,
`0x120` and sets `0x121 = 0x122 = 1`.

---

## 10. Unknowns and the experiment that resolves each

| # | Unknown | Experiment |
|---|---|---|
| 1 | Meaning of `soff` (`0xfe` vs `0xff`) in host frames. | Send the same request with both prefixes and diff the replies: `ff00fd1f95fc` vs `fe00fd1f95fc`. Already partly done: `ff` got a reply, `fe…1b07000000fc` got none; repeat with `1f95` to separate "prefix" from "opcode/payload". |
| 2 | Whether the device's transport is type 1 (HW FC) or type 2 (SW FC). | Send `fe00fd210001330e10fc`; if the chip echoes it 1:1 (SPEC.md step 1) the transport is HW-FC and no escaping is in use. Then send `fe00fd210002120f12fc` and see whether it echoes; an echo proves the SW form and the `p0/p1/p2` encoding. |
| 3 | Actual `xon`/`xoff` on this platform (defaults 0x11/0x13 only when `transport type != 2`). | Echo test in (2); or dump `transport[0x98/0x99]` from the stock daemon (needs a debugger/`/proc/pid/mem` on the stock ROM) or from the config file it reads. |
| 4 | The 20-iteration search: is the accept condition ever reached early, i.e. are the SoftAsserts at lines `0xe2c` reachable? | Feed the algorithm with values that make all 20 candidates collide (e.g. `xon=0x05`, `xoff=0x06`) and observe whether the daemon sends a frame at all or logs. Static reading says the invariants always hold, so the asserts are dead. |
| 5 | Exact retry/timeout behaviour when the chip never answers the FC frame (log wording per attempt, whether the "set it back to BRI" path is reachable). | Power the chip, send 16×`0x80`, then send `fe00fd210001330e10fc` and withhold the echo (e.g. loopback off); capture the daemon log. |
| 6 | The opcode used to *write* the RF/clock plan (the plan record is built but the writer is a virtual call). | Set a breakpoint on the plan-record send (the vtable call at `0x2a588e`) under the stock daemon, or diff UART captures between two devices with different TCXO/RfType. This is the biggest remaining gap for a full parity implementation. |
| 7 | Meaning of opcodes `0x001b` (payload 7) and `0x401a` in the version block, and of the `fe`-prefixed copy of `001b`. | Send each of the three frames alone and log the replies; the `0x401a` reply is not in any capture yet. |
| 8 | Whether the RX "drop duplicate `0xfe`" compaction at `0x2a78c0` also resynchronises mid-frame (i.e. `fe` inside payload). | Send a 10-byte frame containing an extra `0xfe` at offset 5 and observe whether the daemon parses it (needs the daemon, not our probe: use the chroot test rig). |
| 9 | `0x2f15` patch-ack payload semantics (the 24-bit value the chip returns). | Known from SPEC.md §4 to be chip-generated and unverified; capture more ack values against known patch pieces to attempt a CRC fit. |
| 10 | Whether `this->[0x104]` and `this[0x11f]` interact with the `bri_wait4*` operation list (i.e. which stage's timer id is 6). | Set a watchpoint on `this+0xf4`'s timer id argument (`r2=6` at `0x2a6e5e`/`0x2a6f5a`) in the stock daemon. |

---

## 11. Reference C implementation of the flow-control stage

```c
/* offsets per §9; tri-state returns: 0 = ok, <0 = retry, >0 = fatal */
static const u8 SWFC_HDR[6] = { 0xfe,0x00,0xfd,0x21,0x00,0x02 };
static const u8 HWFC_FRAME[10]= { 0xfe,0x00,0xfd,0x21,0x00,0x01,0x33,0x0e,0x10,0xfc };

int bri_program_flow_control(struct bri *b)
{
    u8 frame[10];
    if (b->trans_type == 2) {                    /* 0x1c957c() != 0 */
        u8 A = b->xon, B = b->xoff;              /* 0x10c / 0x10d */
        unsigned na = 0, nb = 0, r4 = 0, r5 = A, r6 = B, cnt = 20;
        if (!b->swfc_enabled || A == B) return -1;
        if (A == 0x21 || A == 0xe0 || (u8)(A - 5) > 0xf6) return -1;
        if (B == 0x21 || B == 0xe0 || (u8)(B - 5) > 0xf6) return -1;
        do {
            if (r4 == A || r4 == B || r4 == 0xe0 ||
                r5 == A || r5 == B || r5 == 0xe0) { na++; r5 = A - na; r4 = (nb << 4) | na; }
            else if (r6 == A || r6 == B || r6 == 0xe0) { nb++; r6 = B - nb; r4 = (nb << 4) | na; }
            else break;
        } while (--cnt);
        memcpy(frame, SWFC_HDR, 6);
        frame[6] = r4; frame[7] = r5; frame[8] = r6; frame[9] = 0xfc;
    } else {
        if (b->trans_port != 1) return -1;       /* 0x2a6bd6 */
        memcpy(frame, HWFC_FRAME, 10);
    }
    if (b->trans_port != 1) return -1;
    b->swfc_enabled = 0;                          /* 0x2a6bd0 */
    bri_send_frame(b, frame, 10);                 /* 0x2a5cd0 */
    b->swfc_enabled = 1;                          /* 0x2a6bf6 */
    return 0;
}

/* reply check, handler 0x2a6dd8, msg = {0x0b, 0x0d, ...} */
int bri_flow_control_ack(struct bri *b)
{
    if (b->trans_type != 2)
        return (b->last_payload_u32 == 0x100E3301) ? 0 : -1;
    if (b->last_payload[0] != 0x02 || !b->swfc_enabled) return -1;
    {   u8 p0 = b->last_payload[1], p1 = b->last_payload[2], p2 = b->last_payload[3];
        return ((u8)((p0 & 0x0f) + p1) == b->xon &&
                (u8)(p2 + (p0 >> 4))  == b->xoff) ? 0 : -1; }
}
```

---

## 12. Bring-up checklist derived from this slice

1. Reset rails, hold `GPS_PWR_EN`, open `/dev/ttySAC0` 921600 8N1 **no** flow control.
2. Send 16 × `0x80` (autobaud) — chip does not answer, by design.
3. **Send `fe00fd210001330e10fc`** and expect the identical 10-byte echo within ~300 ms.
   Without this step the chip stays silent — this is the step our probe was missing.
4. Send `ff00fd0000fc`, expect `fe00fd000005000230fc` (`0x30020005`).
5. Send `ff00fd1f94fc` / `ff00fd1f95fc`, expect `fe00fd1fXX00000000fc`.
6. Then `ff00fd001bfc` → `fe00fd001b07000000fc`, and the `+0x3ec/+0x3f8/+0x3fc` triple.
7. Only after the version block does the RF/clock plan get programmed; 26 MHz is the
   supported clock and plan id 18.
