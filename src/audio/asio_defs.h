#pragma once

#include <windows.h>
#include <unknwn.h>
#include <cstdint>

namespace praccy::audio {

using ASIOBool = int32_t;
using ASIOError = int32_t;

enum ASIOErrorCodes : ASIOError {
    ASE_OK = 0,
    ASE_SUCCESS = 0x3f4847a0,
    ASE_NotPresent = -1000,
    ASE_HWMalfunction,
    ASE_InvalidParameter,
    ASE_InvalidMode,
    ASE_SPNotAdvancing,
    ASE_NoClock,
    ASE_NoMemory
};

enum ASIOSampleType : int32_t {
    ASIOSTInt16MSB = 0,
    ASIOSTInt24MSB = 1,
    ASIOSTInt32MSB = 2,
    ASIOSTFloat32MSB = 3,
    ASIOSTFloat64MSB = 4,
    ASIOSTInt32MSB16 = 8,
    ASIOSTInt32MSB18 = 9,
    ASIOSTInt32MSB20 = 10,
    ASIOSTInt32MSB24 = 11,
    ASIOSTInt16LSB = 16,
    ASIOSTInt24LSB = 17,
    ASIOSTInt32LSB = 18,
    ASIOSTFloat32LSB = 19,
    ASIOSTFloat64LSB = 20,
    ASIOSTInt32LSB16 = 24,
    ASIOSTInt32LSB18 = 25,
    ASIOSTInt32LSB20 = 26,
    ASIOSTInt32LSB24 = 27
};

#pragma pack(push, 4)

struct ASIOSamples {
    uint32_t lo;
    uint32_t hi;
};

struct ASIOTimeStamp {
    uint32_t lo;
    uint32_t hi;
};

struct ASIOTimeCode {
    double speed;
    ASIOSamples timeCodeSamples;
    uint32_t flags;
    char future[64];
};

struct ASIOTimeInfo {
    double speed;
    ASIOTimeStamp systemTime;
    ASIOSamples samplePosition;
    double sampleRate;
    uint32_t flags;
    char reserved[12];
};

struct ASIOTime {
    int32_t reserved[4];
    ASIOTimeInfo timeInfo;
    ASIOTimeCode timeCode;
};

struct ASIOCallbacks {
    void (*bufferSwitch)(int32_t doubleBufferIndex, ASIOBool directProcess);
    void (*sampleRateDidChange)(double sRate);
    int32_t (*asioMessage)(int32_t selector, int32_t value, void* message, double* opt);
    ASIOTime* (*bufferSwitchTimeInfo)(ASIOTime* params, int32_t doubleBufferIndex, ASIOBool directProcess);
};

struct ASIOBufferInfo {
    ASIOBool isInput;
    int32_t channelNum;
    void* buffers[2];
};

struct ASIOChannelInfo {
    int32_t channel;
    ASIOBool isInput;
    ASIOBool isActive;
    int32_t channelGroup;
    ASIOSampleType type;
    char name[32];
};

#pragma pack(pop)

/**
 * @brief IASIO COM Interface definition.
 */
class IASIO : public IUnknown {
public:
    virtual ASIOBool init(void* sysHandle) = 0;
    virtual void getDriverName(char* name) = 0;
    virtual int32_t getDriverVersion() = 0;
    virtual void getErrorMessage(char* string) = 0;
    virtual ASIOError start() = 0;
    virtual ASIOError stop() = 0;
    virtual ASIOError getChannels(int32_t* numInputChannels, int32_t* numOutputChannels) = 0;
    virtual ASIOError getLatencies(int32_t* inputLatency, int32_t* outputLatency) = 0;
    virtual ASIOError getBufferSize(int32_t* minSize, int32_t* maxSize, int32_t* preferredSize, int32_t* granularity) = 0;
    virtual ASIOError canSampleRate(double sampleRate) = 0;
    virtual ASIOError getSampleRate(double* sampleRate) = 0;
    virtual ASIOError setSampleRate(double sampleRate) = 0;
    virtual ASIOError getClockSources(void* clocks, int32_t* numSources) = 0;
    virtual ASIOError setClockSource(int32_t reference) = 0;
    virtual ASIOError getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) = 0;
    virtual ASIOError getChannelInfo(ASIOChannelInfo* info) = 0;
    virtual ASIOError createBuffers(ASIOBufferInfo* bufferInfos, int32_t numChannels, int32_t bufferSize, ASIOCallbacks* callbacks) = 0;
    virtual ASIOError disposeBuffers() = 0;
    virtual ASIOError controlPanel() = 0;
    virtual ASIOError future(int32_t selector, void* opt) = 0;
    virtual ASIOError outputReady() = 0;
};

} // namespace praccy::audio
