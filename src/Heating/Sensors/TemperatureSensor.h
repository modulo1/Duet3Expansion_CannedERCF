#ifndef TEMPERATURESENSOR_H
#define TEMPERATURESENSOR_H

#include "RepRapFirmware.h"
#include "Heating/TemperatureError.h"		// for result codes
#include "GCodes/GCodeResult.h"
#include <Hardware/IoPorts.h>
#include <CanId.h>

class CanMessageGenericParser;
class CanTemperatureReport;

class TemperatureSensor
{
public:
	TemperatureSensor(unsigned int sensorNum, const char *type);

	// Virtual destructor
	virtual ~TemperatureSensor();

	// Get the latest temperature reading
	TemperatureError GetLatestTemperature(float& t);

	// Configure the sensor from M305 parameters.
	// If we find any parameters, process them and return true. If an error occurs while processing them, return error and write an error message to 'reply.
	// If we find no relevant parameters, report the current parameters to 'reply' and return ok.
	virtual GCodeResult Configure(const CanMessageGenericParser& parser, const StringRef& reply);

	// Return the sensor type
	const char *GetSensorType() const { return sensorType; }

	// Return the sensor number
	unsigned int GetSensorNumber() const { return sensorNumber; }

	// Return the code for the most recent error
	TemperatureError GetLastError() const { return lastRealError; }

	// Copy the basic details to the reply buffer
	void CopyBasicDetails(const StringRef& reply) const;

	// Get/set the next sensor in the linked list
	TemperatureSensor *GetNext() const { return next; }
	void SetNext(TemperatureSensor *n) { next = n; }

	// Get the expansion board address. Overridden for remote sensors.
	virtual CanAddress GetBoardAddress() const;

	// Update the temperature, if it is a remote sensor. Overridden in class RemoteSensor.
	virtual void UpdateRemoteTemperature(CanAddress src, const CanTemperatureReport& report);

	// Factory method
	static TemperatureSensor *Create(unsigned int sensorNum, const char *typeName, const StringRef& reply);

	// Try to get a temperature reading
	virtual void Poll() = 0;

protected:
	void SetResult(float t, TemperatureError rslt);
	void SetResult(TemperatureError rslt);

	static TemperatureError GetPT100Temperature(float& t, uint16_t ohmsx100);		// shared function used by two derived classes

private:
	static constexpr uint32_t TemperatureReadingTimeout = 2000;			// any reading older than this number of milliseconds is considered unreliable

	TemperatureSensor *next;
	unsigned int sensorNumber;					// the number of this sensor
	const char * const sensorType;
	float lastTemperature;
	uint32_t whenLastRead;
	TemperatureError lastResult, lastRealError;
};

#endif // TEMPERATURESENSOR_H