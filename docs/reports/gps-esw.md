# BCM4752 ESW / RAM patch embedded in the stock `gpsd`

Target: `/system/bin/gpsd` from the SM-J700M stock image
(`/run/media/asdf/.../j7stock/system.raw`), copied here as `/tmp/stock_gpsd`.

| property | value |
|---|---|
| file size | 4 059 464 bytes |
| SHA-256 | `9eadb64d5e0ad490377808b8e94697bdc11b29182ec3b9f10a40b2c2254c12fb` |
| type | ELF32 LSB, ARM EABI5, PIE `.so` (Android 23), stripped |
| `.rodata` | vaddr `0x002f0570` == file offset `0x002f0570`, size `0x000cfa11` |

Because `.rodata`'s link-time address equals its file offset, every data pointer
stored in the binary is simultaneously a file offset: extraction is a plain
`pread(ptr, len)`.

## 1. Result

The 8051 ESW/RAM patch is not one blob but **four records** in a segment table,
selected by the ASIC revision string. For this device (4752_B2) the image is
**10 845 bytes in 4 blocks**:

| # | bank | load addr | file offset | size | CRC-32 | SHA-256 |
|---|---|---|---|---|---|---|
| 0 | 0 | `0x7000` | `0x003a4410` | 4060 | `0x884e382c` | `79f09f9d7ee3bff5a86d8e613deff95c70795cb1a68885f854c8b9f5ac29c829` |
| 1 | 0 | `0x7fec` | `0x003a53f0` | 4 | `0xddfac19e` | `a63be51b45b90365f7c5b8ecb30922c92bfa355a90f90052c92f93ba236e1d46` |
| 2 | 1 | `0x8000` | `0x003a53f8` | 6777 | `0xd4f6cf7f` | `fdb57d8df7f2da5663d5f5e3fcde7536a9263ef74e122e9f232d032fc017463b` |
| 3 | 0 | `0x7fd2` | `0x003a6e78` | 4 | `0x2144df1c` | `df3f619804a92fdb4057192dc43dd748ea778adc52bc498ce80524c014b81119` |

Concatenation (block order 0,1,2,3): 10 845 bytes,
CRC-32 `0x1370aa54`,
SHA-256 `e3219a3e62aae56f14b47e3cb3ca59ba1b03ae48aa5a617fb4a016605a7e7a9d`.

Block 3 is **not stored in the table**; the daemon synthesises it in code
(`@0x2da196`, see §3). Blocks 0–2 are stored verbatim in `.rodata`.

Sanity: block 0 starts `12 79 07 22 22 …` (8051 `LCALL 0x7907` / `RET`s),
block 1 is `02 73 00 00` (`LJMP 0x7300`), block 2 starts
`78 2a e2 75 f0 45 …` (`MOV R0,#0x2A`), block 3 is four zero bytes.

## 2. Container format in this binary

The public reimplementation describes the vendor container as 24-byte records

```c
struct { uint8_t flags; uint32_t addr; uint64_t ptr; uint32_t len; }   /* x86-64 */
```

(`extract_patch.py` parses it with `"<BxxxIQI"`, 24-byte stride). That layout is
from the *x86-64* vendor daemon, where the data pointer is 64-bit. This Android
build is ARM32, so the same source compiles to a **16-byte** record with a
32-bit pointer:

```c
struct esw_record {          /* ARM32, 16 bytes, natural alignment */
    uint8_t  flags;          /* +0  bank number (0,1,2)       */
    uint32_t addr;           /* +4  16-bit load address       */
    uint32_t ptr;            /* +8  VA of data == file offset */
    uint32_t len;            /* +12 byte count                */
};                           /* flags byte then 3 pad bytes   */
```

Proof that `ptr` is a relocated absolute pointer and not an offset: every
record's `+8` slot carries an `R_ARM_RELATIVE` relocation in `.rel.dyn` —
relocation offsets (vaddrs) are exactly the ten `ptr` slots
`0x3c6d10, 0x3c6d20, 0x3c6d30, 0x3c6d40, 0x3c6d50, 0x3c6d60, 0x3c6d70,
0x3c6d80, 0x3c6d90, 0x3c6da0`, all `type == 23 == R_ARM_RELATIVE`.

Record consumers read exactly these four fields (`0x2da13c`–`0x2da150`:
`ldrb [rec]; ldr [rec+4]; ldr [rec+8]; ldr [rec+0xc]`).

### Table location

All three revision tables live in `.data.rel.ro.local`
(vaddr `0x3c4f68`, file `0x3c3f68` ⇒ `file = vaddr - 0x1000`):

| table | vaddr | file offset | records |
|---|---|---|---|
| 47521_A0 | `0x3c6d08` | `0x3c5d08` | 5 |
| 4752_B2 | `0x3c6d58` | `0x3c5d58` | 3 |
| 4752_B3 | `0x3c6d88` | `0x3c5d88` | 2 |
| terminator | `0x3c6da8` | `0x3c5da8` | `00…00` |

Raw bytes of the whole table (`0x3c5d08`–`0x3c5db7`, 10 records + terminator):

```
0x3c5d08  00 000000  00006000  003a6e80  00000ff6   ; A0 0  addr 0x6000
0x3c5d18  00 000000  00007000  00398ed0  00000fdc   ; A0 1
0x3c5d28  00 000000  00007fec  00399eb0  00000004   ; A0 2
0x3c5d38  01 000000  00008000  00399eb8  00001b2c   ; A0 3  bank 1
0x3c5d48  02 000000  00008000  0039b9e8  00007a3e   ; A0 4  bank 2
0x3c5d58  00 000000  00007000  003a4410  00000fdc   ; B2 0
0x3c5d68  00 000000  00007fec  003a53f0  00000004   ; B2 1
0x3c5d78  01 000000  00008000  003a53f8  00001a79   ; B2 2  bank 1
0x3c5d88  00 000000  00007000  003a3428  00000fdc   ; B3 0
0x3c5d98  00 000000  00007fec  003a4408  00000004   ; B3 1
0x3c5da8  00 000000  00000000  00000000  00000000   ; end
```

## 3. How the segment is selected

```
gps.xml RfType → enum 0x20 ─┐ (RF front-end config; not the table key)
                            │
HW version (0x1c9474) = 0x30020005
        │
        ├─ 0x2a8760(this)  or  0x2a9080(this)  → const char *revision
        │      "4752_B2"
        │
PatchLoader::SetData()  @0x2a9184
        │   r0 = this, revision(lookup), flag = [this+0x28], index = [this+0x10]
        └─ 0x2da0fc(revision, flag, index, &flags, &addr, &ptr, &len)
```

### 3.1 Revision string

Both classifiers are binary searches over a 32-bit version word read from the
ASIC object (`[this]` or `[this+0x14]`; populated at `0x2a1a04` from
`0x1c9474([asic+0x4c])`, mode `0x1000`, and the ctor `0x2a9328` at
`0x2a1a20`). For the value this phone reports, `0x30020005`:

* `0x2a8760`: `0x30020004 ≤ v ≤ 0x30020005` → block `0x2a8826` → `"4752_B2"`
* `0x2a9080`: `0x30020004 ≤ v ≤ 0x30020005` → `0x2a9158`/`0x2a915a` → `"4752_B2"`
  (`0x2a912c` `cmp r0,#0x30020004`; `0x2a9136` `cmp r1,#0x30020005`; `bls` → `0x2a9158`)

The revision strings live at `0x34d218`–`0x34d25c`
(`4752_B2`, `4752_B3`, `47521_A0/A1`, `4753_A0/A1/A2`, `4752_B1` @`0x34d564`).

### 3.2 Record lookup

`0x2da0fc(name, flag, index, &flags, &addr, &ptr, &len)` — `flag` is
`[this+0x28]` (0 on the normal path; 1 for the `0x3010/0x3023` variants):

* `flag == 0`, `name == "4752_B2"` (`0x2da16e`, `0x2da190`):
  * `index 0..2` → record `table_B2 + 16*index` (`0x3c6d58`)
  * `index 3` → synthetic record built at `0x2da196`:
    `flags=0, addr=0x7fd2, ptr=0x3a6e78, len=4`
  * `index ≥ 4` → no record (falls through the B3/A0 tests to `0x2da1b4`,
    returns 0)
* `flag == 0`, `name == "4752_B3"` (`0x2da188`):
  * `index 0..1` → `table_B3 + 16*index` (`0x3c6d88`)
  * `index 2` → the same synthetic 4-byte record
* `flag == 1`, `name == "47521_A0"` (`0x2da118`–`0x2da132`):
  * `index 0..4` → `table_A0 + 16*index` (`0x3c6d08`); index 5 is dead code
    after the `cmp r4,#5 / bhs` guard.

`SetData` stores the result in the loader object:
`flags→+0x18`, `addr→+0x1c`, `ptr→+0x20`, `len→+0x24`
(`0x2a9216`–`0x2a922e`).

### 3.3 Where `GL_RF_47520_BRCM_EXT_LNA` fits

`/etc/gps.xml` on this image (`debugfs -R "cat /etc/gps.xml"`) sets

```xml
RfType="GL_RF_47520_BRCM_EXT_LNA"
```

The string→enum table is at file `0x3c4b90` (vaddr `0x3c5b90`), 8-byte
`{char *name; uint32_t id}` entries; the relevant ones:

| name | id |
|---|---|
| `GL_RF_4752_BRCM` | `0x15` |
| `GL_RF_4752_BRCM_EXT_LNA` | `0x16` |
| `GL_RF_4753_BRCM_EXT_LNA` | `0x1a` |
| `GL_RF_47531_BRCM_EXT_LNA` | `0x1e` |
| **`GL_RF_47520_BRCM_EXT_LNA`** | **`0x20`** |

That enum is **not** the selector: `SetData` is passed `[this+0x28]` (a boolean
flag, 0 here) and the revision *string*, which comes from the ASIC version word
— no `RfType` value reaches `0x2da0fc`. The table key is therefore the ASIC
revision, and the `RfType` only states which RF front end is fitted. For this
unit both agree on the 47520 die (`0x30020005` → `4752_B2`,
`GL_RF_47520_BRCM_EXT_LNA` → 47520 family) — **[INFERENCE]** that the two names
denote the same silicon; the patch bytes themselves follow from the version
word, not from the XML.

### 3.4 The other image: full ESW for 4752_B3

A second container exists for 4752_B3 only, used by the contiguous-image loader
(ctor `0x2a8860`, `GetDataToSend @0x2a8a5c`): the lookup `0x2da1f8`
compares `"4752_B3"` and returns

* version string `0x353974` = `make-ESW:…/esw_dev:4752_B3@215693_4752_B3`
* data `0x3a7e78`, len `0x1794e` = 96 654 bytes (ends `0x3bf7c6`)

That blob starts with an 8051 reset-vector table
(`02 03 31 / 02 7f e0 / …`) and is the whole ESW, not a patch. For
`4752_B2` the same lookup returns “no image”, which is the
`No ESW Image to Load, using ROM` (`0x34c9b4`) path: this phone's chip keeps its
ROM ESW and only receives the 10.8 KB patch above.

## 4. How the image is consumed (bank, address, CRC)

`PatchLoader` state machine (`0x2a7690` region):

* next block: `0x2a9270` — advances `offset += blocklen`; when
  `offset ≥ len` it increments the index (`[this+0x10]`) and calls `SetData`.
* `0x2a8d2c` → `flags` (bank); `0x2a8d70` → `addr + offset` (`[this+0x1c]`,
  `[this+0x14]`); `0x2a8dcc` → `ptr + offset` (`[this+0x20]`).
* `0x2a8e24` → next transfer size: `min(len - offset, [this+4])` where
  `[this+4]` is the ctor's `0x1000` (4096) limit, with a tail of ≤3 bytes
  folded back to 4 so the last word is complete (`0x2a8e36`–`0x2a8e40`).
* sender `0x2a71b0` builds the P6 frames from the local binary, matching the
  public spec byte for byte:
  * data frame `FE <n> FD 61 17 DE C0 <alo> <ahi> …data… FC`,
    `n = (len+3)/4` (`0x2a7258`), payload split at `0x3fc = 1020` bytes
    (`0x2a7208`) and at `0xa0 = 160` bytes per frame (`0x2a72a4`);
  * odd-tail frame at `0x2a73d6` (last 4 bytes sent at `addr+len-4`);
  * verify frame `FE 00 FD 21 18 <alo> <ahi> <llo> <lhi> FC` at `0x2a744a`.
* bank: `flags` is the bank index. The loop reads it before and after loading
  the next record (`0x2a77f2`, `0x2a7806`) and separates blocks whose bank
  differs; the corresponding wire frame is the P5 switch
  `FE 00 FD 61 16 <bank> 00 00 00 00 FC` (builder `0x2a5ce4`, see
  `docs/reports/gps-protocol.md` §P5), issued through the transport vtable
  (`0x2a592c`). B2 blocks 0/1/3 are bank 0, block 2 is **bank 1**.

### CRC / size fields

The record has **no** CRC or checksum field — only `flags`, `addr`, `ptr`,
`len`. The chip computes the CRC: after each block the host asks for it with
`21 18 addr len` and the reply `FE 00 FD 2F 15 <4B> FC` is stored
(`obj+0x13c` = 32-bit value, `obj+0x140` = status byte, handler `0x2a7ad4`)
and logged as `Patch CRC Received(0x%X, 0x%lX) %c`; it is **not compared**
against anything (`0x2a7aa4` just clears the stage). `len` is exact and is the
value echoed in the verify frame, so the only integrity data available offline
is the SHA-256/CRC-32 in §1.

## 5. Is extraction complete?

Yes for `4752_B2` (the revision this phone selects); the *image itself* is
deliberately sparse, not partially extracted.

Evidence:

1. **Only one such table exists.** A scan of the entire 4 059 464-byte file
   (every 4-byte offset, both strides) for runs of ≥2 records with `flags ≤ 2`,
   `addr < 0x10000`, `ptr` inside `.rodata`, `0 < len ≤ 0x40000` finds a single
   contiguous region: a maximal run of 10 records starting at file `0x3c5d08`
   (the A0/B2/B3 tables), followed by the terminator. The 24-byte x86 layout
   (`"<BxxxIQI"`) matches nowhere; no other 16-byte run exists.
2. **Terminator.** The record following B3's two entries is all-zero
   (file `0x3c5da8`), matching the container convention.
3. **Index bound is enforced in code.** For `4752_B2`, `index ≥ 4` returns “no
   record” (`0x2da192` `bhi`, then the B3/A0 tests fail → `0x2da1b4` returns
   0), so blocks 0..3 are the complete set.
4. **Relocations** confirm all 10 `ptr` fields are genuine pointers (§2).

Coverage in the 16-bit load space (B2):

```
bank0  0x7000-0x7fdc   block 0
bank0  0x7fd2-0x7fd6   block 3  (overlaps block 0: a post-load fixup, 4 zero bytes)
bank0  0x7fec-0x7ff0   block 1
bank1  0x8000-0x9a79   block 2
```

Not transferred: `0x7fdc-0x7fec` and `0x7ff0-0x8000` (16 bytes each). This is
the same pattern as the public capture (“pieces are whole transferred regions,
not one region split by an address rule”), so the holes are the vendor's, not
extraction loss. Outside those four ranges no address in
`0x7000`–`0x9a79` is referenced by any record.

## 6. Dumper

`code/gps/esw/esw_dump.c` — single C11 file, no dependencies (own SHA-256 and
CRC-32, own ELF32 section-table mapper). It re-derives the table addresses,
validates each `ptr` with an `R_ARM_RELATIVE` relocation check, and prints
`index, flags(bank), load address, file offset, size, CRC-32, SHA-256` per
block plus the concatenated digest and the covered ranges.

```
$ make
$ ./esw_dump /tmp/stock_gpsd            # 4752_B2 (this phone)
$ ./esw_dump /tmp/stock_gpsd --all      # A0 + B2 + B3
$ ./esw_dump /tmp/stock_gpsd --rev b3
$ mkdir -p out && ./esw_dump /tmp/stock_gpsd --dump out/b2   # one .bin per block
```

Output on the real binary is in `code/gps/esw/esw-dump.txt`; B2 section:

```
file: /tmp/stock_gpsd (4059464 bytes)
sha256(file): 9eadb64d5e0ad490377808b8e94697bdc11b29182ec3b9f10a40b2c2254c12fb
.rodata: vaddr 0x002f0570  file offset 0x002f0570  size 0x000cfa11

== revision 4752_B2 ==
   table: vaddr 0x003c6d58  file offset 0x003c5d58  records 3
   idx  bnk  load addr  file off   size    crc32       sha256
     0     0  0x7000     0x003a4410    4060  0x884e382c  79f09f9d…
     1     0  0x7fec     0x003a53f0       4  0xddfac19e  a63be51b…
     2     1  0x8000     0x003a53f8    6777  0xd4f6cf7f  fdb57d8d…
     3     0  0x7fd2     0x003a6e78       4  0x2144df1c  df3f6198…  (synthetic 4-byte zero block @0x2da196)
   image: 4 blocks, 10845 bytes, crc32 0x1370aa54, sha256(concat) e3219a3e…
   ranges: bank0:0x7000-0x7fdc bank0:0x7fec-0x7ff0 bank1:0x8000-0x9a79 bank0:0x7fd2-0x7fd6
```

No firmware bytes are committed to the repo; run the dumper against your own
copy of the image. Nothing has been uploaded to the chip. (Working copies of
the four B2 blocks for offline analysis are at `/tmp/gps_esw/`, regenerated by
the `--dump` command above.)

## 7. Not established

* Meaning of the per-revision layout differences beyond “bank, address, span”:
  why A0 has a block at `0x6000` and 31.3 KB in bank 2, and why B3 lacks a
  bank-1 block, is not explained by the code.
* The synthetic block's purpose (4 zero bytes at `0x7fd2`, i.e. inside the
  region already loaded by block 0) — the string `Patch will jump to ROM`
  (`0x34c56c`) suggests a ROM entry fixup, **[INFERENCE]**.
* `0x2a8cc4`'s “another block” predicate compares `[this+0x10]` (index) against
  `[this+8]`; the writer of `[this+8]` was not found, so the exact stop
  condition of the send loop is untraced (the lookup bound in §3.2 is the
  enforced limit either way).
* Whether the chip's `2F 15` CRC equals any host-computable checksum: the
  daemon stores and logs it but never compares, so no algorithm could be
  confirmed here.
* Content-level validation of the 8051 patch (it is only shown to be plausible
  8051 code at the segment starts).
