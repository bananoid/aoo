#pragma once

#include "aoo_config.h"
#include "aoo_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AOO_LOW_LATENCY_PROTOCOL_VERSION 1
#define AOO_LOW_LATENCY_PACKET_HEADER_SIZE 52
#define AOO_LOW_LATENCY_STREAM_CONFIGURATION_SIZE 92
#define AOO_LOW_LATENCY_MAX_CHANNELS 64

typedef struct AooSource AooSource;
typedef struct AooSink AooSink;
typedef struct AooClient AooClient;

typedef enum AooLowLatencyProfile {
    kAooLowLatencyProfileDeterministicWired = 1,
    kAooLowLatencyProfileAdaptiveWireless = 2
} AooLowLatencyProfile;

typedef enum AooLowLatencyPcmFormat {
    kAooLowLatencyPcmFloat32 = 1,
    kAooLowLatencyPcmInt24 = 2
} AooLowLatencyPcmFormat;

typedef enum AooLowLatencyCapability {
    kAooLowLatencyCapabilityAbsoluteSamplePosition = 1 << 0,
    kAooLowLatencyCapabilityChannelMap = 1 << 1,
    kAooLowLatencyCapabilityDynamicResampling = 1 << 2,
    kAooLowLatencyCapabilityDeadlineResend = 1 << 3
} AooLowLatencyCapability;

typedef struct AooLowLatencyPacketHeader {
    AooUInt16 protocolVersion;
    AooUInt16 headerSize;
    AooByte profile;
    AooByte pcmFormat;
    AooUInt16 flags;
    AooUInt64 streamId;
    AooUInt32 sequence;
    AooUInt64 absoluteSamplePosition;
    AooUInt64 sourceTimestamp;
    AooUInt16 blockFrames;
    AooUInt16 fragmentIndex;
    AooUInt16 fragmentCount;
    AooUInt16 channelCount;
    AooUInt32 payloadBytes;
} AooLowLatencyPacketHeader;

typedef struct AooLowLatencyStreamConfiguration {
    AooUInt16 protocolVersion;
    AooByte profile;
    AooByte pcmFormat;
    AooUInt16 channelCount;
    AooUInt16 blockFrames;
    AooUInt16 datagramBytes;
    AooUInt16 channelMapCount;
    AooUInt32 sampleRate;
    AooUInt32 targetLatencyFrames;
    AooUInt32 capabilities;
    AooByte channelMap[AOO_LOW_LATENCY_MAX_CHANNELS];
} AooLowLatencyStreamConfiguration;

typedef struct AooLowLatencySinkStatistics {
    AooUInt64 arrivalObservationCount;
    AooSeconds latestArrivalResidual;
    AooSeconds p99ArrivalResidual;
    AooUInt64 completionObservationCount;
    AooSeconds latestCompletionResidual;
    AooSeconds p99CompletionResidual;
    AooUInt64 datagramObservationCount;
    AooUInt64 staleDatagramCount;
    AooUInt64 incompleteBlockCount;
    AooUInt64 emptyBlockCount;
    AooUInt64 reacquisitionCount;
    AooUInt64 trimmedBacklogBlockCount;
    AooUInt32 currentBufferedBlockCount;
    AooUInt32 currentContiguousCompleteBlockCount;
    AooUInt32 currentMissingBlockStreak;
    AooUInt32 maximumMissingBlockStreak;
    AooUInt32 currentResamplerBufferedFrameCount;
    AooUInt32 currentPlayableFrameCount;
    AooSeconds latestDatagramGap;
    AooSeconds maximumDatagramGap;
    AooSampleRate latestSourceSampleRate;
} AooLowLatencySinkStatistics;

typedef struct AooLowLatencyClientSendStatistics {
    AooUInt64 datagramAttemptCount;
    AooUInt64 datagramSuccessCount;
    AooUInt64 datagramFailureCount;
    AooUInt64 attemptedByteCount;
    AooUInt64 sentByteCount;
    AooInt32 lastSendResult;
    AooInt32 lastSocketError;
} AooLowLatencyClientSendStatistics;

typedef struct AooLowLatencyClientReceiveStatistics {
    AooUInt64 datagramCount;
    AooUInt64 kernelTimestampCount;
    AooSeconds latestKernelDatagramGap;
    AooSeconds maximumKernelDatagramGap;
    AooSeconds latestKernelToReceiveDelay;
    AooSeconds maximumKernelToReceiveDelay;
} AooLowLatencyClientReceiveStatistics;

/** Encode the fixed, network-byte-order low-latency packet header. */
AOO_API AooError AOO_CALL aoo_lowLatencyPacketHeaderEncode(
    const AooLowLatencyPacketHeader *header,
    AooByte *destination,
    AooSize destinationSize
);

/** Decode and validate a fixed low-latency packet header. */
AOO_API AooError AOO_CALL aoo_lowLatencyPacketHeaderDecode(
    const AooByte *source,
    AooSize sourceSize,
    AooLowLatencyPacketHeader *header
);

/** Validate a negotiated immutable stream configuration. */
AOO_API AooError AOO_CALL aoo_lowLatencyStreamConfigurationValidate(
    const AooLowLatencyStreamConfiguration *configuration
);

AOO_API AooError AOO_CALL aoo_lowLatencyStreamConfigurationEncode(
    const AooLowLatencyStreamConfiguration *configuration,
    AooByte *destination,
    AooSize destinationSize
);

AOO_API AooError AOO_CALL aoo_lowLatencyStreamConfigurationDecode(
    const AooByte *source,
    AooSize sourceSize,
    AooLowLatencyStreamConfiguration *configuration
);

/**
 * Process one source block with its absolute source timeline coordinates.
 *
 * This is the low-latency counterpart of AooSource_process(). The source
 * sample position and timestamp are copied into every packet fragment and
 * retained by resend history.
 */
AOO_API AooError AOO_CALL AooSource_processLowLatency(
    AooSource *source,
    AooSample **data,
    AooInt32 numSamples,
    AooNtpTime sourceTimestamp,
    AooUInt64 absoluteSamplePosition
);

/** Copy bounded packet-arrival statistics without resetting them. */
AOO_API AooError AOO_CALL AooSink_getLowLatencyStatistics(
    AooSink *sink,
    AooLowLatencySinkStatistics *statistics
);

/** Publish a new receiver target without resetting decoder or jitter state. */
AOO_API AooError AOO_CALL AooSink_setLowLatencyTarget(
    AooSink *sink,
    AooUInt32 targetLatencyFrames
);

/** Copy monotonic UDP send statistics without resetting them. */
AOO_API AooError AOO_CALL AooClient_getLowLatencySendStatistics(
    AooClient *client,
    AooLowLatencyClientSendStatistics *statistics
);

/** Copy monotonic UDP receive timing statistics without resetting them. */
AOO_API AooError AOO_CALL AooClient_getLowLatencyReceiveStatistics(
    AooClient *client,
    AooLowLatencyClientReceiveStatistics *statistics
);

#ifdef __cplusplus
}
#endif
