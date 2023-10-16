/*
 * LIS3DH.cpp
 *
 *  Created on: 14 Mar 2021
 *      Author: David
 */

#include "LIS3DH.h"

#if SUPPORT_LIS3DH

#include <Hardware/IoPorts.h>
#include <Movement/StepTimer.h>

constexpr uint16_t LisAddresses[] =
{
	0b0011000,						// LIS3DH or LIS2DW with SDO/SA0 connected to ground, address 0x30/0x32
	0b0011001,						// LIS3DH or LIS2DW with SDO/SA0 connected to Vcc, address 0x32/0x33
	0b0011110,						// LIS3DSH with SEL grounded
	0b0011101						// LIS3DSH with SEL connected to Vcc
};

constexpr uint8_t FifoInterruptLevel = 24;							// how full the FIFO must get before we want an interrupt

static constexpr uint8_t WhoAmIValue_3DH = 0x33;
static constexpr uint8_t WhoAmIValue_3DSH = 0x3F;
static constexpr uint8_t WhoAmIValue_2DW = 0x44;

#if ACCELEROMETER_USES_SPI

constexpr uint32_t DefaultAccelerometerSpiFrequency = 2000000;
constexpr SpiMode lisMode = SpiMode::mode3;
constexpr uint32_t Lis3dSpiTimeout = 25;							// timeout while waiting for the SPI bus

LIS3DH::LIS3DH(SharedSpiDevice& dev, Pin p_csPin, Pin p_int1Pin) noexcept
	: SharedSpiClient(dev, DefaultAccelerometerSpiFrequency, lisMode, p_csPin, false),
	  taskWaiting(nullptr), accelerometerType(AccelerometerType::LIS3DH), int1Pin(p_int1Pin)
{
	SetCsPin(p_csPin);
}

#else

constexpr uint32_t Lis3dI2CTimeout = 25;

LIS3DH::LIS3DH(SharedI2CMaster& dev, Pin p_int1Pin) noexcept
	: SharedI2CClient(dev, LisAddresses[0]), taskWaiting(nullptr), accelerometerType(AccelerometerType::LIS3DH), int1Pin(p_int1Pin)
{
}

#endif

// Do a quick test to check whether the accelerometer is present, returning true if it is
bool LIS3DH::CheckPresent() noexcept
{
	interruptError = false;

#if !ACCELEROMETER_USES_SPI
	for (uint16_t addr : LisAddresses)
	{
		SetAddress(addr);
#endif
		uint8_t val;
		if (ReadRegister(LisRegister::WhoAmI, val))
		{
			if (val == WhoAmIValue_3DH)
			{
				accelerometerType = AccelerometerType::LIS3DH;
				return true;
			}
			else if (val == WhoAmIValue_3DSH)
			{
				accelerometerType = AccelerometerType::LIS3DSH;
				return true;
			}
			else if (val == WhoAmIValue_2DW)
			{
				accelerometerType = AccelerometerType::LIS2DW;
				return true;
			}
		}
#if !ACCELEROMETER_USES_SPI
	}
#endif

	return false;
}

// Return the type name of the accelerometer. Only valid after checkPresent returns true.
const char *LIS3DH::GetTypeName() const noexcept
{
	return accelerometerType.ToString();
}

uint8_t LIS3DH::ReadStatus() noexcept
{
	uint8_t val;
	return (ReadRegister(LisRegister::Status, val)) ? val : 0xFF;
}

// Configure the accelerometer to collect for the requested axis at or near the requested sampling rate and the requested resolution in bits.
// Update the sampling rate and resolution to the actual values used.
bool LIS3DH::Configure(uint16_t& samplingRate, uint8_t& resolution) noexcept
{
	bool ok;
	switch (accelerometerType.RawValue())
	{
	case AccelerometerType::LIS3DSH:
		resolution = 16;
		// We first need to make sure that the address increment bit in control register 6 is set
		ok = WriteRegister(LisRegister::CtrlReg6_3DSH, 1u << 4);
		if (ok)
		{
			// Set up control registers 1 and 4 according to the selected sampling rate and resolution. The BDA bit must be zero when using the FIFO, see app note AN3393.
			if (samplingRate == 0 || samplingRate >= 1200)
			{
				samplingRate = 1600;									// select 1600Hz if we asked for the default, or for 1200 or higher
				ctrlReg_0x20 = 0x90;
			}
			else if (samplingRate >= 600)
			{
				samplingRate = 800;										// select 800Hz if we asked for 600 or higher
				ctrlReg_0x20 = 0x80;
			}
			else
			{
				samplingRate = 400;										// set 400Hz, lower isn't useful
				ctrlReg_0x20 = 0x70;
			}

			// Set up the control registers, except set ctrlReg1 to 0 to select power down mode
			transferBuffer.data[0] = 0;									// ctrlReg4: for now select power down mode
			transferBuffer.data[1] = 0;									// ctrlReg1: SM1 disabled
			transferBuffer.data[2] = 0;									// ctrlReg2: SM2 disabled
			transferBuffer.data[3] = (1u << 3) | (1u << 6) | (1u << 5);	// ctrlReg3: interrupt 1 active high, enabled
			transferBuffer.data[4] = 0;									// ctrlReg5: anti-aliasing filter 800Hz, 4-wire SPI interface, full scale +/- 2g
			transferBuffer.data[5] = (1u << 2) | (1u << 4) | (1u << 6);	// ctrlReg6: enable fifo, watermark and watermark interrupt on INT1, address auto increment. Do not set WTM_EN.
			ok = WriteRegisters(LisRegister::Ctrl_0x20, 6);
		}
		if (ok)
		{
			// Set the fifo mode
			ok = WriteRegister(LisRegister::FifoControl, (2u << 5) | (FifoInterruptLevel - 1));		// FIFO stream mode
		}
		break;

	case AccelerometerType::LIS3DH:
	default:
		// Set up control registers 1 and 4 according to the selected sampling rate and resolution
		ctrlReg_0x20 = 0;													// collect no axes for now
		{
			uint8_t ctrlReg_0x23 = (1 << 7);								// set block data update
			if (resolution >= 12)											// if high resolution mode
			{
				resolution = 12;
				ctrlReg_0x23 |= (1 << 3);									// set HR
			}
			else if (resolution < 10)										// if low res mode
			{
				resolution = 8;
				ctrlReg_0x20 |= (1 << 3);									// set LP
			}
			else
			{
				resolution = 10;
			}

			uint8_t odr;
			if (samplingRate >= 1000)
			{
				if (resolution >= 10)
				{
					odr = 0x9;												// in 10- or 12-bit mode, so select 1.344kHz (next lowest is 400Hz)
					samplingRate = 1344;
				}
				else if (samplingRate >= 5000)
				{
					odr = 0x9;												// select 5.376kHz
					samplingRate = 5376;
				}
				else
				{
					odr = 0x8;												// select 1.6kHz
					samplingRate = 1600;
				}
			}
			else
			{
				odr = 0x7;
				samplingRate = 400;											// select 400Hz, lower is not useful
			}

			ctrlReg_0x20 |= (odr << 4);

			// Set up the control registers, except set ctrlReg1 to 0 to select power down mode
			transferBuffer.data[0] = 0;										// ctrlReg1: for now select power down mode
			transferBuffer.data[1] = 0;										// ctrlReg2: high pass filter not used
			transferBuffer.data[2] = (1u << 2);								// ctrlReg3: enable fifo watermark interrupt
			transferBuffer.data[3] = ctrlReg_0x23;
			transferBuffer.data[4] = (1u << 6);								// ctrlReg5: enable fifo
			transferBuffer.data[5] = 0;										// ctrlReg6: INT2 disabled, active high interrupts
		}
		ok = WriteRegisters(LisRegister::Ctrl_0x20, 6);
		if (ok)
		{
			// Set the fifo mode
			ok = WriteRegister(LisRegister::FifoControl, (2u << 6) | (FifoInterruptLevel - 1));		// FIFO stream mode
		}
		break;

	case AccelerometerType::LIS2DW:
		resolution = 14;
		// We first need to make sure that the address increment bit in control register 2 is set
		ok = WriteRegister(LisRegister::CtrlReg2_2DW, 1u << 2);
		if (ok)
		{
			// Set up control registers 1 and 4 according to the selected sampling rate and resolution. The BDA bit must be zero when using the FIFO, see app note AN3393.
			if (samplingRate == 0 || samplingRate >= 1200)
			{
				samplingRate = 1600;										// select 1600Hz if we asked for the default, or for 1200 or higher
				ctrlReg_0x20 = 0x90 |(1u << 2);
			}
			else if (samplingRate >= 600)
			{
				samplingRate = 800;											// select 800Hz if we asked for 600 or higher
				ctrlReg_0x20 = 0x80 |(1u << 2);
			}
			else
			{
				samplingRate = 400;											// set 400Hz, lower isn't useful
				ctrlReg_0x20 = 0x70 |(1u << 2);
			}

			// Set up the control registers, except set ctrlReg1 to 0 to select power down mode
			transferBuffer.data[0] = 0;										// ctrlReg1: for now select power down mode
			transferBuffer.data[1] = (1u << 7) | (1u << 2);					// ctrlReg2: BOOT, address auto increment
			transferBuffer.data[2] = 0;										// ctrlReg3: push-pull interrupt output, interrupt active high
			transferBuffer.data[3] = (1u << 1);								// ctrlReg4: INT1 fifo threshold interrupt enabled
			transferBuffer.data[4] = 0;										// ctrlReg5: INT2 disabled
			transferBuffer.data[5] = (1u << 2);								// ctrlReg6: max bandwidth, low pass filter path, +/-2g full scale, low noise mode
			ok = WriteRegisters(LisRegister::Ctrl_0x20, 6);
		}
		if (ok)
		{
			// Set the fifo mode
			ok = WriteRegister(LisRegister::FifoControl, (6u << 5) | (FifoInterruptLevel - 1));		// FIFO continuous mode
		}
		break;
	}
	return ok;
}

void Int1Interrupt(CallbackParameter p) noexcept;						// forward declaration

// Start collecting data, returning true if successful
bool LIS3DH:: StartCollecting(uint8_t axes) noexcept
{
	uint8_t ctrlRegValue = ctrlReg_0x20;

	// Clear the fifo
	switch (accelerometerType.RawValue())
	{
	case AccelerometerType::LIS3DH:
	case AccelerometerType::LIS3DSH:
		{
			uint8_t val;
			while (ReadRegister(LisRegister::FifoSource, val) && (val & (1u << 5)) == 0)	// while fifo not empty
			{
				if (!ReadRegisters(LisRegister::OutXL, 6))
				{
					return false;
				}
			}
		}
		ctrlRegValue |= (axes & 7);
		break;

	case AccelerometerType::LIS2DW:
		{
			uint8_t val;
			while (ReadRegister(LisRegister::FifoSource, val) && (val & 0x3F) != 0)			// while fifo not empty
			{
				if (!ReadRegisters(LisRegister::OutXL, 6))
				{
					return false;
				}
			}
		}
		break;
	}


	// Version 1.1 tool boards need the pullup resistor enabled on the interrupt pin.
	// For other tool board versions and for the SAMMYC21, pulling it up allows us to check for a disconnected pin
	pinMode(int1Pin, INPUT_PULLUP);

	totalNumRead = 0;

	// Before we enable data collection, check that the interrupt line is low
	delayMicroseconds(5);
	interruptError = digitalRead(int1Pin);
	if (interruptError)
	{
		return false;
	}

	const bool ok = WriteRegister(LisRegister::Ctrl_0x20, ctrlRegValue);
	return ok && attachInterrupt(int1Pin, Int1Interrupt, InterruptMode::rising, CallbackParameter(this));
}

// Collect some 8-bit data from the FIFO, suspending until the data is available
unsigned int LIS3DH::CollectData(const uint16_t **collectedData, uint16_t &dataRate, bool &overflowed) noexcept
{
	// Wait until we have some data
	taskWaiting = TaskBase::GetCallerTaskHandle();
	while (!digitalRead(int1Pin))
	{
		TaskBase::Take();
	}
	taskWaiting = nullptr;

	// Get the fifo status to see how much data we can read and whether the fifo overflowed
	uint8_t fifoStatus;
	if (!ReadRegister(LisRegister::FifoSource, fifoStatus))
	{
		return 0;
	}

	uint8_t numToRead;
	switch (accelerometerType.RawValue())
	{
	case AccelerometerType::LIS3DH:
	case AccelerometerType::LIS3DSH:
	default:
		numToRead = fifoStatus & 0x1F;
		if (numToRead == 0 && (fifoStatus & 0x20) == 0)
		{
			numToRead = 32;
		}
		break;

	case AccelerometerType::LIS2DW:
		numToRead = fifoStatus & 0x3F;
		break;
	}

	if (numToRead != 0)
	{
		// Read the data
		// When the auto-increment bit is set, after reading register 0x2D it wraps back to 0x28
		// The datasheet doesn't mention this but ST app note AN3308 does
		if (!ReadRegisters(LisRegister::OutXL, 6 * numToRead))
		{
			return 0;
		}

		*collectedData = reinterpret_cast<const uint16_t*>(transferBuffer.data);
		overflowed = (fifoStatus & 0x40) != 0;
		dataRate = (totalNumRead == 0) ? 0 : (totalNumRead * (uint64_t)StepTimer::StepClockRate)/(lastInterruptTime - firstInterruptTime);
		totalNumRead += numToRead;
	}
	return numToRead;
}

// Stop collecting data
void LIS3DH::StopCollecting() noexcept
{
	WriteRegister(LisRegister::Ctrl_0x20, 0);
}

bool LIS3DH::ReadRegisters(LisRegister reg, size_t numToRead) noexcept
{
#if ACCELEROMETER_USES_SPI
	if (!Select(Lis3dSpiTimeout))
	{
		return false;
	}
	delayMicroseconds(1);
	// On the LIS3DH, bit 6 must be set to 1 to auto-increment the address when doing reading multiple registers
	// On the LIS3DSH and LIS2DW, bit 6 is an extra register address bit, so we must not set it.
	// So that we can read the WHO_AM_I register of both chips before we know which chip we have, only set bit 6 if we have a LIS3DH and we are reading multiple registers.
	transferBuffer.reg = (uint8_t)reg | ((numToRead < 2 || accelerometerType != AccelerometerType::LIS3DH) ? 0x80 : 0xC0);
	const bool ret = TransceivePacket(&transferBuffer.reg, &transferBuffer.reg, 1 + numToRead);
	Deselect();
	return ret;
#else
	// On the LIS3DH, bit 6 of the first byte must be set to 1 to auto-increment the address when doing reading multiple registers
	// On the LIS3DSH and LIS2DW, bit 6 is an extra register address bit, so we must not set it.
	// So that we can read the WHO_AM_I register of both chips before we know which chip we have, only set bit 6 if we have a LIS3DH and we are reading multiple registers.
	const uint8_t regAddr = (numToRead < 2 || accelerometerType != AccelerometerType::LIS3DH) ? (uint8_t)reg : (uint8_t)reg | 0x80;
	transferBuffer.reg = regAddr;
	return Transfer(&transferBuffer.reg, 1, numToRead, Lis3dI2CTimeout);
#endif
}

bool LIS3DH::WriteRegisters(LisRegister reg, size_t numToWrite) noexcept
{
	if ((uint8_t)reg < 0x1E)						// don't overwrite the factory calibration values
	{
		return false;
	}

#if ACCELEROMETER_USES_SPI
	if (!Select(Lis3dSpiTimeout))
	{
		return false;
	}
	transferBuffer.reg = (numToWrite < 2 || accelerometerType != AccelerometerType::LIS3DH) ? (uint8_t)reg : (uint8_t)reg | 0x40;		// set auto increment bit if LIS3DH
	const bool ret = TransceivePacket(&transferBuffer.reg, &transferBuffer.reg, 1 + numToWrite);
	Deselect();
	return ret;
#else
	const uint8_t regAddr = (numToWrite < 2 || accelerometerType != AccelerometerType::LIS3DH) ? (uint8_t)reg : (uint8_t)reg | 0x80;	// set auto increment bit if LIS3DH
	transferBuffer.reg = regAddr;
	return Transfer(&transferBuffer.reg, 1 + numToWrite, 0, Lis3dI2CTimeout);
#endif
}

bool LIS3DH::ReadRegister(LisRegister reg, uint8_t& val) noexcept
{
	const bool ret = ReadRegisters(reg, 1);
	if (ret)
	{
		val = transferBuffer.data[0];
	}
	return ret;
}

bool LIS3DH::WriteRegister(LisRegister reg, uint8_t val) noexcept
{
	transferBuffer.data[0] = val;
	return WriteRegisters(reg, 1);
}

void LIS3DH::Int1Isr() noexcept
{
	const uint32_t now = StepTimer::GetTimerTicks();
	if (totalNumRead == 0)
	{
		firstInterruptTime = now;
	}
	lastInterruptTime = now;
	TaskBase::GiveFromISR(taskWaiting);
	taskWaiting = nullptr;
}

void Int1Interrupt(CallbackParameter p) noexcept
{
	static_cast<LIS3DH*>(p.vp)->Int1Isr();
}

#endif

// End
