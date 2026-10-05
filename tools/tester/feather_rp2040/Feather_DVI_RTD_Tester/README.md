# Feather DVI RTD tester

Engineering tester for an Adafruit Feather RP2040 DVI connected by HDMI to an
independently powered RTD2660 display controller. USB commands select DVI video
patterns, read EDID, and access the controller's SPI flash through HDMI DDC.

For HDMI audio and programming with a Feather RP2350 HSTX, use the
[HSTX tester](../../feather_rp2350/Feather_HSTX_RTD_Tester/README.md).
It shares this ISP driver and host CLI; USB discovery recognizes both boards.

**Bench status, 2026-09-28:** tested with a UCTRONICS UC-586 RTD2660H display and
W25X40 flash. A complete 512 KiB read matched two original backups. A page was
programmed in verified blank sector 0x20000, read back, and erased; the entire
flash then matched the original SHA256 again. Protection remained 0x0C.
EDID base and extension checksums passed. The webcam confirmed video patterns
at 640x480 and 800x480, and an RTD restart followed by video recovery.

The initial 800x480 build with QSPI /2 lost USB. The checked build uses QSPI /4,
starts with video off, and has a watchdog for video startup. These results cover
this board and flash part, not every RTD266x module or Feather overclock margin.
The webcam checks confirm visible response, not calibrated color accuracy.
Flash programming was tested with video off.
The full host `program` and `program --recover` paths also passed using that
scratch sector, including whole-image readback and protection restoration.

## Connections and build

- Connect the Feather and display to their own USB power, then connect HDMI.
- HDMI DDC uses GPIO2 SDA and GPIO3 SCL through the Feather's level shifters.
- Install the Earle Philhower RP2040 Arduino core, PicoDVI - Adafruit Fork, and
  its Arduino Library Manager dependencies.
- Select **Adafruit Feather RP2040 DVI** and **W25Q080 QSPI /4** boot stage 2.
- Tested compile versions: RP2040 core 6.1.1, PicoDVI 1.3.2, Adafruit GFX 1.12.6.

From the repository root:

```sh
arduino-cli compile --fqbn rp2040:rp2040:adafruit_feather_dvi:boot2=boot2_w25q080_4_padded_checksum tools/tester/feather_rp2040/Feather_DVI_RTD_Tester
pip3 install pyserial
```

Upload the sketch normally. If the Feather loses USB, hold BOOT, tap Reset,
release BOOT, and copy the compiled UF2 to RPI-RP2.

## Host commands

Run these from this folder. The host discovers one connected Feather DVI; use
`--port COM23` or `--serial <USB serial>` before the command to select one.
Output files must be new paths; existing files are never overwritten.

```sh
python host.py info
python host.py scan
python host.py edid panel-edid.bin
python host.py ddc-config
python host.py mode 640
# Wait for USB to reconnect after a mode change.
python host.py pattern bars
python host.py pattern red
python host.py capture screen-red.png
python host.py mode off
python host.py dump original.bin
python host.py verify original.bin
```

The camera command uses Windows DirectShow and ffmpeg, selecting `USB Camera`
at 1920x1080 MJPEG. Use `--device` to select another compatible camera.

`ddc-config` reports hex bytes for a fixed whitelist: DDC partition (FF21),
DDC1 controls (FF1B–FF1D), DDC2 controls (FF1E–FF20), DDC3 controls (FF2C–FF2E),
pin share 14 (FFA4), watchdog (FFEA), ISP slave/MCU/clock (FFEC–FFEE), bank control
(FFFC), XDATA start/select (FFFD/FFFE), program bank switch (FFFF), and the
REV_DUMMY2/REV_DUMMY6 scratch bytes (FF19/FFF2). A separate
`channel_access` object contains FFEC/FFED from one two-byte read at I2C 0x4B:
EC bit 1 indicates DDC1, EC bit 0 DDC2, and ED bit 6 DDC3. The configuration
bytes are sequential reads, not an atomic snapshot.

These external registers require ISP mode (RTD2660 manual p316). The host enters
ISP to halt the RTD MCU, reads the snapshot, and finishes only if ISP was not
already active. Finishing restarts the MCU, so this command can interrupt video.
An existing ISP session stays halted. The low-level `ddc-config` command requires
an active, identified session and only reads the fixed registers; it never
enters/exits ISP, changes configuration bytes, or accesses index/data ports.

Video modes are 640x480@60 and 800x480@60, with pixel-doubled RGB565 framebuffers.
`mode panel` selects 800x480 at 31.5 MHz, totals 1000x525, H front/sync/back
112/48/40, V 13/3/29, and negative HS/VS. This matches the provisional SDCC
panel profile; `mode 800` retains PicoDVI's CVT timing. The `panel_timing`
field in `info` identifies this selection. Panel mode uses a 315 MHz RP2040
clock and retains the video-startup watchdog recovery.
On the selected UC-586, the SDCC firmware measured 800x480 active pixels and
1000 pixels per line from this mode, then displayed bars, readable text, and
a grid (2026-09-29 UTC). The grid exposed a capture-alignment issue in the RTD
firmware; these webcam checks do not establish calibrated panel timing/color.
Patterns are bars, checker, red, green, blue, gray, black, white, grid, and text.
There is no HDMI audio generator or native RGB888 one-pixel pattern mode.
800x480 requires a higher RP2040 overclock and may not run on every Feather.
Try 640 first. Reset returns to video off. A startup watchdog returns to that
mode if video initialization stalls; the watchdog is disabled before DDC work.

## Flash operations

The enabled flash profiles are **Winbond W25X40 (`EF3013`)** and
**Zetta ZD25Q40 (`5E6013`)**, both 512 KiB. Other flash IDs are refused.
This is not yet a universal RTD266x programmer.
Reads check each block against the RTD hardware CRC. A backup performs two
complete reads and saves only matching results, with a SHA256 JSON sidecar.

```sh
python host.py program modified.bin --backup original.bin --allow-write
```

Programming requires `info` to report video `off`. Use `mode off` and wait for
USB to reconnect before programming; the host refuses active video and never
changes modes automatically.

Programming checks the full current image against the supplied backup or target,
rewrites only changed 4 KiB sectors, verifies pages, sectors, and the complete
image, then restores the original flash protection and leaves ISP. Leaving ISP
restarts the RTD's MCU; it does not power-cycle the display or its panel rails.
The flash controller is reset with ISP still holding the MCU halted (`81`,
two-second hold, `80`), checked, then ISP is released (`00`) and checked again.
The hold time follows an existing programmer's precedent, not a documented
silicon minimum. The MCU is never deliberately released with controller reset
still asserted.

After a failed write, keep power connected and restore the verified original:

```sh
python host.py program original.bin --backup original.bin --allow-write --recover
```

Recovery requires identical target and backup files. It deliberately permits a
partial current image and leaves ISP active on failure. Read-only operations
preserve an ISP session that was already active. An explicit `reset` command is
only appropriate when the flash contains a complete, valid image.
`reset-chip` requests the whole-chip SOF_RST at FFEE bit 1. It requires a
complete, valid flash image and rejects armed or changed-protection sessions.
It checks idle flash and write-disable first, then waits up to three seconds
for DDC to respond. Only FF6F reads are retried; the reset write is never
repeated automatically. A timeout leaves reset completion unconfirmed.
If ISP remains active, it clears SOF_RST and uses the checked ISP-exit sequence;
otherwise it checks that the flash controller is released without reentering ISP.
A successful command confirms register access and reset release, not video or
firmware execution. EDID may still be unavailable while stock firmware starts.
Use a fresh visible pattern or an execution marker to confirm execution.
On the UC-586, the checked command returned successfully, stock EDID and visible
color bars returned, and a subsequent full-flash read matched the original hash
with protection still at 0x0C (2026-09-29 UTC).

Saved protection is held in Feather RAM. If the Feather itself resets during
programming, the new session cannot reconstruct the previous protection setting.
When the complete intended image is already present, recover the recorded
protection byte from a trusted earlier receipt, with video off:

```sh
python host.py restore-protection current-full.bin --status 0x0c --allow-write --receipt protection-recovery.json
```

This command requires the exact current 512 KiB image and a new receipt path.
It identifies W25X40, reads every flash byte through the existing hardware-CRC
checked read path, and compares the entire image before arming any status write.
Only nonzero BP protection may change; TB/SRP and other stable bits are preserved.
It rereads status, checks the requested protection, then explicitly finishes ISP
and resets the RTD. The receipt records image/read hashes, before/target/after
status where available, and whether protection writing and finish were attempted.
Errors leave the session as encountered, with no additional finish/reset or retry.
Keep display power connected and inspect the receipt before taking another step.
For a partial or mismatched image, restore the verified backup using `program
--recover` first; this command does not erase or repair flash contents.

The Python `Client` API exposes the same line-oriented JSON commands for bench
tests. Closing a client never resets the display. Low-level callers must verify
their image and explicitly issue `finish` before allowing it to boot.
