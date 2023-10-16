/*
 * SharedI2CClient.cpp
 *
 *  Created on: 14 Mar 2021
 *      Author: David
 */

#include <Hardware/SharedI2CClient.h>

#if SUPPORT_I2C_SENSORS

SharedI2CClient::SharedI2CClient(SharedI2CMaster& dev, uint16_t addr) noexcept : device(dev), address(addr)
{
}

bool SharedI2CClient::Transfer(uint8_t *buffer, size_t numToWrite, size_t numToRead, uint32_t timeout) noexcept
{
	if (!device.Take(timeout))
	{
		return false;
	}
	const bool ret = device.Transfer(address, *buffer, buffer + 1, numToWrite, numToRead);
	device.Release();
	return ret;
}

#endif

// End
