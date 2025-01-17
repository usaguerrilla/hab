#include "aprs_board_impl.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef UNIT_TEST
    #include "tiva_c.h"
#else
    #include "stubs\tiva_c.h"
#endif

#include "uart.h"
#include "timer.h"
#include "common.h"

// ran out of memory (Keil IDE limitation to 32Kb so couldn't use good tables)
// will address this later once we move to use GCC or something like it
#define SINE(v)            sinf(v)
#define COSINE_G_THAN_0(v) (cosf(v) >= 0)
#define INVERSE_SINE(v)    asinf(v)

const Callsign CALLSIGN_SOURCE = 
{
    {"HABHAB"},
    '\xF6' // 111 1011 0
           //          ^ not a last address
           //     ^^^^ SSID (11 - balloon)
           // ^^^ some reserved values and command/response
};

const Callsign CALLSIGN_DESTINATION_1 = 
{
    {"WIDE1 "},
    '\xE2' // 111 0001 0
           //          ^ not a last address
           //     ^^^^ SSID (1 - wide1-1)
           // ^^^ some reserved values and command/response
};

const Callsign CALLSIGN_DESTINATION_2 = 
{
    {"WIDE2 "},
    '\xE5' // 111 0010 1
           //          ^ last address
           //     ^^^^ SSID (2 - wide2-2)
           // ^^^ some reserved values and command/response
};

bool g_sendingMessage = false;

uint16_t g_leadingOnesLeft = 0;
uint16_t g_leadingWarmUpLeft = 0;
BitstreamPos g_currentBitstreamPos = { 0 };
BitstreamPos g_currentBitstreamSize = { 0 };
uint8_t g_currentBitstream[APRS_BITSTREAM_MAX_LEN] = { 0 };

bool g_currentFrequencyIsF1200 = true;

float g_currentF1200Frame = 0;
float g_currentF2200Frame = 0;
uint8_t g_currentSymbolPulsesCount = 0;

uint16_t g_aprsMessageId = 0;
uint8_t g_aprsPayloadBuffer[APRS_PAYLOAD_LEN];

void initializeAprs(void)
{
    initializeAprsHardware(PWM_PERIOD, PWM_MIN_PULSE_WIDTH);
}

bool sendAprsMessage(GpsDataSource gpsDataSource, const GpsData* pGpsData, const Telemetry* pTelemetry)
{
    if (g_sendingMessage)
    {
        return false;
    }

    g_leadingOnesLeft = LEADING_ONES_COUNT_TO_CANCEL_PREVIOUS_PACKET;
    g_leadingWarmUpLeft = LEADING_WARMUP_AMPLITUDE_DC_PULSES_COUNT;
    
    g_currentBitstreamSize.bitstreamCharIdx = 0;
    g_currentBitstreamSize.bitstreamCharBitIdx = 0;

    g_currentBitstreamPos.bitstreamCharIdx = 0;
    g_currentBitstreamPos.bitstreamCharBitIdx = 0;

    g_currentF1200Frame = 0;
    g_currentF2200Frame = 0;
    g_currentFrequencyIsF1200 = true;
    g_currentSymbolPulsesCount = F1200_PWM_PULSES_COUNT_PER_SYMBOL;

    if (generateMessage(&CALLSIGN_SOURCE,
                        gpsDataSource,
                        pGpsData,
                        pTelemetry,
                        g_currentBitstream,
                        APRS_BITSTREAM_MAX_LEN,
                        &g_currentBitstreamSize))
    {
        g_sendingMessage = true;
        enableHx1();
        enableAprsPwm();
        return true;
    }
    else
    {
        return false;
    }
}

void advanceBitstreamBit(BitstreamPos* pResultBitstreamSize)
{
    if (pResultBitstreamSize->bitstreamCharBitIdx >= 7)
    {
        ++pResultBitstreamSize->bitstreamCharIdx;
        pResultBitstreamSize->bitstreamCharBitIdx = 0;
    }
    else
    {
        ++pResultBitstreamSize->bitstreamCharBitIdx;
    }
}

bool encodeAndAppendBits(uint8_t* pBitstreamBuffer,
                         uint16_t maxBitstreamBufferLen,
                         EncodingData* pEncodingData,
                         const uint8_t* pMessageData,
                         uint16_t messageDataSize,
                         STUFFING_TYPE stuffingType,
                         FCS_TYPE fcsType,
                         SHIFT_ONE_LEFT_TYPE shiftOneLeftType)
{
    if (!pBitstreamBuffer || !pEncodingData || maxBitstreamBufferLen < messageDataSize)
    {
        return false;
    }
    if (messageDataSize == 0)
    {
        return true;
    }
    if (!pMessageData)
    {
        return false;
    }

    for (uint16_t iByte = 0; iByte < messageDataSize; ++iByte)
    {
        uint8_t currentByte = pMessageData[iByte];

        if (shiftOneLeftType == SHIFT_ONE_LEFT)
        {
            currentByte <<= 1;
        }

        for (uint8_t iBit = 0; iBit < 8; ++iBit)
        {
            const uint8_t currentBit = currentByte & (1 << iBit);

            // add FCS calculation based on tables
            // to improve speed (need to migrate to GCC due to 
            // 32Kb application size limit in Keil)

            if (fcsType == FCS_CALCULATE)
            {
                const uint16_t shiftBit = pEncodingData->fcs & 0x0001;
                pEncodingData->fcs = pEncodingData->fcs >> 1;
                if (shiftBit != ((currentByte >> iBit) & 0x01))
                {
                    pEncodingData->fcs ^= FCS_POLYNOMIAL;
                }
            }

            if (currentBit)
            {
                if (pEncodingData->bitstreamSize.bitstreamCharIdx >= maxBitstreamBufferLen)
                {
                    return false;
                }
                // as we are encoding 1 keep current bit as is
                if (pEncodingData->lastBit)
                {
                    pBitstreamBuffer[pEncodingData->bitstreamSize.bitstreamCharIdx] |= 1 << (pEncodingData->bitstreamSize.bitstreamCharBitIdx);
                }
                else
                {
                    pBitstreamBuffer[pEncodingData->bitstreamSize.bitstreamCharIdx] &= ~(1 << (pEncodingData->bitstreamSize.bitstreamCharBitIdx));
                }

                advanceBitstreamBit(&pEncodingData->bitstreamSize);

                if (stuffingType == ST_PERFORM_STUFFING)
                {
                    ++pEncodingData->numberOfOnes;
                    
                    if (pEncodingData->numberOfOnes == 5)
                    {
                        if (pEncodingData->bitstreamSize.bitstreamCharIdx >= maxBitstreamBufferLen)
                        {
                            return false;
                        }

                        // we need to insert 0 after 5 consecutive ones
                        if (pEncodingData->lastBit)
                        {
                            pBitstreamBuffer[pEncodingData->bitstreamSize.bitstreamCharIdx] &= ~(1 << (pEncodingData->bitstreamSize.bitstreamCharBitIdx));
                            pEncodingData->lastBit = 0;
                        }
                        else
                        {
                            pBitstreamBuffer[pEncodingData->bitstreamSize.bitstreamCharIdx] |= 1 << (pEncodingData->bitstreamSize.bitstreamCharBitIdx);
                            pEncodingData->lastBit = 1;
                        }
                        
                        pEncodingData->numberOfOnes = 0;
                        
                        advanceBitstreamBit(&pEncodingData->bitstreamSize); // insert zero as we had 5 ones
                    }
                }
            }
            else
            {
                if (pEncodingData->bitstreamSize.bitstreamCharIdx >= maxBitstreamBufferLen)
                {
                    return false;
                }
                
                // as we are encoding 0 we need to flip bit
                if (pEncodingData->lastBit)
                {
                    pBitstreamBuffer[pEncodingData->bitstreamSize.bitstreamCharIdx] &= ~(1 << (pEncodingData->bitstreamSize.bitstreamCharBitIdx));
                    pEncodingData->lastBit = 0;
                }
                else
                {
                    pBitstreamBuffer[pEncodingData->bitstreamSize.bitstreamCharIdx] |= 1 << (pEncodingData->bitstreamSize.bitstreamCharBitIdx);
                    pEncodingData->lastBit = 1;
                }

                advanceBitstreamBit(&pEncodingData->bitstreamSize);

                if (stuffingType == ST_PERFORM_STUFFING)
                {
                    pEncodingData->numberOfOnes = 0;
                }
            }
        }
    }

    if (stuffingType == ST_NO_STUFFING)
    {
        // resert ones as we didn't do any stuffing while sending this data
        pEncodingData->numberOfOnes = 0;
    }

    return true;
}

uint8_t createPacketPayload(GpsDataSource gpsDataSource, const GpsData* pGpsData, const Telemetry* pTelemetry, uint16_t messageIdx, uint8_t* pBuffer, uint8_t bufferSize)
{
    uint8_t bufferStartIdx = 0;
    
    if (pGpsData->gpggaData.latitude.isValid && pGpsData->gpggaData.longitude.isValid)
    {
        if (pGpsData->gpggaData.utcTime.isValid)
        {
            if (bufferStartIdx + 8 > bufferSize)
            {
                return 0;
            }

            bufferStartIdx += sprintf((char*) &pBuffer[bufferStartIdx],
                                      "@%02u%02u%02uz",
                                      pGpsData->gpggaData.utcTime.hours,
                                      pGpsData->gpggaData.utcTime.minutes,
                                      pGpsData->gpggaData.utcTime.seconds / 100);
        }
        else
        {
            if (bufferStartIdx + 1 > bufferSize)
            {
                return 0;
            }

            pBuffer[bufferStartIdx++] = '!';
        }

        if (bufferStartIdx + 19 > bufferSize)
        {
            return 0;
        }

        const uint32_t latMinutesWhole = pGpsData->gpggaData.latitude.minutes / 1000000;
        const uint32_t latMinutesFraction = (pGpsData->gpggaData.latitude.minutes - latMinutesWhole * 1000000) / 10000;

        const uint32_t lonMinutesWhole = pGpsData->gpggaData.longitude.minutes / 1000000;
        const uint32_t lonMinutesFraction = (pGpsData->gpggaData.longitude.minutes - lonMinutesWhole * 1000000) / 10000;

        bufferStartIdx += sprintf((char*) &pBuffer[bufferStartIdx],
                                  "%02u%02u.%02u%1c/%03u%02u.%02u%1c",
                                  pGpsData->gpggaData.latitude.degrees,
                                  latMinutesWhole,
                                  latMinutesFraction,
                                  pGpsData->gpggaData.latitude.hemisphere,
                                  pGpsData->gpggaData.longitude.degrees,
                                  lonMinutesWhole,
                                  lonMinutesFraction,
                                  pGpsData->gpggaData.longitude.hemisphere);

        if (bufferStartIdx + 7 > bufferSize)
        {
            return 0;
        }

        bufferStartIdx += sprintf((char*) &pBuffer[bufferStartIdx],
                                  ">%03u/%03u",
                                  (uint32_t) (pGpsData->gpvtgData.trueCourseDegrees / 10),
                                  (uint32_t) (pGpsData->gpvtgData.speedKph / 10));
    }

    if (bufferStartIdx + 42 > bufferSize)
    {
        return 0;
    }

    bufferStartIdx += sprintf((char*) &pBuffer[bufferStartIdx],
                              "T#%03u,%03u,%03u,%03u,000,000,00000000 a=%05u",
                              messageIdx, 
                              gpsDataSource,
                              pTelemetry->cpuTemperature / 10,
                              pTelemetry->voltage / 10,
                              (uint32_t) pGpsData->gpggaData.altitudeMslMeters / 10);

    return bufferStartIdx;
}

bool generateMessage(const Callsign* pCallsignSource,
                     GpsDataSource gpsDataSource,
                     const GpsData* pGpsData,
                     const Telemetry* pTelemetry,
                     uint8_t* bitstreamBuffer,
                     uint16_t maxBitstreamBufferLen,
                     BitstreamPos* pBitstreamSize)
{
    if (!pBitstreamSize || !pCallsignSource || !pGpsData || !bitstreamBuffer)
    {
        return false;
    }
    
    EncodingData encodingData = { 0 };
    encodingData.lastBit = 1;
    encodingData.fcs = FCS_INITIAL_VALUE;

    for (uint8_t i = 0; i < PREFIX_FLAGS_COUNT; ++i)
    {
        encodeAndAppendBits(bitstreamBuffer, maxBitstreamBufferLen, &encodingData, (const uint8_t*) "\x7E", 1, ST_NO_STUFFING, FCS_NONE, SHIFT_ONE_LEFT_NO);
    }

    // addresses to and from
    
    encodeAndAppendBits(bitstreamBuffer, maxBitstreamBufferLen, &encodingData, CALLSIGN_DESTINATION_1.callsign, 6, ST_PERFORM_STUFFING, FCS_CALCULATE, SHIFT_ONE_LEFT);
    encodeAndAppendBits(bitstreamBuffer, maxBitstreamBufferLen, &encodingData, &CALLSIGN_DESTINATION_1.ssid, 1, ST_PERFORM_STUFFING, FCS_CALCULATE, SHIFT_ONE_LEFT_NO);
    encodeAndAppendBits(bitstreamBuffer, maxBitstreamBufferLen, &encodingData, pCallsignSource->callsign, 6, ST_PERFORM_STUFFING, FCS_CALCULATE, SHIFT_ONE_LEFT);
    encodeAndAppendBits(bitstreamBuffer, maxBitstreamBufferLen, &encodingData, &pCallsignSource->ssid, 1, ST_PERFORM_STUFFING, FCS_CALCULATE, SHIFT_ONE_LEFT_NO);
    encodeAndAppendBits(bitstreamBuffer, maxBitstreamBufferLen, &encodingData, CALLSIGN_DESTINATION_2.callsign, 6, ST_PERFORM_STUFFING, FCS_CALCULATE, SHIFT_ONE_LEFT);
    encodeAndAppendBits(bitstreamBuffer, maxBitstreamBufferLen, &encodingData, &CALLSIGN_DESTINATION_2.ssid, 1, ST_PERFORM_STUFFING, FCS_CALCULATE, SHIFT_ONE_LEFT_NO);

    // control bytes
    
    encodeAndAppendBits(bitstreamBuffer, maxBitstreamBufferLen, &encodingData, (const uint8_t*) "\x03", 1, ST_PERFORM_STUFFING, FCS_CALCULATE, SHIFT_ONE_LEFT_NO);
    encodeAndAppendBits(bitstreamBuffer, maxBitstreamBufferLen, &encodingData, (const uint8_t*) "\xF0", 1, ST_PERFORM_STUFFING, FCS_CALCULATE, SHIFT_ONE_LEFT_NO);

    // packet contents
    
    const uint8_t bufferSize = createPacketPayload(gpsDataSource, pGpsData, pTelemetry, g_aprsMessageId++, g_aprsPayloadBuffer, APRS_PAYLOAD_LEN);
    if (bufferSize == 0)
    {
        return false;
    }
#ifdef DUMP_DATA_TO_UART0
    writeString(CHANNEL_OUTPUT, "aprs - ");
    if (!writeMessageBuffer(CHANNEL_OUTPUT, g_aprsPayloadBuffer, bufferSize))
    {
        return false;
    }
    writeString(CHANNEL_OUTPUT, "\r\n");
#endif
    encodeAndAppendBits(bitstreamBuffer, maxBitstreamBufferLen, &encodingData, g_aprsPayloadBuffer, bufferSize, ST_PERFORM_STUFFING, FCS_CALCULATE, SHIFT_ONE_LEFT_NO);
    
    // fcs

    encodingData.fcs ^= FCS_POST_PROCESSING_XOR_VALUE;
    uint8_t fcsByte = encodingData.fcs & 0x00FF; // get low byte
    encodeAndAppendBits(bitstreamBuffer, maxBitstreamBufferLen, &encodingData, &fcsByte, 1, ST_PERFORM_STUFFING, FCS_NONE, SHIFT_ONE_LEFT_NO);
    fcsByte = (encodingData.fcs >> 8) & 0x00FF; // get high byte
    encodeAndAppendBits(bitstreamBuffer, maxBitstreamBufferLen, &encodingData, &fcsByte, 1, ST_PERFORM_STUFFING, FCS_NONE, SHIFT_ONE_LEFT_NO);

    // sufix flags

    for (uint8_t i = 0; i < SUFFIX_FLAGS_COUNT; ++i)
    {
        encodeAndAppendBits(bitstreamBuffer, maxBitstreamBufferLen, &encodingData, (const uint8_t*) "\x7E", 1, ST_NO_STUFFING, FCS_NONE, SHIFT_ONE_LEFT_NO);
    }

    *pBitstreamSize = encodingData.bitstreamSize;

    return true;
}

float normalizePulseWidth(float width)
{
    if (width < PWM_MIN_PULSE_WIDTH)
    {
        return PWM_MIN_PULSE_WIDTH;
    }
    else if (width > PWM_MAX_PULSE_WIDTH)
    {
        return PWM_MAX_PULSE_WIDTH;
    }
    return width;
}

void Pwm10Handler(void)
{
    clearAprsPwmInterrupt();
    
    if (g_leadingWarmUpLeft)
    {
        // make sure HX1 has a chance to warm up
        setAprsPwmPulseWidth(PWM_MIN_PULSE_WIDTH);
        --g_leadingWarmUpLeft;
    }
    else
    {
        if (g_currentSymbolPulsesCount >= F1200_PWM_PULSES_COUNT_PER_SYMBOL)
        {
            g_currentSymbolPulsesCount = 0;

            if (!g_sendingMessage || (g_currentBitstreamPos.bitstreamCharIdx >= g_currentBitstreamSize.bitstreamCharIdx && 
                                      g_currentBitstreamPos.bitstreamCharBitIdx >= g_currentBitstreamSize.bitstreamCharBitIdx))
            {
                disableAprsPwm();
                disableHx1();
                setAprsPwmPulseWidth(PWM_MIN_PULSE_WIDTH);
                g_sendingMessage = false;
                return;
            }
            else if (g_leadingOnesLeft)
            {
                // send ones to stabilize HX1 and cancel any previosuly not-fully received APRS packets
                g_currentFrequencyIsF1200 = true;
                --g_leadingOnesLeft;
            }
            else
            {
                // bit stream is already AFSK encoded so we simply send ones and zeroes as is
                const bool isOne = g_currentBitstream[g_currentBitstreamPos.bitstreamCharIdx] & (1 << g_currentBitstreamPos.bitstreamCharBitIdx);

                // make sure new zero bit frequency is 2200
                if (!isOne && g_currentFrequencyIsF1200)
                {
                    const float triagArg = ANGULAR_FREQUENCY_F1200 * g_currentF1200Frame;
                    const float pulseWidth1200 = normalizePulseWidth(AMPLITUDE_SHIFT + AMPLITUDE_SCALER * SINE(triagArg));
                    const bool pulse1200Positive = COSINE_G_THAN_0(triagArg);

                    if (pulse1200Positive)
                    {
                        g_currentF2200Frame = RECIPROCAL_ANGULAR_FREQUENCY_F2200 * INVERSE_SINE(RECIPROCAL_AMPLITUDE_SCALER * (pulseWidth1200 - AMPLITUDE_SHIFT));
                    }
                    else
                    {
                        g_currentF2200Frame = HALF_PERIOD_F2200 - RECIPROCAL_ANGULAR_FREQUENCY_F2200 * INVERSE_SINE(RECIPROCAL_AMPLITUDE_SCALER * (pulseWidth1200 - AMPLITUDE_SHIFT));
                    }
                    
                    if (g_currentF2200Frame < 0)
                    {
                        g_currentF2200Frame += F2200_PWM_PULSES_COUNT_PER_SYMBOL;
                    }

                    g_currentFrequencyIsF1200 = false;
                }
                // make sure new one bit frequency is 1200
                else if (isOne && !g_currentFrequencyIsF1200)
                {
                    const float trigArg = ANGULAR_FREQUENCY_F2200 * g_currentF2200Frame;
                    const float pulseWidth2200 = normalizePulseWidth(AMPLITUDE_SHIFT + AMPLITUDE_SCALER * SINE(trigArg));
                    const bool pulse2200Positive = COSINE_G_THAN_0(trigArg);
                    
                    if (pulse2200Positive)
                    {
                        g_currentF1200Frame = RECIPROCAL_ANGULAR_FREQUENCY_F1200 * INVERSE_SINE(RECIPROCAL_AMPLITUDE_SCALER * (pulseWidth2200 - AMPLITUDE_SHIFT));
                    }
                    else
                    {
                        g_currentF1200Frame = HALF_PERIOD_F1200 - RECIPROCAL_ANGULAR_FREQUENCY_F1200 * INVERSE_SINE(RECIPROCAL_AMPLITUDE_SCALER * (pulseWidth2200 - AMPLITUDE_SHIFT));
                    }

                    if (g_currentF1200Frame < 0)
                    {
                        g_currentF1200Frame += F1200_PWM_PULSES_COUNT_PER_SYMBOL;
                    }

                    g_currentFrequencyIsF1200 = true;
                }
                
                advanceBitstreamBit(&g_currentBitstreamPos);
            }
        }

        if (g_currentFrequencyIsF1200)
        {
            const uint32_t pulseWidth = (uint32_t) (AMPLITUDE_SHIFT + AMPLITUDE_SCALER * SINE(ANGULAR_FREQUENCY_F1200 * g_currentF1200Frame));
            setAprsPwmPulseWidth(pulseWidth);
            g_currentF1200Frame += PWM_STEP_SIZE;
            if (g_currentF1200Frame >= F1200_PWM_PULSES_COUNT_PER_SYMBOL)
            {
                g_currentF1200Frame -= F1200_PWM_PULSES_COUNT_PER_SYMBOL;
            }
        }
        else
        {
            const uint32_t pulseWidth = (uint32_t) (AMPLITUDE_SHIFT + AMPLITUDE_SCALER * SINE(ANGULAR_FREQUENCY_F2200 * g_currentF2200Frame));
            setAprsPwmPulseWidth(pulseWidth);
            g_currentF2200Frame += PWM_STEP_SIZE;
            if (g_currentF2200Frame >= F2200_PWM_PULSES_COUNT_PER_SYMBOL)
            {
                g_currentF2200Frame -= F2200_PWM_PULSES_COUNT_PER_SYMBOL;
            }
        }
        
        ++g_currentSymbolPulsesCount;
    }
}
