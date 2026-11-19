# GPS userspace: constant tables in stock `gpsd` (.rodata / .data.rel.ro / .data)

Slice: `gps-tables`. Target: `/tmp/stock_gpsd` (stock Android `gpsd`, ELF32 ARM EABI5,
stripped, Android 23). Goal: find structured constant tables that describe the
host<->BCM4752 command protocol or the driver configuration, validate them by
nearby strings and by code xrefs, and dump the best candidates.

## Method / how to read the addresses

* Section map (`readelf -SW`): `.rodata` vaddr=file off 0x2f0570..0x3bff81,
  `.data.rel.ro.local` 0x3c4f68, `.data.rel.ro` 0x3c6e80, `.data` 0x3da000.
  **In the RW segment vaddr is 0x1000 above the file offset**; in `.text`/`.rodata`
  they are equal. All dumps below are at vaddr, i.e. runtime addresses for a
  base-0 mapping.
* Pointer tables were recovered from the 9402 `R_ARM_RELATIVE` entries of
  `.rel.dyn` (addend in place), then filtered to runs of >=3 consecutive words.
* Code xrefs were taken from a full disassembly (`llvm-objdump`, which switches
  ARM/Thumb per `.ARM.exidx` function entry) by resolving every literal-pool load.
  Two address idioms matter here:
  1. `ldr rX,[pc,#k]` -> pool word is the absolute address (used for most data),
  2. `ldr rX,[pc,#k]; add rX,pc` -> the pool word is `target - PC` (PIE-local data),
  3. `ldr rX,[pc,#k]; add rX,pc; add.w rY,rX,#off` -> table = base+off.
  The second idiom is why naive `u32 == address` scans find almost nothing:
  e.g. `0x2a622c` holds `0x000f257c`, and `0xf257c + 0x2a61d8 = 0x398754`.

## T1. BRI (Broadcom Radio Interface) UART request-frame templates  - confidence HIGH

This is the only table in the image that directly encodes the host->chip wire
protocol. It is a packed array of complete request frames, reached as
`0x398754 + off` from `GlMeSrdAsicinit::Bri_*` (`.rodata`). All entries are
little-endian frames: `FF 00 FD <cmd> <sub> FC` (6 B) or
`FE 00 FD <cmd> <sub> <d0..d3> FC` (10 B); 6-byte slots are padded to 8,
10-byte slots to 12.

      addr     base+off  bytes                                   frame
      398afc   +0x3a8    ff 00 fd 1f 94 fc                       cmd 1F sub 94
      398b04   +0x3b0    ff 00 fd 1f 95 fc                       cmd 1F sub 95 (RequestDebugSfr)
      398b0c   +0x3b8    fe 00 fd 00 4d 80 00 00 00 fc            cmd 00 sub 4D data=00000080
      398b18   +0x3c4    ff 00 fd 00 4d fc                       cmd 00 sub 4D
      398b20   +0x3cc    fe 00 fd 00 4d 40 00 00 00 fc            cmd 00 sub 4D data=00000040
      398b2c   +0x3d8    fe 00 fd 00 00 55 aa 00 ff fc            GetOutOfLegacy
      398b38   +0x3e4    ff 00 fd 00 00 fc                       RequestHwVersion
      398b40   +0x3ec    fe 00 fd 00 1b 07 00 00 00 fc            cmd 1B data=00000007
      398b4c   +0x3f8    ff 00 fd 00 1b fc                       cmd 1B
      398b54   +0x400    ff 00 fd 40 1a fc                       cmd 40 sub 1A

Hex dump 0x398afc (64 B):
        00398afc  ff 00 fd 1f 94 fc 00 00 ff 00 fd 1f 95 fc 00 00  ................
        00398b0c  fe 00 fd 00 4d 80 00 00 00 fc 00 00 ff 00 fd 00  ....M...........
        00398b1c  4d fc 00 00 fe 00 fd 00 4d 40 00 00 00 fc 00 00  M.......M@......
        00398b2c  fe 00 fd 00 00 55 aa 00 ff fc 00 00 ff 00 fd 00  .....U..........

Hex dump 0x398b3c (56 B, tail of the block):
        00398b3c  00 fc 00 00 fe 00 fd 00 1b 07 00 00 00 fc 00 00  ................
        00398b4c  ff 00 fd 00 1b fc 00 00 ff 00 fd 40 1a fc 00 00  ...........@....
        00398b5c  ff ff ff ff a0 34 1f bb 50 9a 8f 5d 08 cd c7 ae  .....4..P..]....
        00398b6c  84 e6 63 57 42 f3 b1 2b c1 89 7a cb 03 00 00 00  ..cWB..+..z.....

Code xrefs (each resolved and checked instruction-by-instruction):

| code addr | instruction | resolved target | consumer |
|---|---|---|---|
| 0x2a6134 | `ldr.w r12,[pc,#0x4c]` + `add r12,pc` @0x2a613c | 0x398754 | `Bri_RequestDebugSfr` |
| 0x2a613e | `add.w r6,r12,#0x3b0`; `ldr.w r0,[r12,#0x3b0]`; `ldrh r4,[r6,#4]` | 0x398b04 | sends 6-byte debug-SFR request |
| 0x2a61d4 | `add lr,pc` @0x2a61d4 (base from pool 0x2a622c) | 0x398754 | `Bri_GetOutOfLegacy` |
| 0x2a61d6 | `add.w r12,lr,#0x3d8`; `ldr.w r0,[lr,#0x3d8]`; `ldr.w r1,[lr,#0x3dc]`; `ldrh.w r4,[r12,#8]` | 0x398b2c | 10-byte out-of-legacy request |
| 0x2a67b4 | `ldr.w r12,[pc,#0x4c]` + `add r12,pc` | 0x398754 | `Bri_RequestHwVersion` |
| 0x2a67be | `add.w r6,r12,#0x3e4`; `ldr.w r0,[r12,#0x3e4]`; `ldrh r4,[r6,#4]` | 0x398b38 | 6-byte HW-version request |

Other consumers of the same module base 0x398754 (literal-pool xrefs, each with a
different `add.w ...,#off`): 0x2a2078, 0x2a5518 (+0x350), 0x2a5d22 (+0x3a8),
0x2a70fa.  Those offsets address other sub-structures of the same rodata block,
not frame templates.

Cross-check with the in-tree bridge: `patches/userspace/gps/0002-...source.patch`
(`src/j7elte-gpsd.c`) uses exactly `OUT_OF_LEGACY = FE 00 FD 00 00 55 AA 00 FF FC`,
`REQ_HW_VERSION = FF 00 FD 00 00 FC` and `REQ_DEBUG_SFR = FF 00 FD 1F 95 FC`, i.e.
byte-identical to 0x398b2c / 0x398b38 / 0x398b04.  The autobaud burst (16 x 0x80,
`memset(buf,0x80,16)` at `Bri_SendAutobaud` 0x2a5ae4..0x2a5b08) is generated, not
tabulated.  The patch's template base `0x398b00..0x398b5c` should be corrected to
0x398afc; the frames at
0x398afc/0x398b0c/0x398b18/0x398b20/0x398b40/0x398b4c/0x398b54 are additional
requests the current daemon does not send (cmd 0x1F sub 0x94, cmd 0x00 sub 0x4D
with data 0x80/0x40, cmd 0x1B, cmd 0x40 sub 0x1A).

## T2. `gps.conf` descriptor table  - confidence HIGH (layout), MEDIUM (role)

`.data` 0x3da160, 20-byte records:
`{ const char *name; const char *key; const char *type; void *val; u32 size; }`
where `type` is the string "s"/"l"/"b" (str/int/bool), `val` is the runtime pointer
to the parsed value storage (0 in the file image, filled in at init) and `size` is
the accepted length (0x40 for strings, 4 for 32-bit ints).

        003da160  a0 c9 2f 00 cc 09 2f 00 ac c9 2f 00 00 00 00 00  ../.../.../.....
        003da170  40 00 00 00 b0 c9 2f 00 bc c9 2f 00 c8 c9 2f 00  @...../.../.../.
        003da180  00 00 00 00 04 00 00 00 cc c9 2f 00 d8 c9 2f 00  ........../.../.
        003da190  ac c9 2f 00 00 00 00 00 40 00 00 00 1c 14 2f 00  ../.....@...../.

Decoded first three records: `PortName/acPortName/s/0/0x40`, `BaudRate/lBaudRate/l/0/4`,
`NmeaOutName/acNmeaOutName/s/0/0x40`; then LogEnabled, LogDirectory, LogFileName,
LtoDir, ..., GpioNStdbyPath, GpioNResetPath, GpioDelayMs, RfkillNStdby, IoctlNStdby,
Supl*, Rti*, FactoryTest*.  Exactly the keys the daemon/`gps.conf` needs
(`BaudRate`, `GpioNStdbyPath`, `FactoryTestMode`, ...).

Code xrefs: 0x19036 (`ldr r4,[pc,#0xc8]` @0x19100 + `add r4,pc` => 0x3da160; the
sequence `bl 0x3a034 / adds r0,#0x40 / str r0,[r4,#0x20]`, `adds #0x44 -> [r4,#0x34]`,
`adds #0x84 -> [r4,#0x48]` writes the `val` field of records 0..3, i.e. the config
initializer), plus 0x390ca (`ldr r3,... @0x390f8` + `add r3,pc`, `ldr r1,[r3]`,
`cmp r1,#6`) and 0x3a06c. Because `.data` is writable, the `val` slots are filled at
runtime; name/key/type/size are static.

## T3. Flat `name`/`key` string-pointer array  - confidence MEDIUM

`.data.rel.ro` 0x3def50: 30 pointers (2 per config entry: display name, conf key,
15 entries), e.g. `PortName`/`acPortName`, `BaudRate`/`lBaudRate`, `NmeaOutName`/`acNmeaOutName`,
`LogEnabled`/`cLogEnabled`, ...  This is the string half of T2 (same order).
No literal-pool xref: the consumer indexes it from a base materialized once.
Neighbouring arrays 0x3dee50 (44 ptr) and 0x3dea48 (112 ptr) are the same kind of
descriptor data (0x3dea48 starts `RfType FrqPlan LogPriMask LogFacMask ...`).

        003def50  a0 c9 2f 00 cc 09 2f 00 b0 c9 2f 00 bc c9 2f 00  ../.../.../.../.
        003def60  cc c9 2f 00 d8 c9 2f 00 1c 14 2f 00 e8 c9 2f 00  ../.../.../.../.
        003def70  f8 c9 2f 00 08 ca 2f 00 18 ca 2f 00 24 ca 2f 00  ../.../.../.$./.

## T4. RIL assistance name/handler table  - confidence MEDIUM-HIGH (content), no xref found

`.data.rel.ro` 0x3c5118: two `.text` pointers (0x24f89, 0x24fa5 - likely generic
getter/setter thunks) followed by 18 `(conf-name, handler-name)` string pairs:

        003c5118  89 4f 02 00 a5 4f 02 00 dc 54 2f 00 f0 54 2f 00  .O...O...T/..T/.
        003c5128  04 55 2f 00 10 55 2f 00 1c 55 2f 00 30 55 2f 00  .U/..U/..U/.0U/.
        003c5138  40 55 2f 00 4c 55 2f 00 5c 55 2f 00 68 55 2f 00  @U/.LU/.\U/.hU/.
        003c5148  74 55 2f 00 84 55 2f 00 94 55 2f 00 a4 55 2f 00  tU/..U/..U/..U/.

Pairs: `reset_assist_data`/`RildResetAsstData`, `pos_meas`/`RildPosMeas`,
`location_parameters`/`RildAsstRefLoc`, `ref_time`/`RildAsstRefTime`,
`acqu_assist`/`RildAsstAcq`, `acqu_assist_gln`/`RildAsstAcqGln`,
`nav_model_elem`/`RildAsstEph`, `nav_model_elem_gln`/`RildAsstEphGln`,
`almanac`/`RildAsstAlm`, `almanac_gln`/`RildAsstAlmGln`, `UTC_model`/`RildAsstUtcModel`,
`ionospheric_model`/`RildAsstIono`, `GPS_rt_integrity`/`RildAsstRti`,
`GNSS_time_model`/`RildAsstTimeModel`, `GPS_assist_req`/`RildReqAsstCode`,
`GPS_meas`/`RildMeasGps`, `GNSS_meas`/`RildMeasGnss`, `location`/`RildFixInfo`.
This is the dispatcher mapping between BroadcastRadio RIL message names and the
gpsd handler entry points; useful if the daemon ever needs to speak the
HAL/RILD side, not the BCM4752 UART side.

## T5. ASN.1/LPP error-code -> text table  - confidence HIGH

`.data.rel.ro` 0x3c53c0, 69 string pointers (`Encode buffer overflow`,
`Unexpected end of buffer on decode`, `Enumerated value %s not in defined set`, ...).

        003c53c0  fc 5d 30 00 14 5e 30 00 38 5e 30 00 78 5e 30 00  .]0..^0.8^0.x^0.
        003c53d0  a0 5e 30 00 d0 5e 30 00 08 5f 30 00 48 5f 30 00  .^0..^0.._0.H_0.

Code xref: 0x68418 - `ldr r0,[pc,#0xc]` @0x68428 (pool 0x35cf9a) + `add r0,pc`
=> 0x3c53c0, then `b.w 0x683b4` with `r1 = -1`, `r2 = ~0x62` (lookup/unpack helper).
Used for the ASN.1 runtime's error printing (SUPL/LPP paths).

## T6. RILD trace-channel tag table  - confidence MEDIUM-HIGH

`.data.rel.ro` 0x3c50f0: four `.text` formatter pointers followed by
`"RILD_I"`, `"RILD_D"`, `"AP -> CP"`, `"AP <- CP"`.

        003c50f0  99 46 02 00 cd 46 02 00 8d 46 02 00 ed 46 02 00  .F...F...F...F..
        003c5100  88 54 2f 00 90 54 2f 00 70 54 2f 00 7c 54 2f 00  .T/..T/.pT/.|T/.

Indexed by channel/direction when logging RIL traffic; the strings are the exact
tag prefixes that appear in the RILD logging of this gpsd.  No literal-pool xref
(reached as base+offset).

## T7. `ignore_*` bitmask -> name table  - confidence HIGH

`.data` 0x3de92c: 7 records of 8 bytes `{u32 mask; const char *name;}`:
`1 ignore_nav`, `2 ignore_time`, `4 ignore_ram_alm`, `8 ignore_osc`,
`0x10 ignore_lto`, `0x20 ignore_rom_alm`, `0x40 "GPS"` (the last record is the
first entry of the neighbouring GNSS-name block, but the parser below copies 7
pairs, so 0x40 is part of the same mask enum).

        003de92c  01 00 00 00 a4 b9 31 00 02 00 00 00 b0 b9 31 00  ......1.......1.
        003de93c  04 00 00 00 bc b9 31 00 08 00 00 00 cc b9 31 00  ......1.......1.
        003de94c  10 00 00 00 d8 b9 31 00 20 00 00 00 e4 b9 31 00  ......1. .....1.
        003de95c  40 00 00 00 6c 0e 2f 00 8c 08 31 00 70 0e 2f 00  @...l./...1.p./.

Code xrefs: 0x151604 (`ldm r5!,{r0-r3}` x3 + `ldm.w`, copies the 14 words to the
stack, then `strcmp()`s each name at 0x151622..0x15166e against the user string),
and 0x1515ba (`ldr.w r2,[r3,r4,lsl #2]` indexed read of the same block).
This is the `IgnoreFlags`/`ignore_*` config parser.

## T8. Small u16 constant block  - confidence LOW

`.data` 0x3de690: `32 00 32 00 c8 00 90 01` = u16 {50, 50, 200, 400}, immediately
followed by string pointers (0x2f0dd8, 0x2f0690, 0x313398).  Reads like a small
table of timeout/threshold values (ms) embedded in a mixed struct; no xref found
that indexes it, so it is reported only for completeness.

        003de690  32 00 32 00 c8 00 90 01 d8 0d 2f 00 90 06 2f 00  2.2......./.../.
        003de6a0  98 33 31 00 00 00 00 00 00 00 00 00 00 00 00 00  .31.............

## T9. GNSS constellation name tables  - confidence MEDIUM

`.data` 0x3de960 is a 6-entry pointer array to `"GPS"`, `"SBAS"`, `"GLONASS"`,
`"QZSS"`, `"GALILEO"`, `"BEIDOU"`; 0x3de9a0 repeats the same 6 names (followed by
unrelated XML/format strings).  A shorter variant using `"GLNS"` sits at 0x3de678.
These are `ucGnssId` -> name tables; no literal-pool xref (base+offset consumers).

        003de960  6c 0e 2f 00 8c 08 31 00 70 0e 2f 00 9c 08 31 00  l./...1.p./...1.
        003de970  30 bd 31 00 78 0e 2f 00 00 00 00 00 00 00 00 00  0.1.x./.........

## Negative results (important for the remaining bring-up work)

* No `(opcode, len, ptr)` / `{u8 id; u8 len; void *handler}` dispatch table
  exists. A full scan of `.data.rel.ro`/`.data` for aligned words whose first two
  bytes are < 0x40 and whose 3rd word is in `.rodata`/`.text` (1165 hits) returns
  config descriptors of the T2/T3 kind plus coincidental matches inside floats and
  other structs - no dispatch table. The BRI request/response handling is inlined
  (`Bri_CheckForReceivedPacket_Sardine` compares `cmd`/`sub` with immediates),
  not table-driven.
* No baud-rate table: the 38400/115200 constants near 0x39646f and 0x391153 are
  members of unrelated `{int,int,float}` arrays; the only structured baud data is
  the single `BaudRate` descriptor field in T2 (runtime value 921600 for this
  device comes from `gps.conf`/`lBaudRate`, not from a table).
* NMEA output templates are plain C strings (`$GPGGA,%s,...` 0x33ce30,
  `$GPRMC,%s,` 0x33cf0c, `$%sGSV,...` 0x33cf4c, `$%sGSA,A,%d` 0x33cf7c), not
  tables; per-sentence dispatch is by sentence type in `GlPeNmeaGen`.
* The `$pglirm`/`IRM:` strings (0x31b8xx..0x321b8c) are formatting/assert text;
  the IRM/CTRL command parser switches on immediates (see 0x16850c which loads
  the `dataSize <= MAX_GL_COMM_PACKET_IN` string for its assert only).

## Practical takeaway for j7elte-gpsd

The init sequence the daemon must reproduce is exactly T1's first three frames
(out-of-legacy, HW version, debug SFR) plus the 16 x 0x80 autobaud burst, at the
baud the stock driver probes; every one of those bytes is present and verified in
the stock image, so a silent chip after these frames is not a frame-content bug.

