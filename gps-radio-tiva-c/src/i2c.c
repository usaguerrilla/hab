/*
 * i2c.c - I2C backup communications of telemetry MCU with Raspberry PI 2
 *
 * Acts as an I2C slave with the address set in i2c.h. Supports 100 KHz and 400 KHz. The
 * signals appear on PA6 (I2C1SCL) and PA7 (I2C1SDA). These are pins 23 and 24 respectively
 * on the MCU, corresponding to header pins J1.09 and J1.10 on the Tiva C LaunchPad.
 *
 * Register map [current]:
 * [0x00] - WHO_AM_I - always returns the I2C slave address
 * [0x01] - SW_VERSION_MAJOR - returns the major revision defined in i2c.h
 * [0x02] - SW_VERSION_MINOR - returns the minor revision defined in i2c.h
 * [0x03] - DATA_AVAILABLE - returns 1 if data has been updated since last read of any DR, 0 otherwise
 * [0x04-0x05] - TEMP - temperature ADC reading in raw counts LSB first
 * [0x06-0x07] - VOLT - voltage ADC reading in millivolts LSB first
 * [0x08-0x09] - EEADDR - current EEPROM adddress pointer [writable]
 * [0x0A-0x0D] - EEDATA - word at current EEPROM position
 * [0x0E-0x0F] - RESERVED
 * [0x10-0x13] � LAT 1 - Latitude as signed 32 bit integer in degrees * 1E6 LSB first
 * [0x14-0x17] - LON 1 - Longitude as signed 32 bit integer in degrees * 1E6 LSB first
 * [0x18-0x19] - VEL 1 - Velocity as unsigned 16 bit integer in km/hr * 10 LSB first
 * [0x1A-0x1B] - HDG 1 - Heading as unsigned 16 bit integer in degrees * 10 [true] LSB first
 * [0x1C-0x1F] � ALT 1 � Altitude as signed 32-bit integer in cm LSB first
 * [0x20] � SAT 1 � # satellites visible or 0 if no fix
 * [0x21-0x2F] � RESERVED
 * [0x30-0x33] � LAT 2 - Latitude as signed 32 bit integer in degrees * 1E6 LSB first
 * [0x34-0x37] - LON 2 - Longitude as signed 32 bit integer in degrees * 1E6 LSB first
 * [0x38-0x39] - VEL 2 - Velocity as unsigned 16 bit integer in km/hr * 10 LSB first
 * [0x3A-0x3B] - HDG 2 - Heading as unsigned 16 bit integer in degrees * 10 [true] LSB first
 * [0x3C-0x3F] � ALT 2 � Altitude as signed 32-bit integer in cm LSB first
 * [0x40] � SAT 2 � # satellites visible or 0 if no fix
 *
 * Register map [VERSION_MAJOR = 1]:
 * [0x00] - WHO_AM_I - always returns the I2C slave address
 * [0x01] - SW_VERSION_MAJOR - returns the major revision defined in i2c.h
 * [0x02] - SW_VERSION_MINOR - returns the minor revision defined in i2c.h
 * [0x03] - DATA_AVAILABLE - returns 1 if data has been updated since last read of any DR, 0 otherwise
 * [0x04-0x07] - LAT - Latitude as signed 32 bit integer in degrees * 1E6 LSB first
 * [0x08-0x0B] - LON - Longitude as signed 32 bit integer in degrees * 1E6 LSB first
 * [0x0C-0x0F] - VEL - Velocity as signed 32 bit integer [TBD] LSB first
 * [0x10-0x11] - HDG - Heading as unsigned 16 bit integer in degrees LSB first
 */

#include "i2c.h"
#include "eeprom.h"
#include "signals.h"
#include <string.h>

#include <driverlib/i2c.h>
#include <driverlib/rom.h>
#include <driverlib/gpio.h>
#include <driverlib/sysctl.h>
#include <driverlib/pin_map.h>
#include <driverlib/rom_map.h>

#include <inc/hw_ints.h>
#include <inc/hw_memmap.h>

static struct
{
    // I2C register data
    uint8_t regs[I2C_NUM_REGS];
    // Address pointer
    volatile uint8_t address;
    // 1 if a communication was ever received, or 0 otherwise
    uint8_t running;
} i2cData;

static void updateI2CEEPROM()
{
#ifdef EEPROM_ENABLED            
    // Update the EEDATA register, up to 512 words (0x200) are accessible
    uint32_t address = (uint32_t)i2cData.regs[REG_EEADDR_0];
    address |= ((uint32_t)i2cData.regs[REG_EEADDR_1]) << 8;
    // Read data and load
    uint32_t data = eepromRead((address & 0x1FF) << 2U);
    *((uint32_t *)(&(i2cData.regs[REG_EEDATA_0]))) = data;
#endif
}

bool i2cCommRunning(void)
{
    return i2cData.running != 0U;
}

void I2cSlaveHandler(void)
{
    const uint32_t action = MAP_I2CSlaveStatus(I2C_MODULE);
    bool ack = false;
    // Shut off the alarm clock to prevent us from being called again
    MAP_I2CSlaveIntClear(I2C_MODULE);
    switch (action)
    {
        case I2C_SLAVE_ACT_RREQ_FBR:
        {
            // This is the address
            uint32_t newAddress = MAP_I2CSlaveDataGet(I2C_MODULE);
            if (newAddress >= I2C_NUM_REGS)
                // Prevent array access out of bounds
                newAddress = I2C_NUM_REGS - 1U;
            i2cData.address = (uint8_t)newAddress;
            i2cData.running = 1U;
            ack = true;
            break;
        }
        case I2C_SLAVE_ACT_RREQ:
        {
            // Always ACK, but only allow changes to the EEADDR register
            uint32_t data = MAP_I2CSlaveDataGet(I2C_MODULE), address = (uint32_t)i2cData.address;
            if (address == REG_EEADDR_0 || address == REG_EEADDR_1)
            {
                i2cData.regs[address] = (uint8_t)data;
                updateI2CEEPROM();
            }
            address++;
            if (address >= I2C_NUM_REGS)
                // Prevent array access out of bounds
                address = 0U;
            i2cData.address = (uint8_t)address;
            ack = true;
            break;
        }
        case I2C_SLAVE_ACT_TREQ:
        {
            // Data has been requested from us
            uint32_t address = (uint32_t)i2cData.address;
            // Clear data available flag if necessary
            if (address >= REG_LON_0 && address <= REG_HDG_1)
                i2cData.regs[REG_DATA_AVAILABLE] = 0U;
            // Store data with auto increment
            MAP_I2CSlaveDataPut(I2C_MODULE, i2cData.regs[address++]);
            if (address >= I2C_NUM_REGS)
                // Prevent array access out of bounds
                address = 0U;
            i2cData.address = (uint8_t)address;
            ack = true;
            break;
        }
        default:
            // No action, or an invalid action (No QCMD, 2nd address on this device)
            break;
    }
    // Send ACK/NACK
    MAP_I2CSlaveACKValueSet(I2C_MODULE, ack);
    MAP_I2CSlaveACKOverride(I2C_MODULE, true);
}

void submitI2CData(uint32_t index, GpsData *data)
{
    union
    {
        uint8_t bytes[4];
        int32_t word;
    } data32;
    union
    {
        uint8_t bytes[2];
        uint16_t hword;
    } data16;
    // Find the correct location
    uint8_t *ptr;
    if (index == 0)
        ptr = &i2cData.regs[REG_BANK_1];
    else
        ptr = &i2cData.regs[REG_BANK_2];
    // Mask I2C interrupts while we update
    MAP_I2CSlaveIntDisable(I2C_MODULE);
    // Latitude update
    data32.word = angularCoordinateToInt32Degrees(data->gpggaData.latitude);
    memcpy(ptr + REG_LAT_0, data32.bytes, sizeof(data32.bytes));
    // Longitude update
    data32.word = angularCoordinateToInt32Degrees(data->gpggaData.longitude);
    memcpy(ptr + REG_LON_0, data32.bytes, sizeof(data32.bytes));
    // Altitude update
    data32.word = data->gpggaData.altitudeMslMeters;
    memcpy(ptr + REG_ALT_0, data32.bytes, sizeof(data32.bytes));
    // Velocity update
    data16.hword = data->gpvtgData.speedKph;
    memcpy(ptr + REG_VEL_0, data16.bytes, sizeof(data16.bytes));
    // Heading update
    data16.hword = data->gpvtgData.trueCourseDegrees;
    memcpy(ptr + REG_HDG_0, data16.bytes, sizeof(data16.bytes));
    // Satellites update
    ptr[REG_SAT] = data->gpggaData.numberOfSattelitesInUse;
    // Data is available
    i2cData.regs[REG_DATA_AVAILABLE] = 1U;
    MAP_I2CSlaveIntEnableEx(I2C_MODULE, I2C_SLAVE_INT_DATA);
}

void submitI2CTelemetry(Telemetry *telemetry)
{
    union
    {
        uint8_t bytes[2];
        uint16_t hword;
    } data16;
    // Mask I2C interrupts while we update
    MAP_I2CSlaveIntDisable(I2C_MODULE);
    // Velocity update
    data16.hword = (uint16_t)telemetry->cpuTemperature;
    memcpy(&i2cData.regs[REG_TEMP_0], data16.bytes, sizeof(data16.bytes));
    // Heading update
    data16.hword = (uint16_t)telemetry->voltage;
    memcpy(&i2cData.regs[REG_VOLT_0], data16.bytes, sizeof(data16.bytes));
    // Restore interrupts
    MAP_I2CSlaveIntEnableEx(I2C_MODULE, I2C_SLAVE_INT_DATA);
}

void initializeI2C(void)
{
    // Peripheral enable: the I/O port and the I2C module
    MAP_SysCtlPeripheralEnable(SYSCTL_PERIPH_GPIOA);
    MAP_SysCtlPeripheralEnable(SYSCTL_PERIPH_I2C1);
    // Set up pins to I2C mode
    MAP_GPIOPinConfigure(GPIO_PA6_I2C1SCL);
    MAP_GPIOPinConfigure(GPIO_PA7_I2C1SDA);
    MAP_GPIOPadConfigSet(GPIO_PORTA_BASE, GPIO_PIN_6 | GPIO_PIN_7, GPIO_STRENGTH_8MA, GPIO_PIN_TYPE_STD);
    MAP_GPIOPinTypeI2C(GPIO_PORTA_BASE, GPIO_PIN_7);
    MAP_GPIOPinTypeI2CSCL(GPIO_PORTA_BASE, GPIO_PIN_6);
    // Register IRQ and clear spurious conditions
    MAP_IntPrioritySet(INT_I2C1, 0x20);
    MAP_I2CSlaveIntClear(I2C_MODULE);
    MAP_I2CSlaveIntEnableEx(I2C_MODULE, I2C_SLAVE_INT_DATA);
    MAP_IntEnable(INT_I2C1);
    // Set up in slave mode with correct address
    MAP_I2CMasterDisable(I2C_MODULE);
    MAP_I2CSlaveEnable(I2C_MODULE);
    MAP_I2CSlaveInit(I2C_MODULE, I2C_ADDRESS);
    // Init register file
    i2cData.address = 0U;
    i2cData.running = 0U;
    memset(i2cData.regs, 0, sizeof(i2cData.regs));
    i2cData.regs[REG_WHO_AM_I] = I2C_ADDRESS;
    i2cData.regs[REG_SW_VERSION_MAJOR] = SW_VERSION_MAJOR;
    i2cData.regs[REG_SW_VERSION_MINOR] = SW_VERSION_MINOR;
    updateI2CEEPROM();
}
