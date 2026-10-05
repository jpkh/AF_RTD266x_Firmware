// SPDX-FileCopyrightText: 2026 Limor Fried for Adafruit Industries
// SPDX-License-Identifier: MIT
// USB-controlled HSTX HDMI audio/DDC tester. Reuses the RP2040 host.py CLI.
// The display needs its own power. HDMI DDC uses GPIO2/3 through the HSTX
// adapter. Video is 640x480 or 800x480 with a doubled RGB565 framebuffer.

#include <Adafruit_DVI_Audio.h>
#include <pico_hdmi/hstx_pins.h>
#include <Wire.h>
#include <errno.h>
#include <hardware/structs/watchdog.h>
#include <hardware/watchdog.h>
#include <new>
#include "RTD266xISP.h"

const uint32_t MODE_COOKIE = 0x48535458;
const uint16_t SERIAL_LINE_BYTES = 1024;
const int16_t TONE_AMPLITUDE = 1000; // Leaves headroom at a USB mic input.
const uint8_t DDC_CI_ADDRESS = 0x37; // VESA DDC/CI seven-bit slave address.
const uint8_t DDC_CI_MAX_BYTES = 32;
Adafruit_DVI_Audio_GFX16 *display = nullptr;
alignas(Adafruit_DVI_Audio_GFX16)
    uint8_t displayStorage[sizeof(Adafruit_DVI_Audio_GFX16)];
// Publish only after construction, pin configuration and begin() succeed.
volatile bool videoReady = false;
RTD266xISP flash;
RTD266xISP flash1(Wire1); // Direct RTD ISP probing on the GPIO4/5 bus.
RTD266xISP *isp = &flash; // Selected ISP bus; see the bus command.
uint16_t videoWidth = 0;
int16_t sine[48]; // One 1 kHz period at 48 kHz.
uint8_t phase = 0;
char commandLine[SERIAL_LINE_BYTES];
uint16_t commandLength = 0;
bool lineOverflow = false;

void setup() {
  Serial.begin(115200);
  // Do not wait for USB: keep generating video when the host disconnects.
  delay(250);

  if (watchdog_hw->scratch[0] == MODE_COOKIE &&
      (watchdog_hw->scratch[1] == 640 || watchdog_hw->scratch[1] == 800)) {
    videoWidth = watchdog_hw->scratch[1];
  }
  // A normal reset or a stalled video startup returns to programming mode.
  watchdog_hw->scratch[0] = 0;
  if (videoWidth) {
    watchdog_enable(8000, true);
    // Adafruit 22-pin adapter: clock, data0, data1, data2; positive/negative.
    const pico_hdmi_hstx_pinout_t pins = {
        {14, 15}, {{18, 19}, {16, 17}, {12, 13}}};
    dvi_audio_mode_t mode = DVI_AUDIO_640X480;
    if (videoWidth == 800) {
      mode = DVI_AUDIO_800X480;
    }
    display = new (displayStorage) Adafruit_DVI_Audio_GFX16(mode);
    if (!video_output_set_hstx_pinout(&pins) || !display->begin(48000)) {
      printError("HSTX video startup failed; rebooting to mode off");
      Serial.flush();
      delay(100);
      rp2040.reboot();
    }
    for (uint8_t i = 0; i < 48; ++i) {
      sine[i] = (int16_t)(sinf(i * 2 * PI / 48) * TONE_AMPLITUDE);
    }
    drawPattern("bars");
    // begin() selects 252 MHz (640) or 315 MHz (800). Start core 1 afterward.
    __dmb();
    videoReady = true;
    watchdog_disable();
  }
  // On this board Wire is the physical I2C1 controller, GPIO2/3 on the FPC.
  Wire.setSDA(2);
  Wire.setSCL(3);
  Wire.begin();
  Wire.setClock(100000);
  Wire.setTimeout(100);
  // Second bus for direct RTD ISP probing: physical I2C0 on GPIO4/5.
  Wire1.setSDA(4);
  Wire1.setSCL(5);
  Wire1.begin();
  Wire1.setClock(100000);
  Wire1.setTimeout(100);
}

void setup1() {
  while (!videoReady) {
    delay(1);
  }
  __dmb();
  display->runCore1();
}

void loop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') {
      continue;
    }
    if (c == '\n') {
      commandLine[commandLength] = 0;
      if (lineOverflow) {
        printError("Command is too long");
      } else if (commandLength) {
        processCommand(commandLine);
      }
      commandLength = 0;
      lineOverflow = false;
    } else if (commandLength < SERIAL_LINE_BYTES - 1) {
      commandLine[commandLength++] = c;
    } else {
      lineOverflow = true;
    }
  }
  if (display) {
    feedAudio();
    delayMicroseconds(50);
  } else {
    delay(1);
  }
}

void feedAudio() {
  int16_t frames[128];
  size_t count = display->audioAvailableForWrite();
  if (count > 64) {
    count = 64;
  }
  count -= count % 4; // Audio packets contain four stereo frames.
  if (!count) {
    return;
  }
  for (size_t i = 0; i < count; ++i) {
    int16_t sample = sine[(phase + i) % 48];
    frames[i * 2] = sample;
    frames[i * 2 + 1] = sample;
  }
  phase = (phase + display->audioWrite(frames, count)) % 48;
}

void printError(const char *message) {
  Serial.print("{\"ok\":false,\"error\":\"");
  Serial.print(message);
  Serial.println("\"}");
}

void printResult(bool ok) {
  if (ok) {
    Serial.println("{\"ok\":true}");
  } else {
    printError(isp->error());
  }
}

void printHex(const uint8_t *data, size_t length) {
  const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < length; i++) {
    Serial.write(hex[data[i] / 16]);
    Serial.write(hex[data[i] % 16]);
  }
}

bool number(const char *text, uint32_t &value) {
  if (!text || !*text || *text == '-') {
    return false;
  }
  char *end;
  errno = 0;
  unsigned long parsed = strtoul(text, &end, 0);
  if (errno || *end) {
    return false;
  }
  value = parsed;
  return true;
}

bool hexBytes(const char *text, uint8_t *data, size_t &length) {
  if (!text) {
    return false;
  }
  size_t chars = strlen(text);
  if (!chars || chars % 2 || chars > 512) {
    return false;
  }
  length = chars / 2;
  for (size_t i = 0; i < length; i++) {
    char pair[3] = {text[2 * i], text[2 * i + 1], 0};
    if (!isxdigit(pair[0]) || !isxdigit(pair[1])) {
      return false;
    }
    data[i] = strtoul(pair, nullptr, 16);
  }
  return true;
}

bool readEdid(uint8_t block, uint8_t *data) {
  bool ok = true;
  if (block >= 2) {
    Wire.beginTransmission(0x30);
    Wire.write(block / 2);
    ok = Wire.endTransmission() == 0;
  }
  for (uint16_t offset = 0; ok && offset < 128; offset += 32) {
    Wire.beginTransmission(0x50);
    Wire.write((uint8_t)((block % 2) * 128 + offset));
    if (Wire.endTransmission(false)) {
      ok = false;
      break;
    }
    if (Wire.requestFrom((uint8_t)0x50, (uint8_t)32) != 32) {
      ok = false;
      break;
    }
    for (uint8_t i = 0; i < 32; i++) {
      data[offset + i] = Wire.read();
    }
  }
  // Also restore the segment pointer after a short read or lost ACK.
  if (block >= 2) {
    Wire.beginTransmission(0x30);
    Wire.write((uint8_t)0);
    if (Wire.endTransmission()) {
      ok = false;
    }
  }
  return ok;
}

void ddcTransfer(const char *packet, const char *replyLength) {
  uint8_t data[256]; // hexBytes() also serves the 256-byte flash page command.
  size_t bytes = 0;
  uint32_t length = 0;
  if (!packet || !number(replyLength, length) || length > DDC_CI_MAX_BYTES) {
    printError("DDC requires a hex packet and 0, or - and a read length 1..32");
    return;
  }
  bool reading = !strcmp(packet, "-");
  if ((reading && !length) ||
      (!reading && (length || !hexBytes(packet, data, bytes) ||
                    bytes > DDC_CI_MAX_BYTES))) {
    printError("DDC uses separate writes and reads, each at most 32 bytes");
    return;
  }
  if (isp->active()) {
    printError("Live DDC requires the RTD firmware running, outside ISP");
    return;
  }
  // Keep the 50 ms DDC/CI processing delay on the host. Each command performs
  // one short bus transaction, then audio feeding resumes in loop().
  if (display) {
    feedAudio();
  }
  Wire.setTimeout(10);
  bool ok;
  if (reading) {
    bytes = Wire.requestFrom(DDC_CI_ADDRESS, (uint8_t)length);
    ok = bytes == length;
    for (size_t i = 0; i < bytes; ++i) {
      data[i] = Wire.read();
    }
  } else {
    Wire.beginTransmission(DDC_CI_ADDRESS);
    ok = Wire.write(data, bytes) == bytes;
    if (ok) {
      ok = Wire.endTransmission() == 0;
    }
  }
  Wire.setTimeout(100); // Restore the existing timeout for EDID and ISP.
  if (display) {
    feedAudio();
  }
  if (!ok) {
    printError("DDC transaction failed or returned a short read; not retried");
  } else if (reading) {
    Serial.print("{\"ok\":true,\"data\":\"");
    printHex(data, bytes);
    Serial.println("\"}");
  } else {
    Serial.print("{\"ok\":true,\"written\":");
    Serial.print(bytes);
    Serial.println("}");
  }
}

void processCommand(char *line) {
  char *cmd = strtok(line, " ");
  char *arg1 = strtok(nullptr, " ");
  char *arg2 = strtok(nullptr, " ");
  char *arg3 = strtok(nullptr, " ");
  if (strtok(nullptr, " ")) {
    printError("Too many arguments");
    return;
  }
  // Programming and chip-reset operations use the stock 150 MHz clock with
  // HSTX completely stopped. A host cannot bypass this through raw commands.
  if (display && (!strcmp(cmd, "isp") || !strcmp(cmd, "read") ||
      !strcmp(cmd, "arm") || !strcmp(cmd, "unlock") ||
      !strcmp(cmd, "restore-protection") || !strcmp(cmd, "erase") ||
      !strcmp(cmd, "page") || !strcmp(cmd, "finish") ||
      !strcmp(cmd, "isp1") || !strcmp(cmd, "reset") ||
      !strcmp(cmd, "reset-chip"))) {
    printError("Use mode off before ISP or flash operations");
    return;
  }
  uint32_t address = 0;
  uint32_t length = 0;
  uint8_t data[256];
  if (!strcmp(cmd, "hello")) {
    Serial.print("{\"ok\":true,\"firmware\":\"FeatherDVI-RTD\",\"protocol\":1,\"video\":\"");
    if (display) {
      Serial.print(videoWidth);
      Serial.print("x480@60\",\"framebuffer\":\"");
      Serial.print(display->width());
      Serial.print("x240 RGB565");
    } else {
      Serial.print("off\",\"framebuffer\":\"none");
    }
    Serial.print("\",\"panel_timing\":");
    Serial.print(videoWidth == 800 ? "true" : "false");
    Serial.print(",\"isp_active\":");
    // Recover the actual RTD state even if the Feather itself has rebooted.
    // An absent or unresponsive RTD is unknown, never a false running claim.
    bool ispActive;
    if (isp->readISPState(ispActive)) {
      Serial.print(ispActive ? "true" : "false");
    } else {
      Serial.print("null");
    }
    Serial.print(",\"isp_bus\":\"");
    Serial.print(isp == &flash1 ? "gpio" : "ddc");
    Serial.println("\"}");
  } else if (!strcmp(cmd, "scan")) {
    Serial.print("{\"ok\":true,\"addresses\":[");
    bool first = true;
    for (uint8_t i = 1; i < 127; i++) {
      Wire.beginTransmission(i);
      if (Wire.endTransmission() == 0) {
        if (!first) {
          Serial.print(',');
        }
        Serial.print(i);
        first = false;
      }
    }
    Serial.println("]}");
  } else if (!strcmp(cmd, "scan1")) {
    Serial.print("{\"ok\":true,\"addresses\":[");
    bool first = true;
    for (uint8_t i = 1; i < 127; i++) {
      Wire1.beginTransmission(i);
      if (Wire1.endTransmission() == 0) {
        if (!first) {
          Serial.print(',');
        }
        Serial.print(i);
        first = false;
      }
    }
    Serial.println("]}");
  } else if (!strcmp(cmd, "bus")) {
    RTD266xISP *target = nullptr;
    if (arg1 && !strcmp(arg1, "ddc")) {
      target = &flash;
    } else if (arg1 && !strcmp(arg1, "gpio")) {
      target = &flash1;
    }
    if (!target) {
      printError("bus must be ddc or gpio");
    } else {
      // Each driver object keeps its own session state, so switching the
      // selected pointer is safe; mode changes still gate on both objects.
      isp = target;
      Serial.print("{\"ok\":true,\"bus\":\"");
      Serial.print(arg1);
      Serial.println("\"}");
    }
  } else if (!strcmp(cmd, "i2c1")) {
    uint32_t addr = 0;
    size_t bytes = 0;
    if (!arg1 || !arg2 || !arg3 || !number(arg1, addr) ||
        addr == 0 || addr > 127) {
      printError("i2c1 requires address 1..127, r <length> or w <hex>");
    } else if (!strcmp(arg2, "r")) {
      if (!number(arg3, length) || !length || length > 256) {
        printError("i2c1 read length must be 1..256");
      } else if (Wire1.requestFrom((uint8_t)addr, (uint8_t)length) != length) {
        printError("i2c1 read failed or returned a short read");
      } else {
        for (size_t i = 0; i < length; i++) {
          data[i] = Wire1.read();
        }
        Serial.print("{\"ok\":true,\"data\":\"");
        printHex(data, length);
        Serial.println("\"}");
      }
    } else if (!strcmp(arg2, "w")) {
      if (!hexBytes(arg3, data, bytes) || !bytes) {
        printError("i2c1 write payload must be 1..256 hex bytes");
      } else {
        Wire1.beginTransmission((uint8_t)addr);
        bool ok = Wire1.write(data, bytes) == bytes;
        if (ok) {
          ok = Wire1.endTransmission() == 0;
        }
        if (!ok) {
          printError("i2c1 write failed");
        } else {
          Serial.print("{\"ok\":true,\"written\":");
          Serial.print(bytes);
          Serial.println("}");
        }
      }
    } else {
      printError("i2c1 operation must be r or w");
    }
  } else if (!strcmp(cmd, "isp1")) {
    uint8_t status;
    if (flash1.enter()) {
      if (!flash1.readStatus(status)) {
        printError(flash1.error());
      } else {
        Serial.print("{\"ok\":true,\"jedec\":\"");
        Serial.print(flash1.jedecId(), HEX);
        Serial.print("\",\"size\":");
        Serial.print(flash1.flashSize());
        Serial.print(",\"status\":");
        Serial.print(status);
        Serial.println("}");
      }
    } else {
      // A nonzero JEDEC proves the RTD answered; report it as a non-fatal
      // mismatch so the host prints it. Total I2C failure stays ok:false.
      uint32_t id = flash1.jedecId();
      Serial.print("{\"ok\":");
      Serial.print(id ? "true,\"supported\":false" : "false");
      Serial.print(",\"error\":\"");
      Serial.print(flash1.error());
      Serial.print("\",\"jedec\":\"");
      Serial.print(id, HEX);
      Serial.println("\"}");
    }
  } else if (!strcmp(cmd, "edid")) {
    if ((arg1 && !number(arg1, address)) || address > 7) {
      printError("EDID block must be 0 through 7");
    } else if (!readEdid(address, data)) {
      printError("EDID read failed");
    } else {
      Serial.print("{\"ok\":true,\"block\":");
      Serial.print(address);
      Serial.print(",\"data\":\"");
      printHex(data, 128);
      Serial.println("\"}");
    }
  } else if (!strcmp(cmd, "ddc")) {
    ddcTransfer(arg1, arg2);
  } else if (!strcmp(cmd, "ddc-config")) {
    RTD266xISP::DDCConfig config;
    if (arg1 || arg2) {
      printError("DDC configuration takes no arguments");
    } else if (!isp->readDDCConfig(config)) {
      printError(isp->error());
    } else {
      Serial.print("{\"ok\":true,\"registers\":{");
      for (size_t i = 0; i < RTD266xISP::DDC_REGISTER_COUNT; i++) {
        if (i) {
          Serial.print(',');
        }
        Serial.print('"');
        Serial.print(RTD266xISP::DDC_REGISTERS[i].name);
        Serial.print("\":\"");
        printHex(&config.values[i], 1);
        Serial.print('"');
      }
      Serial.print("},\"channel_access\":{\"FFEC\":\"");
      printHex(&config.channelAccess[0], 1);
      Serial.print("\",\"FFED\":\"");
      printHex(&config.channelAccess[1], 1);
      Serial.println("\"}}");
    }
  } else if (!strcmp(cmd, "isp")) {
    uint8_t status;
    if (!isp->enter() || !isp->readStatus(status)) {
      printError(isp->error());
    } else {
      Serial.print("{\"ok\":true,\"jedec\":\"");
      Serial.print(isp->jedecId(), HEX);
      Serial.print("\",\"size\":");
      Serial.print(isp->flashSize());
      Serial.print(",\"status\":");
      Serial.print(status);
      Serial.println("}");
    }
  } else if (!strcmp(cmd, "read")) {
    if (!number(arg1, address) || !number(arg2, length) || !length || length > 256) {
      printError("Read requires an address and length 1 through 256");
    } else if (!isp->read(address, data, length)) {
      printError(isp->error());
    } else {
      Serial.print("{\"ok\":true,\"address\":");
      Serial.print(address);
      Serial.print(",\"data\":\"");
      printHex(data, length);
      Serial.println("\"}");
    }
  } else if (!strcmp(cmd, "arm")) {
    if (!arg1 || strlen(arg1) != 6 || strspn(arg1, "0123456789abcdefABCDEF") != 6) {
      printError("Arm requires the six-digit flash JEDEC ID");
    } else {
      printResult(isp->arm(strtoul(arg1, nullptr, 16)));
    }
  } else if (!strcmp(cmd, "unlock")) {
    printResult(isp->unlock());
  } else if (!strcmp(cmd, "restore-protection")) {
    if (!number(arg1, address) || address > 255) {
      printError("restore-protection requires a recorded status byte");
    } else {
      printResult(isp->restoreProtection((uint8_t)address));
    }
  } else if (!strcmp(cmd, "erase")) {
    if (!number(arg1, address)) {
      printError("Erase requires a sector address");
    } else {
      printResult(isp->eraseSector(address));
    }
  } else if (!strcmp(cmd, "page")) {
    size_t bytes = 0;
    if (!number(arg1, address) || !hexBytes(arg2, data, bytes)) {
      printError("Page requires an address and 1 through 256 hex bytes");
    } else {
      printResult(isp->programPage(address, data, bytes));
    }
  } else if (!strcmp(cmd, "finish")) {
    printResult(isp->finish());
  } else if (!strcmp(cmd, "reset")) {
    printResult(isp->reset());
  } else if (!strcmp(cmd, "reset-chip")) {
    printResult(isp->resetChip());
  } else if (!strcmp(cmd, "pattern")) {
    if (!display) {
      printError("Video is off; use mode 640 or mode 800");
    } else if (!arg1 || !drawPattern(arg1)) {
      printError("Unknown pattern");
    } else {
      Serial.println("{\"ok\":true}");
    }
  } else if (!strcmp(cmd, "mode")) {
    if (flash.active() || flash1.active()) {
      printError("Finish the ISP session before changing video mode");
    } else if (!arg1 || (strcmp(arg1, "640") && strcmp(arg1, "800") &&
                        strcmp(arg1, "panel") && strcmp(arg1, "off"))) {
      printError("HSTX mode must be 640, 800, panel or off");
    } else {
      watchdog_hw->scratch[0] = MODE_COOKIE;
      watchdog_hw->scratch[1] = !strcmp(arg1, "panel") ? 800 : atoi(arg1);
      Serial.println("{\"ok\":true,\"rebooting\":true}");
      Serial.flush();
      delay(100);
      rp2040.reboot();
    }
  } else {
    printError("Unknown command");
  }
}

bool drawPattern(const char *name) {
  if (!display) {
    return false;
  }
  int16_t w = display->width();
  int16_t h = display->height();
  const uint16_t colors[] = {0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0};
  if (!strcmp(name, "bars")) {
    for (uint8_t i = 0; i < 8; i++) {
      display->fillRect(i * w / 8, 0, (i + 1) * w / 8 - i * w / 8, h, colors[i]);
    }
  } else if (!strcmp(name, "checker")) {
    for (int16_t y = 0; y < h; y += 8) {
      for (int16_t x = 0; x < w; x += 8) {
        uint16_t color = 0;
        if ((x / 8 + y / 8) % 2) {
          color = 0xFFFF;
        }
        display->fillRect(x, y, 8, 8, color);
      }
    }
  } else if (!strcmp(name, "gray")) {
    for (int16_t x = 0; x < w; x++) {
      uint8_t shade = map(x, 0, w - 1, 0, 255);
      display->drawFastVLine(x, 0, h, (uint16_t)((shade / 8) * 2048 + (shade / 4) * 32 + shade / 8));
    }
  } else if (!strcmp(name, "grid")) {
    display->fillScreen(0);
    for (int16_t x = 0; x < w; x += 16) {
      display->drawFastVLine(x, 0, h, 0xFFFF);
    }
    for (int16_t y = 0; y < h; y += 16) {
      display->drawFastHLine(0, y, w, 0xFFFF);
    }
    display->drawRect(0, 0, w, h, 0xF800);
  } else if (!strcmp(name, "text")) {
    display->fillScreen(0);
    display->setTextColor(0xFFFF);
    display->setTextSize(2);
    display->setCursor(20, 50);
    display->println("Feather HSTX / RTD");
    display->setCursor(20, 90);
    display->print(videoWidth);
    display->println(" x 480 @ 60 Hz");
    display->setCursor(20, 130);
    display->println("48 kHz / 1 kHz tone");
  } else if (!strcmp(name, "red")) {
    display->fillScreen(0xF800);
  } else if (!strcmp(name, "green")) {
    display->fillScreen(0x07E0);
  } else if (!strcmp(name, "blue")) {
    display->fillScreen(0x001F);
  } else if (!strcmp(name, "white")) {
    display->fillScreen(0xFFFF);
  } else if (!strcmp(name, "black")) {
    display->fillScreen(0);
  } else {
    return false;
  }
  return true;
}
