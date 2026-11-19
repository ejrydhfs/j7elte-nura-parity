RE tooling for docs/reports/gps-protocol.md (stock /system/bin/gpsd, Thumb-2, VA==file offset).

  armdis.py <hexaddr> [n]   linear Thumb-2 disassembly around addr (capstone 5.0.7)
  testconv.py               proves the PIC convention: string target = literal + pc (add rX,pc),
                            not absolute (30172 vs 44 hits against known string starts)
  xref2.py [addr...]        builds/dumps xrefs: literal+pc -> add-site for `ldr rX,[pc]; add rX,pc`
  scan.py, base_users.py    first-pass literal scans (xrefs of raw pool values; base+offset users)

Inputs assumed: /tmp/stock_gpsd. Regenerate listing.txt with the loop in docs/reports/gps-protocol.md
method section if needed (1.08M insns, ~26 MB).
