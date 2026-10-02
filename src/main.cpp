#define BUFFER_LENGTH 1024

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_NeoPixel.h>

// Pins are set per board in platformio.ini (build_flags), defaults match the LOLIN S2 Mini
#ifndef BUTTON_PIN
#define BUTTON_PIN 9
#endif
// Level the button pin reads while pressed: HIGH for a button to 3.3V with a pull-down,
// LOW for a button to GND (the internal pull-up is used then)
#ifndef BUTTON_ACTIVE_LEVEL
#define BUTTON_ACTIVE_LEVEL HIGH
#endif
#ifndef LED_PIN
#define LED_PIN 11
#endif
#ifndef I2C_SDA_PIN
#define I2C_SDA_PIN 33
#endif
#ifndef I2C_SCL_PIN
#define I2C_SCL_PIN 35
#endif
#define NUM_PIXELS 1

Adafruit_NeoPixel pixels(NUM_PIXELS, LED_PIN, NEO_GRB + NEO_KHZ800);

// I2C addresses for the toner cartridges
const uint16_t BLACK_CHIP_ADDRESS = 0x001;
const uint16_t CYAN_CHIP_ADDRESS = 0x002;
const uint16_t MAGENTA_CHIP_ADDRESS = 0x003;
const uint16_t YELLOW_CHIP_ADDRESS = 0x004;

// Register addresses
const uint8_t REG_ADDR_1 = 0x01;
const uint8_t REG_ADDR_2_SERIAL = 0x40;
const uint8_t REG_ADDR_3_SERIAL = 0x04;
const uint8_t REG_ADDR_2_56_1 = 0x20;
const uint8_t REG_ADDR_2_56_2 = 0x58;
const uint8_t REG_ADDR_2_208 = 0x90;
const uint8_t REG_ADDR_3_RW = 0x00;
const uint8_t REG_ADDR_1_WRITE = 0x02;

// Data sizes
const uint16_t SERIAL_NUMBER_SIZE = 12;
const uint16_t DATA_BLOCK_56_SIZE = 56;
const uint16_t DATA_BLOCK_208_SIZE = 208;

// Copy of the 6-byte chip ID (factory area 0x400) at 16-bit chip address 0x160.
// The printer erases it to 0xFF when the cartridge reaches end of life.
const uint16_t CHIP_ID_COPY_ADDR = 0x160;
const uint16_t CHIP_ID_SIZE = 6;
const uint16_t CHIP_ID_COPY_SIZE = 16;

// Global buffer setup, identical to the original
uint8_t BufferPlus4[1024 + 4];
// Buffer pointer offset by 4 bytes
uint8_t *Buffer = &(BufferPlus4[4]);

/**
 * @brief Calculates CRC16/CCITT-FALSE
 * (This function is platform-independent and copied from the original)
 */
uint16_t crc16(const uint8_t *data_p, uint8_t length)
{
  uint8_t x;
  uint16_t crc = 0xFFFF;
  while (length--)
  {
    x = crc >> 8 ^ *data_p++;
    x ^= x >> 4;
    crc = (crc << 8) ^ ((uint16_t)(x << 12)) ^ ((uint16_t)(x << 5)) ^ ((uint16_t)x);
  }
  return crc;
}

/**
 * @brief Checks the CRC16 stored big-endian in the last two bytes of a data block
 */
bool hasValidCrc(const uint8_t *block, uint16_t blockSize)
{
  uint16_t stored = ((uint16_t)block[blockSize - 2] << 8) | block[blockSize - 1];
  uint16_t computed = crc16(block, blockSize - 2);
  if (computed != stored)
  {
    Serial.printf("CRC mismatch : stored 0x%04X, computed 0x%04X\n", stored, computed);
    return false;
  }
  return true;
}

/**
 * @brief Translates a Wire.endTransmission() result code into a log message.
 * @return true only if the transmission succeeded (code 0)
 */
bool checkI2CResult(uint8_t nRet, const char *operation)
{
  switch (nRet)
  {
  case 0:
    return true;
  case 1:
    Serial.printf("I2C error while trying to %s the slave : Wire buffer is too small\n", operation);
    break;
  case 2:
    Serial.printf("I2C error while trying to %s the slave : address not acknowledged - is it connected ?\n", operation);
    break;
  case 3:
    Serial.printf("I2C error while trying to %s the slave : NACK during data - are the 10 address bits and the written values correct ?\n", operation);
    break;
  case 4:
    Serial.printf("I2C error while trying to %s the slave : bus error - is the wiring correct ?\n", operation);
    break;
  case 5:
    Serial.printf("I2C error while trying to %s the slave : timeout\n", operation);
    break;
  default:
    Serial.printf("I2C error while trying to %s the slave : unknown error code %u\n", operation, nRet);
    break;
  }
  return false;
}

/**
 * @brief Reads data from a 10-bit I2C address using a repeated start.
 * Rewritten for ESP32 Wire library.
 */
bool read_TI046B1_register(uint16_t TenBits_slave_address, uint8_t firstRegisterByte, uint8_t secondRegisterByte, uint8_t thirdRegisterByte, uint8_t *destinationBuffer, uint16_t readSize)
{

  // 10-bit address splitting logic
  uint16_t slave_address_LSB = TenBits_slave_address & 0x0FF;
  uint16_t slave_address_MSB = TenBits_slave_address & 0x300;
  slave_address_MSB = slave_address_MSB >> 8;

  // 0b11110xx (7-bit header for 10-bit addressing)
  uint8_t SevenBits_compat_address = 0x78 | slave_address_MSB;

  // Prepare the 4-byte register address packet
  uint8_t txBuffer[4];
  txBuffer[0] = slave_address_LSB; // 8 LSBs of the 10-bit address
  txBuffer[1] = firstRegisterByte;
  txBuffer[2] = secondRegisterByte;
  txBuffer[3] = thirdRegisterByte;

  // --- Step 1: Write the 10-bit address + register address, NO STOP (repeated start) ---
  Wire.beginTransmission(SevenBits_compat_address);
  Wire.write(txBuffer, 4);

  // Send a REPEATED START (false = do not send stop).
  // On ESP32 this only queues the write; the actual transfer (and any
  // address/NACK error) happens inside requestFrom() below.
  if (!checkI2CResult(Wire.endTransmission(false), "write to"))
    return false;

  // --- Step 2: Read the data ---
  // Request `readSize` bytes and send a STOP (true) at the end
  size_t bytesRead = Wire.requestFrom((uint16_t)SevenBits_compat_address, (size_t)readSize, true);

  // Check if we got all the bytes we asked for
  if (bytesRead == 0)
  {
    Serial.println("I2C error while trying to read from the slave : no data (no ACK, timeout or bus error).");
    return false;
  }

  if (bytesRead != readSize)
  {
    Serial.print("Unable to read all the bytes from the slave : ");
    Serial.print(bytesRead, DEC);
    Serial.println(" bytes read");
    // Still read what we got to clear the buffer
    Wire.readBytes(destinationBuffer, bytesRead);
    return false;
  }

  // Read the data into the buffer
  Wire.readBytes(destinationBuffer, readSize);

  // --- Printing ---
  Serial.print("uint8_t destinationBuffer[");
  Serial.print(readSize);
  Serial.print("] = {");
  for (uint16_t i = 0; i < readSize; i++)
  {
    char bufferHex[5];
    sprintf(bufferHex, "0x%02X", destinationBuffer[i]);
    Serial.write(bufferHex);
    if (i != readSize - 1)
      Serial.print(", ");
    else
      Serial.print("};");
  }
  Serial.println("");

  return true;
}

/**
 * @brief Writes data to a 10-bit I2C address.
 * Rewritten for ESP32 Wire library.
 */
bool write_TI046B1_register(uint16_t TenBits_slave_address, uint8_t firstRegisterByte, uint8_t secondRegisterByte, uint8_t thirdRegisterByte, uint8_t *contentBuffer, uint16_t writeSize)
{

  // 10-bit address splitting logic
  uint16_t slave_address_LSB = TenBits_slave_address & 0x0FF;
  uint16_t slave_address_MSB = TenBits_slave_address & 0x300;
  slave_address_MSB = slave_address_MSB >> 8;

  // 0b11110xx (7-bit header for 10-bit addressing)
  uint8_t SevenBits_compat_address = 0x78 | slave_address_MSB;

  // Prepare the 4-byte prefix in the *main* buffer
  contentBuffer[0] = slave_address_LSB;
  contentBuffer[1] = firstRegisterByte;
  contentBuffer[2] = secondRegisterByte;
  contentBuffer[3] = thirdRegisterByte;

  // --- Write the 4-byte prefix + data, with a STOP ---
  Wire.beginTransmission(SevenBits_compat_address);
  // Write the prefix and the data all at once
  Wire.write(contentBuffer, 4 + writeSize);

  // Send a STOP (true = send stop)
  if (!checkI2CResult(Wire.endTransmission(true), "write to"))
    return false;

  Serial.println("Write OK");
  return true;
}

// -----------------------------------------------------------------
// ALL FUNCTIONS BELOW ARE PLATFORM-INDEPENDENT
// They are copied directly from the original .ino file
// -----------------------------------------------------------------

bool readChip(uint16_t slaveAddress, uint16_t address, uint8_t *destination, uint16_t size)
{
  bool bRet = read_TI046B1_register(slaveAddress, REG_ADDR_1, address & 0xFF, address >> 8, destination, size);
  delay(10);
  return bRet;
}

/**
 * @brief Checks whether the printer has marked the cartridge as end of life.
 * When a cartridge is used up, the printer overwrites the model fields of the
 * 56-byte blocks (bytes 21-24) and the chip ID copy at 0x160 with 0xFF, and it
 * keeps the cartridge as "End of Life" in its own memory, keyed by the
 * read-only serial number. A reset cannot undo that, and resetting such a chip
 * makes the printer report it as "Missing or Defective".
 * @return false if the markers could not be read
 */
bool readEndOfLifeMarkers(uint16_t slaveAddress, bool &endOfLife)
{
  uint8_t block56[DATA_BLOCK_56_SIZE];
  if (!readChip(slaveAddress, REG_ADDR_2_56_1, block56, DATA_BLOCK_56_SIZE))
    return false;
  bool modelMarked = block56[20] == 0xFF && block56[21] == 0xFF && block56[22] == 0xFF && block56[23] == 0xFF;

  uint8_t idCopy[CHIP_ID_COPY_SIZE];
  if (!readChip(slaveAddress, CHIP_ID_COPY_ADDR, idCopy, CHIP_ID_COPY_SIZE))
    return false;
  bool idMarked = true;
  for (int i = 0; i < CHIP_ID_SIZE; i++)
    if (idCopy[i] != 0xFF)
      idMarked = false;

  endOfLife = modelMarked || idMarked;
  return true;
}

void Reset56(uint8_t *Buffer)
{
  // Bytes 2 and 3: Toner level to full
  Buffer[1] = 0xFF;
  Buffer[2] = 0xFF;

  // Byte 6: Status to full
  Buffer[5] = 0x1F;

  // Bytes 15 and 16: Printed pages to 0
  Buffer[14] = 0x00;
  Buffer[15] = 0x00;

  // Bytes 17-20: Set to 0
  Buffer[16] = 0x00;
  Buffer[17] = 0x00;
  Buffer[18] = 0x00;
  Buffer[19] = 0x00;

  // Bytes 29 and 30: Set to 0
  Buffer[28] = 0x00;
  Buffer[29] = 0x00;

  // Byte 47: Set to 0
  Buffer[46] = 0x00;

  // Bytes 50 and 51: Set to 0
  Buffer[49] = 0x00;
  Buffer[50] = 0x00;

  // Recalculate CRC
  uint16_t checksum = crc16(Buffer, 54);
  Buffer[54] = (checksum >> 8) & 0xFF;
  Buffer[55] = checksum & 0xFF;
}

void Reset208(uint8_t *Buffer)
{
  // Clear the first 169 bytes.
  for (int i = 0; i < 169; i++)
  {
    Buffer[i] = 0x00;
  }

  // Generate a new pseudo-random UUID.
  for (int i = 169; i < 199; i++)
  {
    Buffer[i] = random(256);
  }

  // Clear the bytes after the UUID.
  for (int i = 199; i < 206; i++)
  {
    Buffer[i] = 0x00;
  }

  // Recalculate CRC.
  uint16_t checksum = crc16(Buffer, 206);
  Buffer[206] = (checksum >> 8) & 0xFF;
  Buffer[207] = checksum & 0xFF;
}

bool processDataBlock(uint16_t slaveAddress, uint8_t regAddr2, uint16_t dataSize, void (*resetFunc)(uint8_t*)) {
    bool bRet = read_TI046B1_register(slaveAddress, REG_ADDR_1, regAddr2, REG_ADDR_3_RW, Buffer, dataSize);
    delay(10);
    if (bRet == false) return false;

    // Never rewrite a block that was not read cleanly: the reset would stamp
    // a fresh, valid CRC on top of corrupted data.
    if (!hasValidCrc(Buffer, dataSize)) {
        Serial.println("Read data failed CRC check, aborting without writing.");
        return false;
    }

    resetFunc(Buffer);

    // Keep a copy of what we write, the verification read overwrites Buffer
    uint8_t expected[DATA_BLOCK_208_SIZE];
    memcpy(expected, Buffer, dataSize);

    if (!write_TI046B1_register(slaveAddress, REG_ADDR_1_WRITE, regAddr2, REG_ADDR_3_RW, BufferPlus4, dataSize)) return false;
    delay(10);

    bRet = read_TI046B1_register(slaveAddress, REG_ADDR_1, regAddr2, REG_ADDR_3_RW, Buffer, dataSize);
    delay(10);
    if (bRet == false) return false;

    if (memcmp(expected, Buffer, dataSize) != 0) {
        Serial.println("Verification failed : read-back data differs from written data.");
        return false;
    }
    Serial.println("Verify OK");
    Serial.println("");
    return true;
}

enum ResetResult
{
  RESET_OK,
  RESET_FAILED,
  RESET_END_OF_LIFE,
};

/**
 * @brief Resets all data blocks of one cartridge.
 * Stops at the first failing block so a broken transfer is not followed by more writes.
 * Writes nothing to a cartridge the printer has marked as end of life.
 * @return RESET_OK only if every block was written and verified
 */
ResetResult ResetCartridge(uint16_t slaveAddress, const char *colorName)
{
  bool bRet = read_TI046B1_register(slaveAddress, REG_ADDR_1, REG_ADDR_2_SERIAL, REG_ADDR_3_SERIAL, Buffer, SERIAL_NUMBER_SIZE);
  delay(10);
  if (bRet == false)
    return RESET_FAILED;
  Buffer[SERIAL_NUMBER_SIZE] = 0x00;
  Serial.print(colorName);
  Serial.print(" serial number : ");
  Serial.println((char *)Buffer);

  bool endOfLife;
  if (!readEndOfLifeMarkers(slaveAddress, endOfLife))
    return RESET_FAILED;
  if (endOfLife)
  {
    Serial.println("This cartridge has reached END OF LIFE: the printer keeps it as empty, keyed by its serial number.");
    Serial.println("A reset cannot change that and would make the printer report it as defective. Nothing was written.");
    return RESET_END_OF_LIFE;
  }

  Serial.print(colorName);
  Serial.print(", (0x");
  Serial.print(slaveAddress, HEX);
  Serial.println("), 56 bytes register, first one");
  if (!processDataBlock(slaveAddress, REG_ADDR_2_56_1, DATA_BLOCK_56_SIZE, Reset56))
    return RESET_FAILED;

  Serial.print(colorName);
  Serial.print(", (0x");
  Serial.print(slaveAddress, HEX);
  Serial.println("), 56 bytes register, second one");
  if (!processDataBlock(slaveAddress, REG_ADDR_2_56_2, DATA_BLOCK_56_SIZE, Reset56))
    return RESET_FAILED;

  Serial.print(colorName);
  Serial.print(", (0x");
  Serial.print(slaveAddress, HEX);
  Serial.println("), 208 bytes register");
  if (!processDataBlock(slaveAddress, REG_ADDR_2_208, DATA_BLOCK_208_SIZE, Reset208))
    return RESET_FAILED;
  return RESET_OK;
}

void blinkLed(uint32_t color, int times, uint32_t intervalMs)
{
  for (int i = 0; i < times; i++)
  {
    pixels.setPixelColor(0, color);
    pixels.show();
    delay(intervalMs);
    pixels.setPixelColor(0, pixels.Color(0, 0, 0)); // Off
    pixels.show();
    delay(intervalMs);
  }
}

void ResetBlack()
{
  ResetCartridge(BLACK_CHIP_ADDRESS, "Black");
}

void ResetCyan()
{
  ResetCartridge(CYAN_CHIP_ADDRESS, "Cyan");
}

void ResetMagenta()
{
  ResetCartridge(MAGENTA_CHIP_ADDRESS, "Magenta");
}

void ResetYellow()
{
  ResetCartridge(YELLOW_CHIP_ADDRESS, "Yellow");
}

void detectAndReset()
{
  const uint16_t addresses[] = {BLACK_CHIP_ADDRESS, CYAN_CHIP_ADDRESS, MAGENTA_CHIP_ADDRESS, YELLOW_CHIP_ADDRESS};
  const char *colors[] = {"Black", "Cyan", "Magenta", "Yellow"};
  const uint32_t ledColors[] = {
    pixels.Color(255, 255, 255), // White for Black
    pixels.Color(0, 255, 255),   // Cyan
    pixels.Color(255, 0, 255),   // Magenta
    pixels.Color(255, 255, 0)    // Yellow
  };
  for (int i = 0; i < 4; i++)
  {
    Serial.print("Pinging address 0x");
    Serial.print(addresses[i], HEX);
    Serial.println("...");

    // Try to read 1 byte to see if a chip is present.
    if (read_TI046B1_register(addresses[i], REG_ADDR_1, REG_ADDR_2_SERIAL, REG_ADDR_3_SERIAL, Buffer, 1))
    {
      Serial.print("Detected ");
      Serial.print(colors[i]);
      Serial.println(" cartridge. Starting reset.");

      pixels.setPixelColor(0, ledColors[i]);
      pixels.show();

      ResetResult result = ResetCartridge(addresses[i], colors[i]);
      if (result == RESET_OK)
      {
        Serial.println("Reset successful. Insert another cartridge of the same color into the printer");
        Serial.println("before this one, otherwise the printer restores its remembered level.");
        delay(2000);
      }
      else if (result == RESET_END_OF_LIFE)
      {
        pixels.setPixelColor(0, pixels.Color(255, 0, 0)); // Solid red: end of life, nothing written
        pixels.show();
        delay(5000);
      }
      else
      {
        Serial.println("RESET FAILED. The cartridge may not have been reset completely.");
        blinkLed(pixels.Color(255, 0, 0), 20, 125); // Fast red blink: reset failed
      }

      pixels.setPixelColor(0, pixels.Color(0, 255, 0)); // Green
      pixels.show();
      return; // Found and processed, so we are done.
    }
    else
    {
      Serial.println("No response.");
      delay(10);
    }
  }

  Serial.println("No cartridge detected.");
  blinkLed(pixels.Color(255, 0, 0), 5, 500); // Slow red blink: no cartridge
  pixels.setPixelColor(0, pixels.Color(0, 255, 0)); // Green
  pixels.show();
}

// -----------------------------------------------------------------
// READ-ONLY DIAGNOSTICS (serial commands)
// Nothing below writes to the chip: every access uses the read command REG_ADDR_1.
// -----------------------------------------------------------------

void dumpDataBlock(uint16_t slaveAddress, uint8_t regAddr2, uint8_t regAddr3, uint16_t dataSize, bool hasCrc)
{
  Serial.printf("--- 0x%X : 0x%02X 0x%02X 0x%02X, %u bytes ---\n", slaveAddress, REG_ADDR_1, regAddr2, regAddr3, dataSize);
  bool bRet = read_TI046B1_register(slaveAddress, REG_ADDR_1, regAddr2, regAddr3, Buffer, dataSize);
  delay(10);
  if (bRet && hasCrc && hasValidCrc(Buffer, dataSize))
    Serial.println("CRC OK");
}

/**
 * @brief Dumps the known blocks of every cartridge chip that answers, without writing.
 */
void dumpCartridges()
{
  const uint16_t addresses[] = {BLACK_CHIP_ADDRESS, CYAN_CHIP_ADDRESS, MAGENTA_CHIP_ADDRESS, YELLOW_CHIP_ADDRESS};
  const char *colors[] = {"Black", "Cyan", "Magenta", "Yellow"};
  bool found = false;

  for (int i = 0; i < 4; i++)
  {
    if (!read_TI046B1_register(addresses[i], REG_ADDR_1, REG_ADDR_2_SERIAL, REG_ADDR_3_SERIAL, Buffer, 1))
      continue;
    found = true;
    Serial.printf("=== DUMP %s (0x%X) ===\n", colors[i], addresses[i]);
    dumpDataBlock(addresses[i], REG_ADDR_2_SERIAL, REG_ADDR_3_SERIAL, SERIAL_NUMBER_SIZE, false);
    dumpDataBlock(addresses[i], REG_ADDR_2_56_1, REG_ADDR_3_RW, DATA_BLOCK_56_SIZE, true);
    dumpDataBlock(addresses[i], REG_ADDR_2_56_2, REG_ADDR_3_RW, DATA_BLOCK_56_SIZE, true);
    dumpDataBlock(addresses[i], REG_ADDR_2_208, REG_ADDR_3_RW, DATA_BLOCK_208_SIZE, true);
    dumpDataBlock(addresses[i], CHIP_ID_COPY_ADDR & 0xFF, CHIP_ID_COPY_ADDR >> 8, CHIP_ID_COPY_SIZE, false);
    Serial.printf("=== END DUMP %s ===\n", colors[i]);
  }
  if (!found)
    Serial.println("No cartridge detected.");
}

// Blocks the "w" command may write; the data must always be a whole block
struct WritableBlock
{
  uint16_t address;
  uint16_t size;
  bool hasCrc;
};
const WritableBlock WRITABLE_BLOCKS[] = {
    {REG_ADDR_2_56_1, DATA_BLOCK_56_SIZE, true},
    {REG_ADDR_2_56_2, DATA_BLOCK_56_SIZE, true},
    {REG_ADDR_2_208, DATA_BLOCK_208_SIZE, true},
    {CHIP_ID_COPY_ADDR, CHIP_ID_COPY_SIZE, false},
};

/**
 * @brief Writes one whole block from hex data and verifies it by reading it back.
 * Used to restore a chip from a saved image, e.g. after the printer corrupted it.
 */
bool writeBlockCommand(uint16_t slaveAddress, uint16_t address, const char *hex)
{
  const WritableBlock *block = NULL;
  for (const WritableBlock &candidate : WRITABLE_BLOCKS)
    if (candidate.address == address)
      block = &candidate;
  if (block == NULL)
  {
    Serial.println("Not the start of a writable block (20, 58, 90 or 160).");
    return false;
  }

  if (strlen(hex) != block->size * 2u)
  {
    Serial.printf("Expected %u bytes of hex data, got %u characters.\n", block->size, strlen(hex));
    return false;
  }
  for (uint16_t i = 0; i < block->size; i++)
  {
    char byteHex[3] = {hex[2 * i], hex[2 * i + 1], 0};
    char *end;
    Buffer[i] = strtoul(byteHex, &end, 16);
    if (*end != 0 || !isxdigit(byteHex[0]))
    {
      Serial.println("Invalid hex data.");
      return false;
    }
  }
  if (block->hasCrc && !hasValidCrc(Buffer, block->size))
  {
    Serial.println("Refusing to write a block with an invalid CRC.");
    return false;
  }

  uint8_t expected[DATA_BLOCK_208_SIZE];
  memcpy(expected, Buffer, block->size);
  if (!write_TI046B1_register(slaveAddress, REG_ADDR_1_WRITE, address & 0xFF, address >> 8, BufferPlus4, block->size))
    return false;
  delay(10);
  if (!readChip(slaveAddress, address, Buffer, block->size))
    return false;
  if (memcmp(expected, Buffer, block->size) != 0)
  {
    Serial.println("Verification failed : read-back data differs from written data.");
    return false;
  }
  Serial.println("Verify OK");
  return true;
}

/**
 * @brief Handles one serial command line:
 *   d                         dump all known blocks of the connected chip(s)
 *   r <addr> <reg2> <reg3> <len>  raw read, all values hex (e.g. "r 2 20 0 38")
 *   w <addr> <block> <data>   write one whole block (20, 58, 90 or 160) from hex data, verified
 */
void handleSerialCommand()
{
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0)
    return;

  unsigned int addr, reg2, reg3, len;
  int dataStart = -1;
  if (line == "d")
  {
    dumpCartridges();
  }
  else if (sscanf(line.c_str(), "r %x %x %x %x", &addr, &reg2, &reg3, &len) == 4)
  {
    if (addr > 0x3FF || reg2 > 0xFF || reg3 > 0xFF || len == 0 || len > 512)
      Serial.println("Out of range : addr <= 3FF, reg2/reg3 <= FF, 0 < len <= 200 (hex)");
    else
      dumpDataBlock(addr, reg2, reg3, len, false);
  }
  else if (sscanf(line.c_str(), "w %x %x %n", &addr, &reg2, &dataStart) == 2 && dataStart > 0)
  {
    if (addr > 0x3FF)
      Serial.println("Out of range : addr <= 3FF");
    else
      writeBlockCommand(addr, reg2, line.c_str() + dataStart);
  }
  else
  {
    Serial.println("Commands : d | r <addr> <reg2> <reg3> <len> | w <addr> <block> <data> (hex)");
  }
  Serial.println("End command.");
}

bool isButtonPressed()
{
  return digitalRead(BUTTON_PIN) == BUTTON_ACTIVE_LEVEL;
}

/**
 * @brief Main setup function
 */
void setup()
{
  Serial.begin(115200);
#if ARDUINO_USB_CDC_ON_BOOT
  // Native USB (e.g. ESP32-S2): give the host a moment to open the port so the banner is not lost
  unsigned long serialWaitStart = millis();
  while (!Serial && millis() - serialWaitStart < 2000)
    delay(10);
#endif
  Serial.println("----- I2C Reset of the TI046B1 CHIPS ------");
  Serial.println("-------------------------------------------");

  pinMode(BUTTON_PIN, BUTTON_ACTIVE_LEVEL == LOW ? INPUT_PULLUP : INPUT_PULLDOWN);
  // No randomSeed() on purpose: without it random() uses the ESP32 hardware RNG
  // (esp_random), calling it would switch to the weaker software rand().

  pixels.begin();
  pixels.setPixelColor(0, pixels.Color(0, 255, 0)); // Green
  pixels.show();

  // Set the I2C buffer size to 1024 bytes (must be called BEFORE Wire.begin())
  Wire.setBufferSize(1024);
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);

  // Set I2C clock to 100 kHz, matching the original TWI_FREQ
  Wire.setClock(100000);

  Serial.printf("Button on GPIO %d is %s (active %s)\n", BUTTON_PIN, isButtonPressed() ? "PRESSED - check the wiring" : "released", BUTTON_ACTIVE_LEVEL == HIGH ? "HIGH" : "LOW");
  Serial.println("Ready to reset. Press the button.");
}
void loop()
{
  if (Serial.available())
    handleSerialCommand();

  // A press only counts after the button has been seen released, so a button
  // that already reads as pressed at boot (or is still held after a reset)
  // never starts a write by itself.
  static bool buttonArmed = false;
  if (!isButtonPressed())
  {
    buttonArmed = true;
    return;
  }
  if (!buttonArmed)
    return;

  delay(50); // Debounce
  if (isButtonPressed())
  {
    buttonArmed = false;
    Serial.println("Button pressed. Begin reset op.");
    detectAndReset();
    Serial.println("End reset op.");
  }
}