#define BUFFER_LENGTH 1024

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_NeoPixel.h>

#define BUTTON_PIN 12
#define LED_PIN 13
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

  // Send a REPEATED START (false = do not send stop)
  uint8_t nRet = Wire.endTransmission(false);

  // Error handling
  if (nRet == 1)
  {
    Serial.println("Error occured wile trying to write to the slave : TWI class's buffer is too small");
    return false;
  }
  else if (nRet == 2)
  {
    Serial.println("Error occured wile trying to write to the slave : Slave's address not acknowldged (begning only) - is it connected ?");
    return false;
  }
  else if (nRet == 3)
  {
    Serial.println("Error occured wile trying to write to the slave :");
    Serial.println("Slaves's begining of address acknowledged but NACK encountered during the following writes - are all of the 10 bits of slave's address correct ? Are the writes value are meaningful to the slave ?");
    return false;
  }
  else if (nRet == 4)
  {
    Serial.println("I2C protocol error occured wile trying to write to the slave. Is the wiring correct ? TWI Initialisation ?");
    return false;
  }

  // --- Step 2: Read the data ---
  // Request `readSize` bytes and send a STOP (true) at the end
  uint16_t bytesRead = Wire.requestFrom(SevenBits_compat_address, (uint8_t)readSize, (uint8_t)true);

  // Check if we got all the bytes we asked for
  if (bytesRead == 0)
  {
    Serial.println("Error occured wile trying to read from the slave.");
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
  uint8_t nRet = Wire.endTransmission(true);

  // Error handling
  if (nRet == 1)
  {
    Serial.println("Error occured wile trying to address the slave : TWI class's buffer is too small");
    return false;
  }
  else if (nRet == 2)
  {
    Serial.println("Error occured wile trying to address the slave : Slave's address not acknowldged (begning only) - is it connected ?");
    return false;
  }
  else if (nRet == 3)
  {
    Serial.println("Error occured wile trying to address the slave :");
    Serial.println("Slaves's begining of address acknowledged but NACK encountered during the following writes - are all of the 10 bits of slave's address correct ?");
    Serial.println("Are the writes value are meaningful to the slave ?");
    return false;
  }
  else if (nRet == 4)
  {
    Serial.println("I2C protocol error occured wile trying to address the slave. Is the wiring correct ? TWI Initialisation ?");
    return false;
  }

  // else :
  Serial.println("Write OK");
  return true;
}

// -----------------------------------------------------------------
// ALL FUNCTIONS BELOW ARE PLATFORM-INDEPENDENT
// They are copied directly from the original .ino file
// -----------------------------------------------------------------

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

void processDataBlock(uint16_t slaveAddress, uint8_t regAddr2, uint16_t dataSize, void (*resetFunc)(uint8_t*)) {
    bool bRet = read_TI046B1_register(slaveAddress, REG_ADDR_1, regAddr2, REG_ADDR_3_RW, Buffer, dataSize);
    delay(10);
    if (bRet == false) return;

    resetFunc(Buffer);

    if (!write_TI046B1_register(slaveAddress, REG_ADDR_1_WRITE, regAddr2, REG_ADDR_3_RW, BufferPlus4, dataSize)) return;
    delay(10);

    read_TI046B1_register(slaveAddress, REG_ADDR_1, regAddr2, REG_ADDR_3_RW, Buffer, dataSize);
    delay(10);
    Serial.println("");
}

void ResetCartridge(uint16_t slaveAddress, const char *colorName)
{
  bool bRet = read_TI046B1_register(slaveAddress, REG_ADDR_1, REG_ADDR_2_SERIAL, REG_ADDR_3_SERIAL, Buffer, SERIAL_NUMBER_SIZE);
  delay(10);
  if (bRet == false)
    return;
  Buffer[SERIAL_NUMBER_SIZE] = 0x00;
  Serial.print(colorName);
  Serial.print(" serial number : ");
  Serial.println((char *)Buffer);

  Serial.print(colorName);
  Serial.print(", (0x");
  Serial.print(slaveAddress, HEX);
  Serial.println("), 56 bytes register, first one");
  processDataBlock(slaveAddress, REG_ADDR_2_56_1, DATA_BLOCK_56_SIZE, Reset56);

  Serial.print(colorName);
  Serial.print(", (0x");
  Serial.print(slaveAddress, HEX);
  Serial.println("), 56 bytes register, second one");
  processDataBlock(slaveAddress, REG_ADDR_2_56_2, DATA_BLOCK_56_SIZE, Reset56);

  Serial.print(colorName);
  Serial.print(", (0x");
  Serial.print(slaveAddress, HEX);
  Serial.println("), 208 bytes register");
  processDataBlock(slaveAddress, REG_ADDR_2_208, DATA_BLOCK_208_SIZE, Reset208);
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
  bool found = false;

  for (int i = 0; i < 4; i++)
  {
    Serial.print("Pinging address 0x");
    Serial.print(addresses[i], HEX);
    Serial.println("...");

    // Try to read 1 byte to see if a chip is present.
    if (read_TI046B1_register(addresses[i], REG_ADDR_1, REG_ADDR_2_SERIAL, REG_ADDR_3_SERIAL, Buffer, 1))
    {
      found = true;
      Serial.print("Detected ");
      Serial.print(colors[i]);
      Serial.println(" cartridge. Starting reset.");

      pixels.setPixelColor(0, ledColors[i]);
      pixels.show();

      ResetCartridge(addresses[i], colors[i]);

      delay(2000);
      pixels.setPixelColor(0, pixels.Color(0, 255, 0)); // Green
      pixels.show();
      return; // Found and reset, so we are done.
    }
    else
    {
      Serial.println("No response.");
      delay(10);
    }
  }

  if (!found) {
    Serial.println("No cartridge detected.");
    for (int i = 0; i < 5; i++) {
      pixels.setPixelColor(0, pixels.Color(255, 0, 0)); // Red
      pixels.show();
      delay(500);
      pixels.setPixelColor(0, pixels.Color(0, 0, 0)); // Off
      pixels.show();
      delay(500);
    }
    pixels.setPixelColor(0, pixels.Color(0, 255, 0)); // Green
    pixels.show();
  }
}

/**
 * @brief Main setup function
 */
void setup()
{
  Serial.begin(115200);
  Serial.println("----- I2C Reset of the TI046B1 CHIPS ------");
  Serial.println("-------------------------------------------");

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(34, INPUT);
  randomSeed(analogRead(34));

  pixels.begin();
  pixels.setPixelColor(0, pixels.Color(0, 255, 0)); // Green
  pixels.show();

  // Set the I2C buffer size to 1024 bytes (must be called BEFORE Wire.begin())
  Wire.setBufferSize(1024);
  // Default ESP32 pins are SDA=21, SCL=22
  Wire.begin();

  // Set I2C clock to 100 kHz, matching the original TWI_FREQ
  Wire.setClock(100000);

  Serial.println("Ready to reset. Press the button.");
}
void loop()
{
  if (digitalRead(BUTTON_PIN) == LOW)
  {
    delay(50); // Debounce
    if (digitalRead(BUTTON_PIN) == LOW)
    {
      Serial.println("Button pressed. Begin reset op.");
      detectAndReset();
      Serial.println("End reset op.");
    }
  }
}