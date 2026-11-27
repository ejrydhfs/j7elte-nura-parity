# J7 parity — omp session transcripts

The three omp sessions that produced this repository, verbatim (`jsonl`, one JSON
record per line, as written by omp). Fragments of the middle were compacted in the
live sessions, so state that survived compaction is in `docs/`.

| File | Session | Date | Lines | SHA-256 |
|---|---|---|---|---|
| `2026-10-24_01a16c42-samsung-rom-kernel-re.jsonl` | `01a16c42-0334-7126-841d-1d7a03af4f31` | 2026-10-24 | 1420 | `153859a2c6c100384b27015ef4a49ff1acf8f4d682054a0c3d1e6abe6e04e300` |
| `2026-11-14_01a1d845-pmos-twrp-install-camera.jsonl` | `01a1d845-226c-7344-9052-466a7210e374` | 2026-11-14 | 4092 | `8f50dea95823a5e3ff978306711be05c288f13dce6495253d5a8ba03af52b082` |
| `2026-11-17_01a1eaa3-bringup-resume.jsonl` | `01a1eaa3-9b98-76a5-8d93-39deff8d1888` | 2026-11-17 → 2026-11-27 | 14942 | `2dcb9f968055bf06c1c0ec8aefbed7e5fbff1a80f8fb3da963ae8cc787df4658` |

## What each session covers

| Session | Scope |
|---|---|
| `01a16c42` | Samsung SM-J700M stock-firmware RE: kernel.org comparison, `morluto/rea` + Ghidra, which hardware drivers the Samsung ROM actually ships. |
| `01a1d845` | postmarketOS/Nura install on the J7 via TWRP (recovery zip, boot images, rootfs on SYSTEM), then the camera bring-up: FIMC-IS firmware, ISP pin handover, `HIC_ISP_I2C_CONTROL`. |
| `01a1eaa3` | Bring-up resume: GPU 155 fps (leak, mmap-offset race, dma-fence shim fps bug), GPS (GLME/ESW), audio (UCM, PCM), modem/RIL (kernel int49 + 19 clean GPU runs), then hardware DECON/DSIM KMS bring-up to KWin 6.8.3 + Plasma Mobile 6.8.3, the GPU31 panfrost7580 driver through kernel int121 (blank-frame guard), and the panel-flash / panfrost-timeout investigation of 2026-11-27. |

## Reading them

```sh
# user/assistant text only, in order
jq -r '.message.content[]? | select(.type=="text") | .text' sessions/2026-11-17_01a1eaa3-bringup-resume.jsonl

# tool calls with intents
jq -r 'select(.type=="custom" and .customType=="tool_execution_start") | [.data.toolName, .data.intent] | @tsv' sessions/*.jsonl | head
```

Device-specific facts (USB net `172.16.42.2/24` / `172.16.42.1`, ssh user `user`, password
`nura`, kernel int49 image hashes, knob names) are in `docs/SESSION-STATE.md` and
`docs/reports/gpu31-*.md`, not required for reading a transcript.

No credentials other than that local-device password appear in these files; the scan for
`gho_`/`ghp_`/`github_pat_`/`AKIA`/`xoxb-`/`sk-ant`/SSH private keys returns empty.
