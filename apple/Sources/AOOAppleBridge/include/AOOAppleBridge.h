#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AOOAppleSender AOOAppleSender;
typedef struct AOOAppleReceiver AOOAppleReceiver;

typedef enum AOOAppleTransportProfile {
    AOOAppleTransportProfileAutomatic = 0,
    AOOAppleTransportProfileDeterministicWired = 1,
    AOOAppleTransportProfileAdaptiveWireless = 2
} AOOAppleTransportProfile;

typedef enum AOOApplePCMFormat {
    AOOApplePCMFormatFloat32 = 1,
    AOOApplePCMFormatInt24 = 2
} AOOApplePCMFormat;

typedef struct AOOAppleSenderStatus {
    int32_t isReady;
    int32_t isEnabled;
    int32_t hasDestination;
    int32_t peerResponsive;
    int32_t localPort;
    int32_t channelCount;
    int32_t sampleRate;
    int32_t streamBlockSize;
    int32_t packetSize;
    int32_t transportProfile;
    int32_t pcmFormat;
    int32_t lastError;
    uint64_t processCallCount;
    uint64_t processedFrameCount;
    uint64_t processErrorCount;
    uint64_t processIdleCount;
    uint64_t processWouldBlockCount;
    uint64_t handoffDropCount;
    uint64_t resentFrameCount;
    uint64_t pingEventCount;
    uint64_t inviteEventCount;
    uint64_t uninviteEventCount;
    uint64_t sinkRemoveEventCount;
    uint64_t datagramAttemptCount;
    uint64_t datagramSuccessCount;
    uint64_t datagramFailureCount;
    uint64_t attemptedDatagramByteCount;
    uint64_t sentDatagramByteCount;
    int32_t lastDatagramSendResult;
    int32_t lastDatagramSocketError;
    double handoffLatencyMilliseconds;
    double maximumHandoffLatencyMilliseconds;
    double maximumProcessIntervalMilliseconds;
    double processCadenceSampleRate;
    double sourcePresentationLeadMilliseconds;
    double roundTripMilliseconds;
    double packetLoss;
    double realSampleRate;
} AOOAppleSenderStatus;

typedef struct AOOAppleReceiverStatus {
    int32_t isReady;
    int32_t streamActive;
    int32_t streamState;
    int32_t localPort;
    int32_t channelCount;
    int32_t sampleRate;
    int32_t maximumBlockSize;
    int32_t fixedBlockSizeEnabled;
    int32_t dynamicResamplingEnabled;
    int32_t lastProcessFrameCount;
    int32_t minimumProcessFrameCount;
    int32_t maximumProcessFrameCount;
    int32_t sourceChannelCount;
    int32_t sourceSampleRate;
    int32_t sourceBlockSize;
    int32_t transportProfile;
    int32_t pcmFormat;
    int32_t lastError;
    uint64_t processCallCount;
    uint64_t processedFrameCount;
    uint64_t processErrorCount;
    uint64_t processBlockMismatchCount;
    uint64_t streamStartCount;
    uint64_t streamActiveCount;
    uint64_t streamBufferingCount;
    uint64_t streamInactiveCount;
    uint64_t bufferUnderrunCount;
    uint64_t bufferOverrunCount;
    uint64_t droppedBlockCount;
    uint64_t resentBlockCount;
    uint64_t sourceXRunCount;
    uint64_t concealmentCount;
    uint64_t reacquisitionCount;
    uint64_t incompatibleStreamCount;
    uint64_t adaptiveAdjustmentCount;
    uint64_t arrivalObservationCount;
    uint64_t completionObservationCount;
    uint64_t datagramObservationCount;
    uint64_t staleDatagramCount;
    uint64_t incompleteBlockCount;
    uint64_t trimmedBacklogBlockCount;
    uint64_t pingEventCount;
    uint64_t aooStreamTimeEventCount;
    uint64_t streamTimeEventCount;
    uint64_t presentationTimestampEventCount;
    uint64_t sourceSamplePosition;
    double sourceLatencyMilliseconds;
    double sinkLatencyMilliseconds;
    double jitterBufferLatencyMilliseconds;
    double rawRoundTripMilliseconds;
    double rawClockOffsetMilliseconds;
    double rawAOOStreamTimeDeltaMilliseconds;
    double rawStreamTimeDeltaMilliseconds;
    double rawPresentationToSinkCallbackDeltaMilliseconds;
    double outputPresentationLatencyMilliseconds;
    double latestArrivalResidualMilliseconds;
    double p99ArrivalResidualMilliseconds;
    double latestCompletionResidualMilliseconds;
    double p99CompletionResidualMilliseconds;
    double latestDatagramGapMilliseconds;
    double maximumDatagramGapMilliseconds;
    double targetLatencyMilliseconds;
    double bufferCapacityMilliseconds;
    double bufferFillRatio;
    double bufferedAudioMilliseconds;
    double sourceRealSampleRate;
    double realSampleRate;
} AOOAppleReceiverStatus;

const char *AOOAppleErrorString(int32_t errorCode);
int32_t AOOAppleLastSocketErrorCode(void);
uint64_t AOOAppleCurrentNTPTime(void);
uint64_t AOOAppleNTPTimeForMachHostTime(uint64_t hostTime);
uint64_t AOOAppleNTPTimeOffsetFrames(
    uint64_t timestamp,
    int64_t frameOffset,
    double sampleRate
);
uint64_t AOOAppleNTPTimeOffsetNanoseconds(
    uint64_t timestamp,
    int64_t nanosecondOffset
);

AOOAppleSender *AOOAppleSenderCreate(
    int32_t localPort,
    int32_t sourceID,
    int32_t channelCount,
    const int32_t *sourceChannelIndices,
    int32_t sourceChannelIndexCount,
    double sampleRate,
    int32_t maximumBlockSize,
    int32_t streamBlockSize,
    int32_t packetSize,
    double targetLatencyMilliseconds,
    int32_t transportProfile,
    int32_t pcmFormat,
    uint32_t capabilities,
    int32_t *errorCode
);

void AOOAppleSenderDestroy(AOOAppleSender *sender);

int32_t AOOAppleSenderSetDestination(
    AOOAppleSender *sender,
    const char *host,
    int32_t port,
    int32_t sinkID
);

void AOOAppleSenderClearDestination(AOOAppleSender *sender);
int32_t AOOAppleSenderSetEnabled(AOOAppleSender *sender, int32_t enabled);
int32_t AOOAppleSenderSetSimulatedPacketLoss(
    AOOAppleSender *sender,
    float fraction
);

int32_t AOOAppleSenderProcessPlanarAtTime(
    AOOAppleSender *sender,
    const float *planarBaseAddress,
    int32_t channelStride,
    int32_t frameOffset,
    int32_t channelCount,
    int32_t frameCount,
    uint64_t sourceSamplePosition,
    uint64_t sourceNtpTime,
    uint64_t sourcePresentationNtpTime
);

int32_t AOOAppleSenderProcessSilenceAtTime(
    AOOAppleSender *sender,
    int32_t frameCount,
    uint64_t sourceSamplePosition,
    uint64_t sourceNtpTime,
    uint64_t sourcePresentationNtpTime
);

void AOOAppleSenderGetStatus(
    const AOOAppleSender *sender,
    AOOAppleSenderStatus *status
);

AOOAppleReceiver *AOOAppleReceiverCreate(
    int32_t localPort,
    int32_t sinkID,
    int32_t outputChannelCount,
    int32_t sourceChannelCount,
    const int32_t *sourceChannelIndices,
    int32_t sourceChannelIndexCount,
    double sampleRate,
    int32_t maximumBlockSize,
    int32_t streamBlockSize,
    int32_t fixedBlockSizeEnabled,
    double latencyMilliseconds,
    int32_t packetSize,
    int32_t transportProfile,
    int32_t pcmFormat,
    uint32_t capabilities,
    int32_t *errorCode
);

void AOOAppleReceiverDestroy(AOOAppleReceiver *receiver);

int32_t AOOAppleReceiverSetLatency(
    AOOAppleReceiver *receiver,
    double latencyMilliseconds
);

void AOOAppleReceiverSetOutputPresentationLatency(
    AOOAppleReceiver *receiver,
    double latencyMilliseconds
);

void AOOAppleReceiverSetMonitorPair(
    AOOAppleReceiver *receiver,
    int32_t monitorPairStartChannel
);

int32_t AOOAppleReceiverProcessStereoAtTime(
    AOOAppleReceiver *receiver,
    float *outputLeft,
    float *outputRight,
    int32_t frameCount,
    uint64_t sinkNtpTime
);

void AOOAppleReceiverGetStatus(
    AOOAppleReceiver *receiver,
    AOOAppleReceiverStatus *status
);

int32_t AOOAppleReceiverCopyChannelPeaks(
    const AOOAppleReceiver *receiver,
    float *destination,
    int32_t capacity
);

#ifdef __cplusplus
}
#endif
