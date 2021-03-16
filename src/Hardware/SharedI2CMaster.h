/*
 * SharedI2CMaster.h
 *
 *  Created on: 13 Mar 2021
 *      Author: David
 */

#ifndef SRC_HARDWARE_SHAREDI2CMASTER_H_
#define SRC_HARDWARE_SHAREDI2CMASTER_H_

#include <RepRapFirmware.h>

#if SUPPORT_I2C_SENSORS

#include <RTOSIface/RTOSIface.h>

class SharedI2CMaster
{
public:
	struct ErrorCounts
	{
		uint32_t naks;
		uint32_t sendTimeouts;
		uint32_t recvTimeouts;
		uint32_t finishTimeouts;
		uint32_t resets;

		void Clear() noexcept;
	};

	SharedI2CMaster(uint8_t sercomNum) noexcept;

	void SetClockFrequency(uint32_t freq) const noexcept;
	bool Transfer(uint16_t address, uint8_t firstByte, uint8_t *buffer, size_t numToWrite, size_t numToRead) noexcept;
	ErrorCounts GetErrorCounts(bool clear) noexcept;

	bool Take(uint32_t timeout) noexcept { return mutex.Take(timeout); }		// get ownership of this SPI, return true if successful
	void Release() noexcept { mutex.Release(); }

	void Interrupt() noexcept;

private:
	enum class I2cState : uint8_t
	{
		idle = 0, sendingAddressForWrite, writing, sendingTenBitAddressForRead, sendingAddressForRead, reading, busError, nakError, otherError
	};

	void Enable() const noexcept;
	void Disable() const noexcept;
	bool WaitForStatus(uint8_t statusBits) noexcept;
	bool WaitForSend() noexcept;
	bool WaitForReceive() noexcept;
	bool InternalTransfer(uint16_t address, uint8_t firstByte, uint8_t *buffer, size_t numToWrite, size_t numToRead) noexcept;
	void ProtocolError()  noexcept;

	Sercom * const hardware;
	TaskHandle taskWaiting;
	ErrorCounts errorCounts;			// error counts
	Mutex mutex;

	uint8_t *transferBuffer;
	size_t numLeftToRead, numLeftToWrite;
	uint16_t currentAddress;
	uint8_t firstByteToWrite;
	volatile I2cState state;
};

#endif

#endif /* SRC_HARDWARE_SHAREDI2CMASTER_H_ */