// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Adafruit Industries
// Independent implementation of the RTD2660 register interface, revision 1.00.

#ifndef RTD266X_ISP_H
#define RTD266X_ISP_H

#include <Arduino.h>
#include <Wire.h>

/** Checked HDMI DDC access to an RTD2660 and its SPI flash. */
class RTD266xISP {
public:
  /** Fixed readable configuration register; no index or data ports. */
  struct DDCRegister {
    const char *name; ///< Register name in the RTD2660 manual.
    uint8_t address;  ///< Low byte of the FFxx external register address.
  };
  /** RTD2660 external register whitelist for a DDC configuration snapshot. */
  static constexpr DDCRegister DDC_REGISTERS[] = {
      {"DDC_RAM_PARTITION", 0x21},
      {"DDC1_CONTROL0", 0x1B},
      {"DDC1_CONTROL1", 0x1C},
      {"DDC1_CONTROL2", 0x1D},
      {"DDC2_CONTROL0", 0x1E},
      {"DDC2_CONTROL1", 0x1F},
      {"DDC2_CONTROL2", 0x20},
      {"DDC3_CONTROL0", 0x2C},
      {"DDC3_CONTROL1", 0x2D},
      {"DDC3_CONTROL2", 0x2E},
      {"PIN_SHARE_CONTROL14", 0xA4},
      {"WDT_CONTROL", 0xEA},
      {"ISP_SLAVE_ADDRESS", 0xEC},
      {"ISP_MCU_CONTROL", 0xED},
      {"ISP_MCU_CLOCK_CONTROL", 0xEE},
      {"BANK_CONTROL", 0xFC},
      {"BANK_XDATA_START", 0xFD},
      {"BANK_XDATA_SELECT", 0xFE},
      {"BANK_PBANK_SWITCH", 0xFF},
      {"REV_DUMMY2", 0x19},
      {"REV_DUMMY6", 0xF2},
  };
  /** Number of fixed registers in a snapshot. */
  static constexpr size_t DDC_REGISTER_COUNT =
      sizeof(DDC_REGISTERS) / sizeof(DDC_REGISTERS[0]);
  /** Sequential configuration reads plus one two-byte channel-access read. */
  struct DDCConfig {
    uint8_t values[DDC_REGISTER_COUNT]; ///< Values in DDC_REGISTERS order.
    uint8_t channelAccess[2]; ///< FFEC and FFED from one auto-increment read.
  };

  explicit RTD266xISP(TwoWire &wire = Wire);
  bool enter();
  bool leave();
  bool reset();
  bool resetChip();
  uint32_t jedecId() const;
  uint32_t flashSize() const;
  bool readStatus(uint8_t &value);
  bool readISPState(bool &enabled);
  bool readDDCConfig(DDCConfig &config);
  bool read(uint32_t address, uint8_t *data, size_t length);
  bool arm(uint32_t expectedJedec);
  bool unlock();
  bool restoreProtection(uint8_t originalStatus);
  bool eraseSector(uint32_t address);
  bool programPage(uint32_t address, const uint8_t *data, size_t length);
  bool finish();
  bool active() const;
  bool armed() const;
  const char *error() const;

private:
  /** ISP address with register auto-increment disabled (datasheet p350). */
  static constexpr uint8_t ISP_ADDRESS = 0x4A;
  /** Auto-increment ISP address, used only for the fixed EC/ED snapshot. */
  static constexpr uint8_t ISP_INCREMENT_ADDRESS = 0x4B;
  static constexpr uint8_t ISP_SLAVE_ADDRESS_REGISTER = 0xEC;
  static constexpr size_t PAGE_SIZE = 256;
  static constexpr size_t SECTOR_SIZE = 4096;
  static constexpr size_t WIRE_CHUNK = 16;
  static constexpr uint32_t COMMAND_TIMEOUT_MS = 500;
  static constexpr uint32_t PROGRAM_TIMEOUT_MS = 1000;
  static constexpr uint32_t ERASE_TIMEOUT_MS = 5000;
  /** Established programmer delay, not a documented silicon minimum. */
  static constexpr uint32_t RESET_HOLD_MS = 2000;
  /** Host readiness bound after whole-chip reset, not a silicon timing spec. */
  static constexpr uint32_t RESET_READY_TIMEOUT_MS = 3000;
  /** FFEE bit 1 resets the whole chip; preserve all other clock controls. */
  static constexpr uint8_t WHOLE_CHIP_RESET = 0x02;
  /** CRC-8, x^8 + x^2 + x + 1, initial zero, no reflection or final XOR. */
  static constexpr uint8_t CRC_POLYNOMIAL = 0x07;

  /** RTD external function registers, low byte at ISP address 0x4A. */
  enum Register : uint8_t {
    COMMON_CONTROL = 0x60,
    COMMON_OPCODE = 0x61,
    WREN_OPCODE = 0x62,
    ADDRESS_HIGH = 0x64,
    ADDRESS_MIDDLE = 0x65,
    ADDRESS_LOW = 0x66,
    RESULT_HIGH = 0x67,
    READ_OPCODE = 0x6A,
    PROGRAM_OPCODE = 0x6D,
    STATUS_OPCODE = 0x6E,
    PROGRAM_CONTROL = 0x6F,
    DATA_PORT = 0x70,
    PROGRAM_LENGTH = 0x71,
    CRC_END_HIGH = 0x72,
    CRC_RESULT = 0x75,
    MCU_CLOCK_CONTROL = 0xEE,
  };

  /** Common SPI instruction types from RTD datasheet p311. */
  enum Instruction : uint8_t {
    SPI_WRITE = 1,
    SPI_READ = 2,
    SPI_ERASE = 5,
  };

  /** Enabled flash profiles; each has ONE status register and standard SPI. */
  struct FlashProfile {
    uint32_t jedec;    ///< Three-byte JEDEC ID read through the RTD.
    const char *name;  ///< Human-readable part name.
    uint32_t size;     ///< Capacity in bytes.
  };
  static constexpr FlashProfile FLASH_PROFILES[] = {
      {0xEF3013, "W25X40", 512 * 1024},
      {0x5E6013, "ZD25Q40", 512 * 1024},
  };
  static constexpr size_t FLASH_PROFILE_COUNT =
      sizeof(FLASH_PROFILES) / sizeof(FLASH_PROFILES[0]);

  /** W25X40/ZD25Q40 instructions; both profiles share this command set. */
  enum FlashCommand : uint8_t {
    WRITE_STATUS = 0x01,
    PAGE_PROGRAM = 0x02,
    READ_DATA = 0x03,
    WRITE_DISABLE = 0x04,
    READ_STATUS = 0x05,
    WRITE_ENABLE = 0x06,
    SECTOR_ERASE = 0x20,
    JEDEC_ID = 0x9F,
  };

  /** Atomic command snapshot: bit fields follow RP2040 GCC byte ordering. */
  union CommonControl {
    uint8_t value;
    struct {
      uint8_t execute : 1;
      uint8_t readCount : 2;
      uint8_t writeCount : 2;
      uint8_t instruction : 3;
    } bits;
  };

  /** Checked snapshot of FF6F; bit 5, NOT bit 6, indicates programming. */
  union ProgramControl {
    uint8_t value;
    struct {
      uint8_t resetFlashController : 1;
      uint8_t crcDone : 1;
      uint8_t crcStart : 1;
      uint8_t dummy : 1;
      uint8_t bufferWritable : 1;
      uint8_t programming : 1;
      uint8_t aaiMode : 1;
      uint8_t ispEnabled : 1;
    } bits;
  };

  /** Status register shared by both enabled profiles; bit 6 is reserved. */
  union FlashStatus {
    uint8_t value;
    struct {
      uint8_t busy : 1;
      uint8_t writeEnabled : 1;
      uint8_t blockProtection : 3;
      uint8_t bottomProtection : 1;
      uint8_t reserved : 1;
      uint8_t statusProtection : 1;
    } bits;
  };

  static_assert(sizeof(CommonControl) == 1,
                "Command snapshot must be one byte");
  static_assert(sizeof(ProgramControl) == 1, "ISP snapshot must be one byte");
  static_assert(sizeof(FlashStatus) == 1, "Status snapshot must be one byte");

  TwoWire &_wire;
  bool _active = false;
  bool _identified = false;
  bool _armed = false;
  bool _statusSaved = false;
  bool _statusChanged = false;
  uint8_t _originalStatus = 0;
  uint32_t _jedec = 0;
  uint32_t _size = 0;
  const char *_error = "";

  bool fail(const char *message);
  bool requireActive();
  bool requireArmed();
  bool validRange(uint32_t address, size_t length);
  const FlashProfile *findProfile(uint32_t jedec) const;
  bool writeRegister(uint8_t reg, uint8_t value);
  bool readRegister(uint8_t reg, uint8_t &value);
  bool writeBytes(uint8_t reg, const uint8_t *data, size_t length);
  bool readBytes(uint8_t reg, uint8_t *data, size_t length);
  bool writeAddress(uint8_t firstRegister, uint32_t address);
  bool commonCommand(Instruction instruction, FlashCommand opcode,
                     uint8_t readCount, uint8_t writeCount, uint32_t argument,
                     uint32_t &result, uint32_t timeoutMs = COMMAND_TIMEOUT_MS);
  bool waitProgramReady(uint32_t timeoutMs);
  bool waitFlashReady(uint32_t timeoutMs);
  bool writeEnable();
  bool writeStatus(uint8_t value);
  bool checkReadCRC(uint32_t address, const uint8_t *data, size_t length);
  bool statusMatches(uint8_t first, uint8_t second) const;
};

#endif
