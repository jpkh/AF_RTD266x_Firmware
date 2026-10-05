# Live menus and DDC/CI

The firmware exposes its settings controller over DDC/CI at seven-bit I2C
address `0x37`. The Feather RP2350 HSTX tester can send commands while video
runs or while its video output is off, without ISP or a reset. Physical button
sampling is not yet implemented. The earlier menu releases passed the live
navigation, setting readback and DDC-during-drawing bench checks below.

Use the shared CLI at
`tools/tester/feather_rp2040/Feather_DVI_RTD_Tester/host.py` with an RP2350 tester:

```sh
python host.py key menu
python host.py key down
python host.py key menu
python host.py menu-state
python host.py key back
python host.py vcp-set 0x8d 1
python host.py vcp-get 0x8d
python host.py vcp-set 0x8d 2
```

Add `--port` or `--serial` before the subcommand to select a tester. Menu opens
a four-icon rail on the left: Picture, Audio, Display and Menu Settings.
Up/down select a category and preview its current settings in the right pane;
Menu enters that pane. Up/down then select a row, and Menu enters/leaves its
adjustment. Back leaves adjustment, returns to the same category icon, or
closes the rail. Percentage adjustments step by five. Disabled rows do not
enter edit mode. Picture contains a Color submenu; Menu Settings contains
No Signal, OSD Setup and System. Back from a nested page returns to the row
that opened it. Menu Settings has seven rows in a five-row viewport: selecting
System or Back displays rows 2–6, and moving above System restores rows 0–4.
The footer indicates that more rows are available.

The added Color, OSD Setup, System and reset-confirmation pages passed controller
host tests and the v40 bench navigation checks below. The v44 checks qualify
saturation behavior and forced ratios with the native 800x480/525-line input.
Menus are English-only; translated menus and font coverage are deferred.

## Control map

Codes below are hexadecimal; values are decimal unless prefixed with `0x`.
Settings persist in the separate board EEPROM by default (`SETTINGS=1`).
Use `SETTINGS=0` for session-only preferences. Settings writes target only that
EEPROM; the application has no intentional program-flash write path.

| VCP | Control | Accepted values / readback |
| --- | --- | --- |
| `04` | Factory reset | Write 1 restores defaults; reads 0, maximum 1 |
| `12` | Image contrast | 0–100, neutral/default 50 |
| `16`, `18`, `1A` | Red, green, blue gain | 0–100 each, neutral/default 50; combined with contrast |
| `62` | Audio volume | 0–100 linear amplitude, default 100; zero mutes independently of `8D` |
| `87` | Horizontal sharpness | 0–100, default 50; lower softens, higher sharpens |
| `8A` | Color saturation | 0–100, neutral/default 50 |
| `8D` | Audio mute | 1 mute, 2 unmute |
| `D6` | Soft power | 1 on, 4 off |
| `DF` | VCP version, read-only | `0x0202` |
| `E0` | Virtual key event | 1 menu, 2 back, 4 up, 8 down, 16 power toggle; reads 0 |
| `E1` | Menu state, read-only | `(page << 8) \| (selection << 1) \| editing` |
| `E2` | Image brightness | 0–100, neutral/default 50 |
| `E3` | Aspect | 0 Keep (default), 1 Fill; 2 forced 4:3 with `ASPECT_4_3=1`; 3 forced 16:9 with `ASPECT_16_9=1` and compatible input |
| `E4` | Startup Splash | 0 off, 1 on; default follows build-time `SPLASH`; saved value restores before startup display |
| `E5` | Connection Popup | 0 off, 1 on (default) |
| `E6` | No Signal Background | 0 black, 1 blue, 2 test bitmap (default) |
| `E7` | No Signal Sleep After | 0 Never (default), 1=1s, 2=2s, 3=5s, 4=10s, 5=20s, 6=30s, 7=40s, 8=50s, 9=60s |
| `E8` | Menu Timeout | 0 Never, 1=5s, 2=10s (default), 3=20s |
| `EB` | Settings storage status, read-only | Low byte: 0 unavailable/disabled, 1 blank, 2 loaded/saved, 3 error; bit8 means a save is pending or in progress |
| `F0`, `F1` | OSD horizontal, vertical position | 0–100 across the visible panel; default 50 centers the menu |
| `F2` | OSD background transparency | 0–100; default 0 opaque, mapped to eight hardware blend levels |
| `F3` | Sleep timer | 0 off (default), 1–120 minutes until soft power off |
| `F4` | Burn-in color test | 0 off (default), 1 on; transient, never saved |
| `F6` | Flash CRC job | Write 1 for bank0 or 2 for vendor probe; reads 0 idle, 1 busy, 2 ready; maximum 2 |
| `F7`, `F8` | Firmware CRC result, read-only | Low/high 16 bits respectively, maximum 65535; unavailable until ready |
| `F9` | Firmware CRC progress, read-only | Completed 256-byte pages; maximum 256 for bank0 or 32 for vendor probe |
| `FA` | Firmware CRC region, read-only | 0 before a job, 1 bank0, 2 vendor probe; maximum 2 |
| `FB`, `FC` | Bitmap decode time, read-only; `BITMAP_TIMING=1` only | Last splash/no-signal decode time in milliseconds; 65535 before first load |
| `FD`, `FE` | Bitmap OSD load time, read-only; `BITMAP_TIMING=1` only | Last splash/no-signal complete OSD load time in milliseconds; 65535 before first load |

The mute and power values follow [ddcutil's MCCS reference](https://www.ddcutil.com/vcpinfo_output/).
`E0`–`EB` and `F0`–`FE` are project-specific. Reserved `F5` reads English=0
with maximum 0, rejects writes and is omitted from the capabilities string.
LED backlight (`10`) is unsupported and omitted
from the capabilities string; its menu row is disabled with a gray `--`.
Mirror and rotation are also unavailable. Image brightness
changes pixel values independently of LED backlight.

The optional bitmap timing build runs an extra decode into a volatile sink
before the normal display upload, using the same decoder. `FB`/`FC` include
that sink overhead; `FD`/`FE` include decoding, SRAM writes, map/palette setup
and DDC service, but exclude the extra benchmark pass and the one-second splash
hold. The board timer has two-millisecond ticks. These diagnostics are omitted
from the capabilities string; ordinary builds omit both the benchmark pass and
these four controls. See [custom artwork](../assets/README.md) for usage.
Fast programming waits ten seconds after reset before its final CRC query,
allowing both startup and no-signal artwork to finish even in a timing build.

For `E1`, page numbers are 0 closed, 1 category rail, 2 Picture, 3 Audio, 4 Display,
5 Menu Settings, 6 No Signal, 7 Color, 8 OSD Setup, 9 System and 10 Reset
Settings. Selection is zero-based within the complete page, including hidden
rows; editing is bit zero.
For example, `0x0201` means Picture, first row, adjustment active. Selection
bits are relevant while a menu is open. `menu-state` returns the raw VCP value.
On the rail, selection 0–3 identifies Picture, Audio, Display or Menu Settings;
for example, `0x0104` previews Display, and Menu changes to `0x0400` to enter it.

Picture has brightness, contrast, Color, horizontal sharpness and Back. Color
has red/green/blue gain, saturation and Back. Menu Settings retains splash,
popup, menu timeout and No Signal, then adds OSD Setup, System and Back.
OSD Setup has horizontal/vertical position, transparency and Back. System has
Sleep (minutes), Burn-in, Factory Reset and Back. Factory Reset opens a
two-row confirmation with Cancel selected; Back also cancels. Selecting Reset
restores all preferences and returns to the System reset row. A direct `04=1`
command performs the reset immediately without opening confirmation.
The host client waits one second after `04=1` before accepting another
transaction. Reset reapplies several hardware blocks; sending another Set
and Get after only the usual 50 ms can overrun the receive FIFO. Automation
that confirms reset through virtual menu keys should also pause one second
before its next command. The bus ACK alone is not reset-completion status.

Position and transparency affect live menus only, leaving splash artwork,
no-signal artwork and input timing popups at their existing positions and
opacity. Horizontal placement uses four-pixel hardware steps. Transparency
blends the menu background with video from opaque through 7/8 video; text
remains opaque. Sharpness is horizontal filtering, with 50 retaining the
original linear filter. See [video register details](video-registers.md) for
color coefficients and aspect paths. Both aspect build flags default to 1;
set either to 0 to omit that forced ratio. `E3` reports the effective
displayed aspect. Its maximum follows the compiled modes; omitted modes and
16:9 with incompatible input are rejected. A saved 16:9 preference can remain
stored while the actual display falls back to Keep, and changing build flags
does not discard other settings merely because that saved mode is unavailable.

Startup Splash controls startup and resume after `D6=4` then `D6=1`. With
`SETTINGS=0`, a reset restores the build-time `SPLASH` default. No-signal sleep
requests backlight power off after the selected delay and on when valid video
is acquired. An
open menu postpones sleep and wakes the backlight. Soft power off also stops
audio and blanks video; DDC/CI remains serviced for resume. The retained vendor
flash tail is not used for settings storage.

The separate `F3` sleep timer runs even with valid input or an open menu and
enters soft power off (`D6=4`) when it expires. Its interval starts at boot,
on a timer-setting request, or on soft power on. Resume with a power-key event
or `D6=1`; returning input alone does not resume this intentional power-off.
Resuming starts a full new interval. Timer adjustment steps are one minute.

Burn-in replaces input video with red, green, blue, white and black backgrounds,
holding each for two seconds. Audio stops during the test; menus and DDC remain
available so `F4=0` can exit and reacquire input. Reset, factory reset and a
soft-power transition clear burn-in. Its state is never restored from EEPROM.

## Settings storage

`SETTINGS=1` is the default for the qualified UC-586 board. It restores picture,
color, sharpness, aspect, volume/mute, OSD position/transparency, splash, popup
and timeout preferences before startup display.
Changes are coalesced
for two seconds, then saved to a separate 24LC16B EEPROM. Soft power and menu
focus and burn-in are not saved. Power loss during the two-second delay can discard the
most recent adjustments.

The driver follows GPIO routines found in the UC-586 stock disassembly:
P6.6/RTD pin56 (`FFCD`) for SCL and P6.7/pin57 (`FFCE`) for SDA, open-drain
selection `FF9A=05`. The board photograph identifies a 24LC16B, whose
[Microchip datasheet](https://ww1.microchip.com/downloads/en/devicedoc/20002213b.pdf)
specifies 2048 bytes and 16-byte write pages. `FFC0` bit 3 must also be set:
it selects physical P6 input readback instead of the output latch. Without
that setting, the released SDA latch looked like a NACK even when the EEPROM
was acknowledging. The driver now reads and writes the board's EEPROM.

Two matching original 2048-byte backups were obtained before the first write,
with SHA256
`6d1f20d29512af538be93feb1aadfdb7ef7ed3ae697f76c93b581cf821d2a8ca`.
The qualified reservation is `0x4C0..0x4FF`, within a blank region, split into
two 32-byte records. Schema/count, sequence, CRC16 and a commit-last marker
select the newest complete record. Loaded values are range-checked before
applying them. Unknown occupied data or recognizable newer schemas are not
overwritten. The previously proposed `0x7C0..0x7FF` range contains stock data
and is not used. Confirm wiring, make two matching complete backups, and
qualify the reservation separately before enabling storage on another board.
A complete readback after two hardware saves confirmed that all 1984 bytes
outside the reservation still matched the original backup. Both records passed
CRC and commit-marker checks.

The payload now contains 21 bytes within those same two 32-byte records. Its
first eleven bytes retain the v39 layout; appended fields are red, green, blue,
saturation, sharpness, OSD X/Y/transparency, sleep minutes and reserved language.
The explicit compatible loader accepts the old eleven-byte payload and keeps
defaults for appended fields, choosing the newest valid sequence across both
formats. The next preference save writes 21 bytes into the other slot and
retains the old record until the new commit completes. Unknown versions or
payload lengths remain protected. An older strict loader may recover a
surviving old-format slot but cannot save beside a newer-format record.
Host tests cover every migration write interruption, final-readback failure,
sequence wrap, defaults and preservation outside the reservation. These tests
do not replace a physical legacy-record migration/power-removal check. The v43
hardware test saved ten nondefault values in the expanded record and restored
them after a whole-chip reset: RGB gains, sharpness, saturation, OSD X/Y and
transparency, volume and aspect mode 3. This verifies the new record's reset
restoration, not actual removal of board power.

Failed writes leave the runtime preferences usable and report 3 in the low
byte of `EB`; another setting change permits a new attempt. A valid older record
survives an interrupted update. A first-ever torn write before any valid
ownership record may conservatively disable saves rather than overwrite
unrecognized data.

Saves take an immutable snapshot and service DDC between completed EEPROM
transactions, after STOP releases the bus. Save-time reads and writes use
four-byte chunks, with additional polling around CRC preparation. A setting
changed by a DDC callback remains queued for another save
after its own two-second delay. `EB` bit 8 remains set while either save is
pending or in progress. No DDC callback runs inside an EEPROM transaction.

Hardware saves and restoration of ten changed preferences passed a whole-chip
reset with application XRAM cleared. A physical power-disconnect test remains
pending. The v39 release passed 120 alternating volume/status reads at the normal
50 ms transaction spacing during a save. A second test changed volume through
the Audio menu from 25 to 20, then to 15 about 2.45 seconds later while the first
save was underway. Ninety subsequent reads passed, and 15 restored after reset.
An 18-second analog recording during the first test had no measured dropouts:
50 ms windows ranged from -27.506 to -27.434 dBFS at 25% volume, excluding the
first and last second. Host tests cover corruption, interrupted page operations, write
protection, readback failure, sequence/timer wrap, coalescing, startup restoration
and settings changed at each of the 36 save-service boundaries. Interrupted
writes are checked at all ten write operations, including successful recovery
when the inactive slot started blank beside one valid record.

Startup explicitly clears the application's 512-byte XRAM allocation before
SDCC's explicit initializers: `--no-xinit-opt` omits the usual XRAM clear.
Long-lived monitor state now lives in XRAM, and the video-measurement pointer
uses its XRAM address space. These changes avoid stale flags after reset and
8051 stack overflow when DDC callbacks run during video measurement.

The v39 image passed full 512 KiB readback with flash protection restored to
`0x0C`; full-image SHA256 is
`261633f6b4c6e9241fc396f08d9ae272e80617f4523caffaf037e2403e1dfa29`.
For that v39 release, the default and rainbow-splash SDCC builds passed host
checks using 54,365 and 63,162 bytes of code respectively, with 353 of 512 XRAM
bytes used. Its `SETTINGS=0` variant also passed. These are historical build
sizes, not the size of the expanded menu implementation.

For a read-only bench build, use `SETTINGS=0 EEPROM_DIAGNOSTICS=1`.
Set VCP `E9` to a byte address 0..2046; Get `EA` returns two bytes, high byte
first, and advances by two (wrapping to zero). Get `EC` reports read-failure
stage in the high byte and mux/line state in the low byte. These diagnostic
codes do not write EEPROM and are absent from normal builds. The 16-bit
readout uses existing `host.py` VCP commands; no programmer changes are needed.
Use 120 ms transaction gaps for live-video `EA` reads; 50 ms bulk diagnostic
reads have failed. This slower diagnostic setting is separate from ordinary
VCP controls.

The PWM1 experiment accepted VCP requests for 100%, 25% and 0%, but three camera
captures showed unchanged brightness. Level adjustment is therefore disabled;
`board_backlight_available()` returns false. Separate `board_backlight_power()`
uses P6.4 (pin 54), `0xFFCB` bit 0, following the stock button's output path.
The gate's physical backlight-off and wake behavior was verified on the UC-586.
Limor also confirmed continuity from the boost IC's EN to pin 54 on 2026-09-29.
This provides on/off control, not adjustable LED brightness.

## Firmware CRC and fast updates

The firmware CRC result matches `zlib.crc32` over one explicitly selected
flash region. Writing `F6=1` starts code bank0, addresses `0x000000..0x00FFFF`,
including padding (65,536 bytes, 256 pages). Writing `F6=2` starts the vendor
probe at `0x010000..0x011FFF` (8,192 bytes, 32 pages). The latter reads the
first two retained vendor sectors through the firmware's mapped XDATA window;
it does not cover the remaining retained tail. Neither region includes the
external settings EEPROM.

Poll Get `F6` for status and `F9` for page progress. When Get `F6` returns 2
(ready), verify `FA` matches the requested region ID (1 bank0, 2 vendor probe),
then read low word `F7` and high word `F8`; combine as `(F8 << 16) | F7`.
The value 2 means vendor probe when written to `F6`, and ready when read.
A start while busy is rejected, and result words remain unavailable until
completion. These are runtime operations, not settings or flash writes.

The shared host's `firmware-crc IMAGE` defaults to `--region bank0` and accepts
a 64 KiB bank or full 512 KiB image. `--region vendor-probe` requires a full
512 KiB image and compares only the 8 KiB range above. Both have a 120-second
deadline and report scope, region ID, address and size. The host rejects an
already-busy job, a ready result without an observed busy state after starting,
or a mismatched region and never retries failed transfers or
restarts a timed-out job. A mismatch reports expected and observed CRC values.

`program --fast` always requests bank0 and uses that interface only when current
firmware is running and supports it, video is off, and supplied full-size
target/backup tails match.
ISP must identify an enabled flash (W25X40 `EF3013` or ZD25Q40 `5E6013`)
with whole-flash protection
(`status & 0x1c == 0x1c`) before any unlock or write. Partial protection such
as `0x0c` requires normal full verification and restoration of full protection
before fast updates are allowed.
Before ISP, live CRC must match the backup or target bank. ISP still reads and
exactly compares bank0, and every changed bank0 sector is read back. After
protection restoration, ISP exit and whole-chip reset, the host waits three
seconds and checks the target's live CRC. It skips the final 512 KiB readback
and explicitly records that the unchanged tail was not read. Full programming
and recovery remain the defaults; recovery cannot use `--fast`. See the
[tester workflow](../tools/tester/README.md#firmware-crc-and-faster-bank0-updates)
for commands and receipt scope.

The UC-586 investigation on 2026-09-30 found intermittent changes in retained
bank1 with W25X40 protection `0x0C` (only the upper half protected). The live
probe and ISP readback agreed: an 8 KiB region that matched while halted could
already differ before the next ISP entry. One replay changed `0x10000` from
`02` to `00` and `0x10029` from `E8` to `40`; its live CRC remained unchanged
during a subsequent 45-second wait. This brackets the fault to ISP release or
early execution, without identifying the writer.

The shared programmer now verifies WIP/WEL clear and protection after its final
flash-controller reset, before releasing the MCU. That guard is not a proven
fix: under partial protection, three release cycles passed and the fourth
changed `0x10000`. Full-flash protection `0x1C` contained the observed fault.
All diagnosed bytes were restored from the verified backup. Fast updates require
that full protection; the settings EEPROM remains writable independently.

The byte-table CRC build was installed through `program --fast` on that board:
16 changed bank0 sectors (246 programmed pages) passed readback, protection
restoration and post-reset CRC `79CCF2EE`. The update took 139.4 seconds without
the final 512 KiB readback. Subsequent live checks took 29.8 seconds with the
source off and 33.7 seconds during 640x480 video. The vendor probe remained
`6807A515`. During a live checksum, a 53-second analyzed analog capture measured
a 1000.02 Hz tone at -21.436 dBFS; 50 ms windows ranged from -21.452 to -21.402
dBFS, with no windows more than 6 dB below the median. The camera confirmed
color bars with Keep-aspect sidebars. Menu, Down, Up and Back each produced
the expected menu state while the CRC job reported busy, and its final result
still matched. These results qualify this UC-586 and
HSTX tester combination, not other boards or timings.

## Transport and validation

The firmware implements Get VCP (`01`), Set VCP (`03`) and capabilities requests
(`F3`). Get VCP replies contain the maximum and current value. The host validates
the checksum, header, echoed code, result and type. Set VCP has no application
reply: a bus ACK alone does not establish that a setting was accepted; read it
back to check. Unsupported gets report unsupported; invalid sets leave settings
unchanged. No host command is automatically retried.

The host defaults to 50 ms transaction spacing for ordinary VCP controls;
live EEPROM diagnostics use 120 ms as described above. The v39 cooperative-save
release passed the rapid-read and audio stress checks described above; newer
control checks are recorded below. The tester's raw
`ddc HEXPACKET 0` and `ddc - N` operations are bounded to 32 bytes and refuse a known active ISP
session. Capabilities replies are fragmented in groups of at most ten text
bytes. See the [RP2350 tester instructions](../tools/tester/feather_rp2350/Feather_HSTX_RTD_Tester/README.md)
for transport details. Programming still requires `mode off` and the existing
backup, authorization, readback and protection safeguards.

After programming, explicitly run `python host.py reset-chip` before returning
the tester to `mode 640`. An ISP-only MCU restart retained DDC peripheral state
on the bench; a whole-chip reset restored the live interface.

On 2026-09-30, the expanded controls passed these UC-586 checks:

- v40: Color, OSD Setup, System and reset-confirmation navigation, Settings
  pagination and Cancel passed. The menu was positioned at the top right and
  displayed with transparency set to 100. A one-minute intentional sleep
  switched soft power off after 60 seconds; DDC power-on resumed it. Burn-in
  cycled red, green, blue, white and black at two-second intervals and exited
  back to input.
- v41: the compressed no-signal bitmap displayed correctly, and the 30-second
  no-signal sleep setting switched the physical backlight off.
- v43: immediate Get readback after factory reset returned all nineteen checked
  defaults. A request arriving during the reset setter is now retained rather
  than discarded when the receive FIFO is reset; the host transport regression
  also injects and checks that exact sequence. Ten nondefault preferences in
  the expanded EEPROM record restored after a whole-chip reset as listed above.
- v43: sharpness changes to 0, 100 and 50 and a fifteen-read sequence at 50 ms
  spacing passed. Camera comparison of Fill at sharpness 0 versus 100 showed
  a modest edge change; this is a functional check, not image calibration.
- The experimental 16:9 build displayed the complete 640x480 test grid across
  an 800x450 image, including all fifteen grid rows. Input off/on recovery and
  a menu at the bottom of that viewport also passed.
- v44 with the HSTX native 800x480/525-line input: Keep showed the complete
  800x480 grid; forced 4:3 retained all 25 columns, all 15 rows and both red
  borders in a centered 640-pixel image with 80-pixel sidebars. Forced 16:9
  showed the complete image in an 800x450 letterbox, and 4:3 restored after
  input off/on. CVT 4:3 remains physically untested; CVT 16:9 falls back to
  Keep. These results qualify geometry. Native-800 audio failed continuity
  with periodic mutes/pops; the tester is left in its passing 640 mode.
- v44's corrected saturation precision produced luminance bars at 0, reduced
  chroma at 25 and the original bars at 50; the DDC sweep passed. Gray ramps
  at 0/100 were broadly preserved with a common camera/panel blue cast. This
  is not color calibration. Saturation-100 coefficient arithmetic is host
  tested; already-maximal primary colors clip and do not demonstrate its
  full adjustment range.
- Red gain zero removed red in v40; green and blue gain zero each removed
  their channel in v44, and returning each gain to 50 restored it.
- The final v44 client check sent factory reset, volume 50 and 21 control
  readbacks successfully with the one-second reset pause. Preferences saved
  to EEPROM. The release bank matched the fully verified installed image;
  flash protection was restored to `0x0C`.

Actual board power removal and restoration remains untested. The dated v39
audio/save stress results below and above are separate from these newer checks.

The 2026-09-29 v31 font/icon-rail build passed full 512 KiB readback and restored
protection to `0x0C`; its full-image SHA256 was
`b5aeb66f6e3d40081dcf518f3bc075c3d2998d5ab57a40310d36b9d7ea35d4d5`.
Thirty key events checked the four category previews, all settings pages,
No Signal, edit mode, unavailable rows and category-preserving Back. Ten setting
readbacks confirmed the normal defaults after brightness and mute adjustments.
An additional 53 category changes ran during a 20-second analog audio capture:
the 1 kHz tone remained at -15.364 dBFS RMS, with 50 ms windows ranging from
-15.373 to -15.357 dBFS and no measured dropouts. The analysis excludes the
capture's first and last second. Bitmap-to-text replacement, the timing popup,
return to Picture and ten-second menu expiry also passed. Photos partly obscure
the lower rail/footer; the host SRAM renderer checks their complete layout.

On 2026-09-29, the UC-586 v29 image passed full 512 KiB readback and protection
restoration to `0x0C`. Its full-image SHA256 was
`f0525e30e37b741788cc8fa8b253bec8f05ffc5d78698511090317afe4c42b26`.
Camera captures confirmed all six live pages: Main, Picture, Audio, Display,
Menu Settings and No Signal. Virtual keys, edit mode and Back matched `E1`
state readback; disabled rows could not enter adjustment. Brightness, contrast,
aspect, splash, popup and timeout settings round-tripped. With polling during
drawing, 50 ms DDC transaction spacing passed while menus rendered. A simultaneous
30-second audio capture had no measured dropouts; see the
[audio results](audio.md#validation).

The same v29 build then passed these physical checks:

- Contrast 25 darkened the picture; image brightness 75 lifted black to gray.
  Both were restored to neutral 50.
- VGA Keep showed a centered 640-pixel grid with sidebars; Fill expanded it to
  the 800-pixel panel width. Keep was restored.
- Virtual-key mute produced `8D=1` and reduced recorded RMS by about 36 dB;
  `8D=2` restored the tone. This was attenuation, not measured zero silence.
- `D6=4` visibly darkened the panel; `D6=1` recovered it. No Signal Black and
  Blue were visually confirmed. A follow-up Blue-to-Test transition showed the
  complete Adafruit test card after an eight-second settling interval, with
  `E6=2` readback. The earlier two-second capture had shown only black.
- Selecting `E7=2` after input had already been absent for more than two seconds
  switched the backlight off while firmware remained running and `D6` stayed
  1. Returning the tester to `mode 640` visibly woke the panel to color bars,
  with final `D6=1`.

The last test verifies configured expiry and valid-signal wake, not a precisely
measured two-second interval from initial signal loss. It confirms P6.4/pin 54
as the physical backlight gate; the ineffective PWM1 dimming path stays disabled.

`MENU_PREVIEW=1` is a separate static artwork exercise with sample values. Its
previous camera validation does not establish live menu, DDC or PWM behavior.
