/*
 * Thermistor.cpp
 * Reads temperature from a thermistor or a PT1000 sensor connected to a thermistor port
 *
 *  Created on: 10 Nov 2016
 *      Author: David
 */

#include "Thermistor.h"
#include "Platform.h"
#include "CanMessageGenericParser.h"

// The Steinhart-Hart equation for thermistor resistance is:
// 1/T = A + B ln(R) + C [ln(R)]^3
//
// The simplified (beta) equation assumes C=0 and is:
// 1/T = A + (1/Beta) ln(R)
//
// The parameters that can be configured in RRF are R25 (the resistance at 25C), Beta, and optionally C.

// Create an instance with default values
Thermistor::Thermistor(unsigned int sensorNum, bool p_isPT1000)
	: SensorWithPort(sensorNum, (p_isPT1000) ? "PT1000" : "Thermistor"), adcFilterChannel(-1),
	  r25(DefaultR25), beta(DefaultBeta), shC(DefaultShc), seriesR(DefaultThermistorSeriesR), isPT1000(p_isPT1000)
#if !HAS_VREF_MONITOR
		, adcLowOffset(0), adcHighOffset(0)
#endif
{
	CalcDerivedParameters();
}

// Configure the temperature sensor
GCodeResult Thermistor::Configure(const CanMessageGenericParser& parser, const StringRef& reply)
{
	bool seen = false;
	if (!ConfigurePort(parser, reply, PinAccess::readAnalog, seen))
	{
		return GCodeResult::error;
	}

	seen = parser.GetFloatParam('R', seriesR) || seen;
	if (!isPT1000)
	{
		seen = parser.GetFloatParam('B', beta) || seen;
		if (seen)
		{
			shC = 0.0;						// if user changes B and doesn't define C, assume C=0
		}
		seen = parser.GetFloatParam('C', shC) || seen;
		seen = parser.GetFloatParam('T', r25) || seen;
		if (seen)
		{
			CalcDerivedParameters();
		}
	}

#if !HAS_VREF_MONITOR
	seen = parser.GetIntValue('L', adcLowOffset) || seen;
	seen = parser.GetIntValue('H', adcHighOffset) || seen;
#endif

	if (seen)
	{
		adcFilterChannel = Platform::GetAveragingFilterIndex(port);
		if (adcFilterChannel >= 0)
		{
			Platform::GetAdcFilter(adcFilterChannel).Init((1u << AnalogIn::AdcBits) - 1);
		}
	}
	else
	{
		CopyBasicDetails(reply);
		if (isPT1000)
		{
			// For a PT1000 sensor, only the series resistor is configurable
			reply.catf(", R:%.1f", (double)seriesR);
		}
		else
		{
			reply.catf(", T:%.1f B:%.1f C:%.2e R:%.1f", (double)r25, (double)beta, (double)shC, (double)seriesR);
		}
#if !HAS_VREF_MONITOR
		reply.catf(" L:%d H:%d", adcLowOffset, adcHighOffset);
#endif
	}

	return GCodeResult::ok;
}

// Get the temperature
void Thermistor::Poll()
{
	const volatile ThermistorAveragingFilter& tempFilter = Platform::GetAdcFilter(adcFilterChannel);

#if HAS_VREF_MONITOR
	// Use the actual VSSA and VREF values read by the ADC
	const volatile ThermistorAveragingFilter& vrefFilter = Platform::GetAdcFilter(VrefFilterIndex);
	const volatile ThermistorAveragingFilter& vssaFilter = Platform::GetAdcFilter(VssaFilterIndex);
	if (tempFilter.IsValid() && vrefFilter.IsValid() && vssaFilter.IsValid())
	{
		const int32_t averagedVssaReading = vssaFilter.GetSum()/(vssaFilter.NumAveraged() >> Thermistor::AdcOversampleBits);
		const int32_t averagedVrefReading = vrefFilter.GetSum()/(vssaFilter.NumAveraged() >> Thermistor::AdcOversampleBits);

		// VREF is the measured voltage at VREF less the drop of a 15 ohm resistor. Assume that the maximum load is four 2K2 resistors and one 4K7 resistor to ground = 492 ohms.
		// VSSA is the voltage measured across the VSSA fuse. We assume the same maximum load and the same 15 ohms maximum resistance for the fuse.
		// Assume a maximum ADC reading offset of 100.
		constexpr int32_t maxDrop = ((4096 << Thermistor::AdcOversampleBits) * 15)/492 + (100 << Thermistor::AdcOversampleBits);

		if (averagedVrefReading < (4096 << Thermistor::AdcOversampleBits) - maxDrop)
		{
			SetResult(TemperatureError::badVref);
		}
		else if (averagedVssaReading > maxDrop)
		{
			SetResult(TemperatureError::badVssa);
		}
		else
		{
#else
	if (tempFilter.IsValid())
	{
		{
#endif
			const int32_t averagedTempReading = tempFilter.GetSum()/(tempFilter.NumAveraged() >> Thermistor::AdcOversampleBits);

			// Calculate the resistance
#if HAS_VREF_MONITOR
			const float denom = (float)(averagedVrefReading - averagedTempReading);
#else
			const int32_t averagedVrefReading = AdcRange + 2 * adcHighOffset;		// double the offset because we increased AdcOversampleBits from 1 to 2
			const float denom = (float)(averagedVrefReading - averagedTempReading) - 0.5;
#endif
			if (denom <= 0.0)
			{
				SetResult(ABS_ZERO, TemperatureError::openCircuit);
			}
			else
			{

#if HAS_VREF_MONITOR
				const float resistance = seriesR * (float)(averagedTempReading - averagedVssaReading)/denom;
#else
				const int32_t averagedVssaReading = 2 * adcLowOffset;					// double the offset because we increased AdcOversampleBits from 1 to 2
				float resistance = seriesR * ((float)(averagedTempReading - averagedVssaReading) + 0.5)/denom;
# ifdef DUET_NG
				// The VSSA PTC fuse on the later Duets has a resistance of a few ohms. I measured 1.0 ohms on two revision 1.04 Duet WiFi boards.
				resistance -= 1.0;														// assume 1.0 ohms and only one PT1000 sensor
# endif
#endif
				if (isPT1000)
				{
					// We want 100 * the equivalent PT100 resistance, which is 10 * the actual PT1000 resistance
					const uint16_t ohmsx100 = (uint16_t)rintf(constrain<float>(resistance * 10, 0.0, 65535.0));
					float t;
					const TemperatureError sts = GetPT100Temperature(t, ohmsx100);
					SetResult(t, sts);
				}
				else
				{
					// Else it's a thermistor
					const float logResistance = log(resistance);
					const float recipT = shA + shB * logResistance + shC * logResistance * logResistance * logResistance;
					const float temp =  (recipT > 0.0) ? (1.0/recipT) + ABS_ZERO : BadErrorTemperature;

					if (temp < MinimumConnectedTemperature)
					{
						// thermistor is disconnected
						SetResult(ABS_ZERO, TemperatureError::openCircuit);
					}
					else
					{
						SetResult(temp, TemperatureError::success);
					}
				}
			}
		}
	}
	else
	{
		// Filter is not ready yet
		SetResult(TemperatureError::notReady);
	}
}

// Calculate shA and shB from the other parameters
void Thermistor::CalcDerivedParameters()
{
	shB = 1.0/beta;
	const float lnR25 = logf(r25);
	shA = 1.0/(25.0 - ABS_ZERO) - shB * lnR25 - shC * lnR25 * lnR25 * lnR25;
}

// End