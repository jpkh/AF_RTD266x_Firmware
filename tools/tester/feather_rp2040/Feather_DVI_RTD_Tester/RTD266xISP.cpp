// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Adafruit Industries
// Register sequences: Realtek RTD2660 series rev 1.00, pp279, 311-317, 350.
// Flash commands: Winbond W25X10/20/40/80, status and instruction sections.
// The old programmers are useful protocol references, but their polling of
// FF6F bit 6 is not the documented program-busy flag. We check bit 5 and WIP.

#include "RTD266xISP.h"
#include <string.h>

/** Construct a programmer. The caller initializes Wire and its timeout. */
RTD266xISP::RTD266xISP(TwoWire &wire) : _wire(wire) {}

bool RTD266xISP::fail(const char *message) {
  _error = message;
  return false;
}

bool RTD266xISP::requireActive() {
  if (!_active || !_identified) {
    return fail("Enter ISP and identify a supported flash first");
  }
  return true;
}

bool RTD266xISP::requireArmed() {
  if (!requireActive()) {
    return false;
  }
  if (!_armed) {
    return fail("Flash writing is not armed");
  }
  return true;
}

bool RTD266xISP::validRange(uint32_t address, size_t length) {
  if (length == 0 || address >= _size || length > _size - address) {
    return fail("Flash address or length is out of range");
  }
  return true;
}

const RTD266xISP::FlashProfile *RTD266xISP::findProfile(uint32_t jedec) const {
  for (size_t i = 0; i < FLASH_PROFILE_COUNT; i++) {
    if (FLASH_PROFILES[i].jedec == jedec) {
      return &FLASH_PROFILES[i];
    }
  }
  return nullptr;
}

// Address 0x4A keeps the register address fixed, required for the data FIFO.
// Raw transfers also let us reject every NACK and short read, including polls.
bool RTD266xISP::writeBytes(uint8_t reg, const uint8_t *data, size_t length) {
  _wire.beginTransmission(ISP_ADDRESS);
  size_t written = _wire.write(reg);
  written += _wire.write(data, length);
  uint8_t status = _wire.endTransmission();
  if (status != 0 || written != length + 1) {
    return fail("RTD I2C write failed");
  }
  return true;
}

bool RTD266xISP::readBytes(uint8_t reg, uint8_t *data, size_t length) {
  _wire.beginTransmission(ISP_ADDRESS);
  size_t written = _wire.write(reg);
  uint8_t status = _wire.endTransmission(false);
  if (status != 0 || written != 1) {
    return fail("RTD I2C register selection failed");
  }
  size_t received = _wire.requestFrom(ISP_ADDRESS, (uint8_t)length);
  if (received != length) {
    while (_wire.available()) {
      _wire.read();
    }
    return fail("RTD I2C short read");
  }
  for (size_t i = 0; i < length; i++) {
    int value = _wire.read();
    if (value < 0) {
      return fail("RTD I2C receive buffer empty");
    }
    data[i] = (uint8_t)value;
  }
  return true;
}

bool RTD266xISP::writeRegister(uint8_t reg, uint8_t value) {
  return writeBytes(reg, &value, 1);
}

bool RTD266xISP::readRegister(uint8_t reg, uint8_t &value) {
  return readBytes(reg, &value, 1);
}

bool RTD266xISP::writeAddress(uint8_t firstRegister, uint32_t address) {
  // SPI flash addresses are transmitted high byte first, over three registers.
  return writeRegister(firstRegister, (uint8_t)(address >> 16)) &&
         writeRegister(firstRegister + 1, (uint8_t)(address >> 8)) &&
         writeRegister(firstRegister + 2, (uint8_t)address);
}

bool RTD266xISP::commonCommand(Instruction instruction, FlashCommand opcode,
                               uint8_t readCount, uint8_t writeCount,
                               uint32_t argument, uint32_t &result,
                               uint32_t timeoutMs) {
  CommonControl command = {};
  command.bits.instruction = instruction;
  command.bits.readCount = readCount;
  command.bits.writeCount = writeCount;
  // Clear execute before selecting an opcode and loading its argument.
  if (!writeRegister(COMMON_CONTROL, command.value) ||
      !writeRegister(COMMON_OPCODE, opcode)) {
    return false;
  }
  if (opcode == READ_DATA && !writeAddress(ADDRESS_HIGH, argument)) {
    return false;
  }
  for (uint8_t i = 0; i < writeCount; i++) {
    uint8_t value = (uint8_t)(argument >> (8 * (writeCount - i - 1)));
    if (!writeRegister(ADDRESS_HIGH + i, value)) {
      return false;
    }
  }
  command.bits.execute = true;
  if (!writeRegister(COMMON_CONTROL, command.value)) {
    return false;
  }
  uint32_t started = millis();
  do {
    if (!readRegister(COMMON_CONTROL, command.value)) {
      return false;
    }
    if (!command.bits.execute) {
      result = 0;
      if (opcode == READ_DATA) {
        return true; // Sequential reads consume DATA_PORT, not result slots.
      }
      for (uint8_t i = 0; i < readCount; i++) {
        uint8_t value;
        if (!readRegister(RESULT_HIGH + i, value)) {
          return false;
        }
        result = (result << 8) | value;
      }
      return true;
    }
    delay(1);
  } while (millis() - started < timeoutMs);
  return fail("RTD SPI command timed out");
}

/** Enter ISP without altering flash protection or flash contents. */
bool RTD266xISP::enter() {
  _error = "";
  if (_active && _identified) {
    return true;
  }
  ProgramControl control = {};
  control.bits.ispEnabled = true;
  if (!writeRegister(PROGRAM_CONTROL, control.value)) {
    return false;
  }
  _active = true;
  if (!readRegister(PROGRAM_CONTROL, control.value)) {
    return false;
  }
  if (!control.bits.ispEnabled) {
    return fail("RTD did not enter ISP");
  }
  if (!commonCommand(SPI_READ, JEDEC_ID, 3, 0, 0, _jedec)) {
    return false;
  }
  _size = 0;
  const FlashProfile *profile = findProfile(_jedec);
  if (!profile) {
    return fail("Unsupported flash; enabled: W25X40 EF3013, ZD25Q40 5E6013");
  }
  _size = profile->size;
  // Select the profile opcodes in the RTD controller, not in flash storage.
  if (!writeRegister(WREN_OPCODE, WRITE_ENABLE) ||
      !writeRegister(READ_OPCODE, READ_DATA) ||
      !writeRegister(PROGRAM_OPCODE, PAGE_PROGRAM) ||
      !writeRegister(STATUS_OPCODE, READ_STATUS)) {
    return false;
  }
  if (!_statusSaved) {
    if (!readStatus(_originalStatus)) {
      return false;
    }
    FlashStatus status = {};
    status.value = _originalStatus;
    if (status.bits.busy) {
      return fail("Flash was already busy on ISP entry");
    }
    _statusSaved = true;
  }
  _identified = true;
  return true;
}

/** Read actual ISP state, including a session predating this Feather boot. */
bool RTD266xISP::readISPState(bool &enabled) {
  _error = "";
  ProgramControl control = {};
  if (!readRegister(PROGRAM_CONTROL, control.value)) {
    return false;
  }
  enabled = control.bits.ispEnabled;
  return true;
}

/**
 * Read fixed DDC configuration registers while ISP has halted the MCU.
 * RTD2660 p316 requires ISP for all external registers except FF6F. This
 * method never enters/exits ISP, changes register values, or accesses FIFO,
 * scaler index/data, DDC index/data, or flash command ports.
 * @param config Snapshot, valid only if the complete read succeeds.
 * @return true when every whitelisted byte and the EC/ED pair was read.
 */
bool RTD266xISP::readDDCConfig(DDCConfig &config) {
  _error = "";
  if (!requireActive()) {
    return false;
  }
  bool enabled;
  if (!readISPState(enabled)) {
    return false;
  }
  if (!enabled) {
    return fail("Enter ISP before reading DDC configuration");
  }
  DDCConfig snapshot = {};
  for (size_t i = 0; i < DDC_REGISTER_COUNT; i++) {
    if (!readRegister(DDC_REGISTERS[i].address, snapshot.values[i])) {
      return false;
    }
  }
  // One EC/ED read keeps the access flags associated with the same DDC
  // transaction: EC bit 1 = DDC1, EC bit 0 = DDC2, ED bit 6 = DDC3.
  _wire.beginTransmission(ISP_INCREMENT_ADDRESS);
  size_t written = _wire.write(ISP_SLAVE_ADDRESS_REGISTER);
  uint8_t status = _wire.endTransmission(false);
  if (status != 0 || written != 1) {
    return fail("DDC channel register selection failed");
  }
  if (_wire.requestFrom(ISP_INCREMENT_ADDRESS, (uint8_t)2) != 2) {
    while (_wire.available()) {
      _wire.read();
    }
    return fail("DDC channel snapshot short read");
  }
  for (size_t i = 0; i < 2; i++) {
    int value = _wire.read();
    if (value < 0) {
      return fail("DDC channel snapshot receive buffer empty");
    }
    snapshot.channelAccess[i] = (uint8_t)value;
  }
  config = snapshot;
  return true;
}

/** Read the flash's single status byte; reads never unlock the flash. */
bool RTD266xISP::readStatus(uint8_t &value) {
  _error = "";
  if (!_active) {
    return fail("ISP is not active");
  }
  uint32_t result;
  if (!commonCommand(SPI_READ, READ_STATUS, 1, 0, 0, result)) {
    return false;
  }
  value = (uint8_t)result;
  return true;
}

bool RTD266xISP::waitProgramReady(uint32_t timeoutMs) {
  uint32_t started = millis();
  do {
    ProgramControl control = {};
    if (!readRegister(PROGRAM_CONTROL, control.value)) {
      return false;
    }
    if (!control.bits.ispEnabled) {
      return fail("RTD unexpectedly left ISP");
    }
    if (!control.bits.programming) {
      return true;
    }
    delay(1);
  } while (millis() - started < timeoutMs);
  return fail("RTD page programming timed out");
}

bool RTD266xISP::waitFlashReady(uint32_t timeoutMs) {
  uint32_t started = millis();
  do {
    FlashStatus status = {};
    if (!readStatus(status.value)) {
      return false;
    }
    if (!status.bits.busy) {
      return true;
    }
    delay(1);
  } while (millis() - started < timeoutMs);
  return fail("Flash remained busy");
}

bool RTD266xISP::checkReadCRC(uint32_t address, const uint8_t *data,
                              size_t length) {
  uint8_t expected = 0;
  // A CRC polynomial is arithmetic on bits, not a device register field.
  for (size_t i = 0; i < length; i++) {
    expected ^= data[i];
    for (uint8_t bit = 0; bit < 8; bit++) {
      if (expected & 0x80) {
        expected = (expected << 1) ^ CRC_POLYNOMIAL;
      } else {
        expected <<= 1;
      }
    }
  }
  if (!writeAddress(ADDRESS_HIGH, address) ||
      !writeAddress(CRC_END_HIGH, address + length - 1)) {
    return false;
  }
  ProgramControl control = {};
  control.bits.ispEnabled = true;
  control.bits.crcStart = true;
  if (!writeRegister(PROGRAM_CONTROL, control.value)) {
    return false;
  }
  uint32_t started = millis();
  do {
    if (!readRegister(PROGRAM_CONTROL, control.value)) {
      return false;
    }
    if (control.bits.crcDone) {
      uint8_t actual;
      if (!readRegister(CRC_RESULT, actual)) {
        return false;
      }
      if (actual != expected) {
        return fail("Flash read CRC does not match RTD hardware");
      }
      return true;
    }
    delay(1);
  } while (millis() - started < COMMAND_TIMEOUT_MS);
  return fail("RTD read CRC timed out");
}

/** Read 1-256 bytes and compare against the RTD's independent flash CRC. */
bool RTD266xISP::read(uint32_t address, uint8_t *data, size_t length) {
  _error = "";
  if (!requireActive() || !validRange(address, length)) {
    return false;
  }
  if (!data || length > PAGE_SIZE) {
    return fail("Read requires a buffer and 1-256 bytes");
  }
  if (!waitProgramReady(PROGRAM_TIMEOUT_MS) ||
      !waitFlashReady(PROGRAM_TIMEOUT_MS)) {
    return false;
  }
  // READ03 starts a sequential stream at the address registers. The three
  // read-result slots select the documented stream/FIFO path in ISP mode.
  uint32_t ignored;
  if (!commonCommand(SPI_READ, READ_DATA, 3, 0, address, ignored)) {
    return false;
  }
  for (size_t offset = 0; offset < length; offset += WIRE_CHUNK) {
    size_t count = length - offset;
    if (count > WIRE_CHUNK) {
      count = WIRE_CHUNK;
    }
    if (!readBytes(DATA_PORT, data + offset, count)) {
      return false;
    }
  }
  return checkReadCRC(address, data, length);
}

/** Allow writes only when the host names the detected supported JEDEC ID. */
bool RTD266xISP::arm(uint32_t expectedJedec) {
  _error = "";
  if (!requireActive()) {
    return false;
  }
  if (expectedJedec != _jedec || !findProfile(expectedJedec)) {
    return fail("Arming JEDEC ID does not match the supported flash");
  }
  _armed = true;
  return true;
}

bool RTD266xISP::writeEnable() {
  uint32_t ignored;
  if (!waitFlashReady(PROGRAM_TIMEOUT_MS) ||
      !commonCommand(SPI_WRITE, WRITE_ENABLE, 0, 0, 0, ignored)) {
    return false;
  }
  FlashStatus status = {};
  if (!readStatus(status.value)) {
    return false;
  }
  if (!status.bits.writeEnabled) {
    return fail("Flash write-enable latch did not set");
  }
  return true;
}

bool RTD266xISP::statusMatches(uint8_t first, uint8_t second) const {
  FlashStatus a = {}, b = {};
  a.value = first;
  b.value = second;
  a.bits.busy = b.bits.busy = false;
  a.bits.writeEnabled = b.bits.writeEnabled = false;
  return a.value == b.value;
}

/**
 * @brief Restore BP2:BP0 after the programmer loses its saved RAM session.
 *
 * First verify a complete image and use the protection byte from its receipt.
 * This requires arming and stays in ISP. Call finish() to release the image.
 *
 * @param originalStatus Recorded W25X40 status, without BUSY/WEL.
 * @return True when nonzero block protection is restored and read back.
 */
bool RTD266xISP::restoreProtection(uint8_t originalStatus) {
  _error = "";
  if (!requireArmed() || !waitProgramReady(PROGRAM_TIMEOUT_MS) ||
      !waitFlashReady(ERASE_TIMEOUT_MS)) {
    return false;
  }
  FlashStatus requested = {}, current = {};
  requested.value = originalStatus;
  if (requested.bits.busy || requested.bits.writeEnabled ||
      requested.bits.reserved || !requested.bits.blockProtection) {
    return fail(
        "Recovery requires a nonzero protection value without BUSY/WEL");
  }
  if (!readStatus(current.value)) {
    return false;
  }
  if (requested.bits.bottomProtection != current.bits.bottomProtection ||
      requested.bits.statusProtection != current.bits.statusProtection) {
    return fail("Recovery cannot change protection direction or status lock");
  }
  // Keep the intended value even if its write loses an acknowledgement, so
  // finish() retries that protection rather than restoring the unprotected
  // byte.
  _originalStatus = originalStatus;
  _statusSaved = true;
  if (!writeStatus(originalStatus)) {
    return false;
  }
  _statusChanged = false;
  return true;
}

bool RTD266xISP::writeStatus(uint8_t value) {
  uint32_t ignored;
  if (!writeEnable()) {
    return false;
  }
  // Track an attempted change before issuing it: a lost ACK can follow a write.
  _statusChanged = true;
  if (!commonCommand(SPI_WRITE, WRITE_STATUS, 0, 1, value, ignored) ||
      !waitFlashReady(PROGRAM_TIMEOUT_MS)) {
    return false;
  }
  uint8_t actual;
  if (!readStatus(actual)) {
    return false;
  }
  if (!statusMatches(actual, value)) {
    return fail("Flash protection status did not verify");
  }
  return true;
}

/** Explicitly clear BP2:BP0; retain the saved original status for finish(). */
bool RTD266xISP::unlock() {
  _error = "";
  if (!requireArmed() || !waitFlashReady(PROGRAM_TIMEOUT_MS)) {
    return false;
  }
  FlashStatus status = {};
  if (!readStatus(status.value)) {
    return false;
  }
  if (status.bits.blockProtection == 0) {
    return true;
  }
  status.bits.blockProtection = 0;
  status.bits.busy = false;
  status.bits.writeEnabled = false;
  return writeStatus(status.value);
}

/** Erase and read-verify one aligned 4 KiB sector, without unlocking it. */
bool RTD266xISP::eraseSector(uint32_t address) {
  _error = "";
  if (!requireArmed() || !validRange(address, SECTOR_SIZE)) {
    return false;
  }
  if (address % SECTOR_SIZE != 0) {
    return fail("Sector erase address must be aligned to 4096 bytes");
  }
  if (!waitProgramReady(PROGRAM_TIMEOUT_MS) || !writeEnable()) {
    return false;
  }
  uint32_t ignored;
  // RTD's erase instruction sends WREN, the sector opcode and three address
  // bytes. Its completion and the flash's WIP bit are independently checked.
  if (!commonCommand(SPI_ERASE, SECTOR_ERASE, 0, 3, address, ignored,
                     ERASE_TIMEOUT_MS) ||
      !waitFlashReady(ERASE_TIMEOUT_MS)) {
    return false;
  }
  uint8_t buffer[PAGE_SIZE];
  for (size_t offset = 0; offset < SECTOR_SIZE; offset += sizeof(buffer)) {
    if (!read(address + offset, buffer, sizeof(buffer))) {
      return false;
    }
    for (size_t i = 0; i < sizeof(buffer); i++) {
      if (buffer[i] != 0xFF) {
        return fail("Sector erase verification failed");
      }
    }
  }
  return true;
}

/** Program and read-verify 1-256 bytes, never crossing a 256-byte page. */
bool RTD266xISP::programPage(uint32_t address, const uint8_t *data,
                             size_t length) {
  _error = "";
  if (!requireArmed() || !validRange(address, length)) {
    return false;
  }
  if (!data || length > PAGE_SIZE || address % PAGE_SIZE + length > PAGE_SIZE) {
    return fail("Program must fit in one 256-byte flash page");
  }
  uint8_t check[PAGE_SIZE];
  if (!read(address, check, length)) {
    return false;
  }
  for (size_t i = 0; i < length; i++) {
    // NOR programming can clear bits; setting a bit needs a sector erase.
    if ((check[i] & data[i]) != data[i]) {
      return fail("Page needs a sector erase before programming");
    }
  }
  if (!writeEnable() || !writeRegister(PROGRAM_LENGTH, (uint8_t)(length - 1)) ||
      !writeAddress(ADDRESS_HIGH, address)) {
    return false;
  }
  // Fill the SRAM FIFO before triggering the RTD's page-program engine.
  for (size_t offset = 0; offset < length; offset += WIRE_CHUNK) {
    size_t count = length - offset;
    if (count > WIRE_CHUNK) {
      count = WIRE_CHUNK;
    }
    if (!writeBytes(DATA_PORT, data + offset, count)) {
      return false;
    }
  }
  ProgramControl control = {};
  control.bits.ispEnabled = true;
  control.bits.programming = true;
  if (!writeRegister(PROGRAM_CONTROL, control.value) ||
      !waitProgramReady(PROGRAM_TIMEOUT_MS) ||
      !waitFlashReady(PROGRAM_TIMEOUT_MS) || !read(address, check, length)) {
    return false;
  }
  if (memcmp(data, check, length) != 0) {
    return fail("Page program verification failed");
  }
  return true;
}

/** Restore original protection, disarm, and reboot by leaving ISP. */
bool RTD266xISP::finish() {
  _error = "";
  if (!requireActive() || !waitProgramReady(PROGRAM_TIMEOUT_MS) ||
      !waitFlashReady(ERASE_TIMEOUT_MS)) {
    return false;
  }
  uint8_t current;
  if (!readStatus(current)) {
    return false;
  }
  if (_statusSaved &&
      (_statusChanged || !statusMatches(current, _originalStatus))) {
    if (!writeStatus(_originalStatus)) {
      return false; // Remain in ISP and armed so recovery is still possible.
    }
    _statusChanged = false;
  }
  uint32_t ignored;
  if (!commonCommand(SPI_WRITE, WRITE_DISABLE, 0, 0, 0, ignored)) {
    return false;
  }
  FlashStatus finalStatus = {};
  if (!readStatus(finalStatus.value) || finalStatus.bits.busy ||
      finalStatus.bits.writeEnabled ||
      !statusMatches(finalStatus.value, _originalStatus)) {
    return fail("Original flash protection was not restored");
  }
  _armed = false;
  return leave();
}

/** Exit ISP only when disarmed. ISP exit resets DW8051 (datasheet p279). */
bool RTD266xISP::leave() {
  _error = "";
  if (_armed || _statusChanged) {
    return fail("Use finish to restore protection before leaving ISP");
  }
  if (!_active) {
    return true;
  }
  if (_identified && (!waitProgramReady(PROGRAM_TIMEOUT_MS) ||
                      !waitFlashReady(ERASE_TIMEOUT_MS))) {
    return false;
  }
  // FF6F bit 0 resets the flash controller; bit 7 keeps the MCU halted in ISP
  // (manual p316). Keep ISP asserted until controller reset is released and
  // confirmed. The two-second hold comes from the floppes programmer; it is
  // not a documented silicon minimum. Do not release ISP with reset held.
  ProgramControl control = {};
  control.bits.ispEnabled = true;
  control.bits.resetFlashController = true;
  if (!writeRegister(PROGRAM_CONTROL, control.value)) {
    return false;
  }
  delay(RESET_HOLD_MS);
  control.bits.resetFlashController = false;
  if (!writeRegister(PROGRAM_CONTROL, control.value)) {
    return false;
  }
  if (!readRegister(PROGRAM_CONTROL, control.value)) {
    return false;
  }
  if (!control.bits.ispEnabled || control.bits.resetFlashController) {
    return fail(
        "Flash controller reset release while halted was not confirmed");
  }
  // Controller reset follows finish()'s earlier WRDI. Check the flash itself
  // again at the final handoff, while the MCU is still held in reset. WEL is
  // intentionally ignored by statusMatches(), so test it explicitly here.
  if (_identified) {
    uint32_t ignored;
    FlashStatus status = {};
    if (!commonCommand(SPI_WRITE, WRITE_DISABLE, 0, 0, 0, ignored) ||
        !readStatus(status.value)) {
      return false;
    }
    if (status.bits.busy || status.bits.writeEnabled ||
        (_statusSaved && !statusMatches(status.value, _originalStatus))) {
      return fail(
          "Flash protection or write-disable was not safe for ISP exit");
    }
  }
  control.value = 0;
  if (!writeRegister(PROGRAM_CONTROL, control.value)) {
    return false;
  }
  // FF6F remains readable after ISP exit; no other external register does.
  if (!readRegister(PROGRAM_CONTROL, control.value)) {
    return false;
  }
  if (control.bits.ispEnabled || control.bits.resetFlashController) {
    return fail("ISP exit or flash controller reset release was not confirmed");
  }
  _active = false;
  _identified = false;
  _statusSaved = false;
  return true;
}

/** Reboot the RTD through an ISP enter/exit; never reset an armed session. */
bool RTD266xISP::reset() {
  _error = "";
  if (_armed || _statusChanged) {
    return fail("Finish the armed session before resetting");
  }
  if (!enter()) {
    return false;
  }
  return leave();
}

/** Request SOF_RST, the whole-chip software reset documented on p279. */
bool RTD266xISP::resetChip() {
  _error = "";
  if (_armed || _statusChanged) {
    return fail("Finish the armed session before resetting");
  }
  if (!enter() || !waitProgramReady(PROGRAM_TIMEOUT_MS) ||
      !waitFlashReady(ERASE_TIMEOUT_MS)) {
    return false;
  }
  uint32_t ignored;
  FlashStatus status = {};
  if (!commonCommand(SPI_WRITE, WRITE_DISABLE, 0, 0, 0, ignored) ||
      !readStatus(status.value)) {
    return false;
  }
  if (!statusMatches(status.value, _originalStatus) || status.bits.busy ||
      status.bits.writeEnabled) {
    return fail("Flash protection or idle state is not safe for reset");
  }
  uint8_t clockControl;
  if (!readRegister(MCU_CLOCK_CONTROL, clockControl) ||
      !writeRegister(MCU_CLOCK_CONTROL, clockControl | WHOLE_CHIP_RESET)) {
    return false;
  }
  // The manual specifies neither self-clearing nor a minimum pulse width.
  // DDC can disappear temporarily. Retry only FF6F reads, never the reset
  // write, and confirm ISP before accessing any other external register.
  delay(100);
  ProgramControl control = {};
  uint32_t started = millis();
  while (!readRegister(PROGRAM_CONTROL, control.value)) {
    if (millis() - started >= RESET_READY_TIMEOUT_MS) {
      // Preserve session state when reset completion cannot be confirmed.
      return fail(
          "Whole-chip reset sent; DDC readiness timed out, not retried");
    }
    delay(20);
  }
  _error = ""; // Discard transient read errors after DDC responds.
  if (control.bits.ispEnabled) {
    if (!readRegister(MCU_CLOCK_CONTROL, clockControl) ||
        !writeRegister(MCU_CLOCK_CONTROL, clockControl & ~WHOLE_CHIP_RESET)) {
      return false;
    }
    return leave();
  }
  if (control.bits.resetFlashController) {
    return fail("Whole-chip reset left the flash controller in reset");
  }
  _active = false;
  _identified = false;
  _statusSaved = false;
  return true;
}

/** Return the last identified flash ID. */
uint32_t RTD266xISP::jedecId() const { return _jedec; }
/** Return supported flash size in bytes, or zero for an unknown part. */
uint32_t RTD266xISP::flashSize() const { return _size; }
/** Return whether ISP is active, including an incomplete entry. */
bool RTD266xISP::active() const { return _active; }
/** Return whether the host has explicitly armed writing. */
bool RTD266xISP::armed() const { return _armed; }
/** Return the error from the last attempted operation. */
const char *RTD266xISP::error() const { return _error; }
