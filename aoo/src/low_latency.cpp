#include "aoo_low_latency.h"

#include "common/utils.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace {

constexpr std::array<AooByte, 4> kMagic{'A', 'O', 'L', 'L'};
constexpr std::array<AooByte, 4> kConfigurationMagic{'A', 'O', 'L', 'S'};
constexpr AooUInt32 kRequiredCapabilities =
    kAooLowLatencyCapabilityAbsoluteSamplePosition
    | kAooLowLatencyCapabilityChannelMap
    | kAooLowLatencyCapabilityDynamicResampling;

bool validProfile(AooByte profile) {
    return profile == kAooLowLatencyProfileDeterministicWired
        || profile == kAooLowLatencyProfileAdaptiveWireless;
}

bool validPcmFormat(AooByte format) {
    return format == kAooLowLatencyPcmFloat32
        || format == kAooLowLatencyPcmInt24;
}

bool validProfileFormat(AooByte profile, AooByte format) {
    return (profile == kAooLowLatencyProfileDeterministicWired
            && format == kAooLowLatencyPcmFloat32)
        || (profile == kAooLowLatencyProfileAdaptiveWireless
            && format == kAooLowLatencyPcmInt24);
}

template <typename T>
void write(T value, AooByte *&iterator) {
    aoo::write_bytes<T>(value, iterator);
}

template <typename T>
T read(const AooByte *&iterator) {
    return aoo::read_bytes<T>(iterator);
}

} // namespace

AooError AOO_CALL aoo_lowLatencyPacketHeaderEncode(
    const AooLowLatencyPacketHeader *header,
    AooByte *destination,
    AooSize destinationSize
) {
    if (!header || !destination
        || destinationSize < AOO_LOW_LATENCY_PACKET_HEADER_SIZE
        || header->protocolVersion != AOO_LOW_LATENCY_PROTOCOL_VERSION
        || header->headerSize != AOO_LOW_LATENCY_PACKET_HEADER_SIZE
        || !validProfile(header->profile)
        || !validPcmFormat(header->pcmFormat)
        || !validProfileFormat(header->profile, header->pcmFormat)
        || header->blockFrames == 0
        || (header->fragmentCount == 0
            ? header->fragmentIndex != 0 || header->payloadBytes != 0
            : header->fragmentIndex >= header->fragmentCount)
        || header->channelCount == 0
        || header->channelCount > AOO_LOW_LATENCY_MAX_CHANNELS) {
        return kAooErrorBadArgument;
    }

    auto iterator = destination;
    std::copy(kMagic.begin(), kMagic.end(), iterator);
    iterator += kMagic.size();
    write<AooUInt16>(header->protocolVersion, iterator);
    write<AooUInt16>(header->headerSize, iterator);
    write<AooByte>(header->profile, iterator);
    write<AooByte>(header->pcmFormat, iterator);
    write<AooUInt16>(header->flags, iterator);
    write<AooUInt64>(header->streamId, iterator);
    write<AooUInt32>(header->sequence, iterator);
    write<AooUInt64>(header->absoluteSamplePosition, iterator);
    write<AooUInt64>(header->sourceTimestamp, iterator);
    write<AooUInt16>(header->blockFrames, iterator);
    write<AooUInt16>(header->fragmentIndex, iterator);
    write<AooUInt16>(header->fragmentCount, iterator);
    write<AooUInt16>(header->channelCount, iterator);
    write<AooUInt32>(header->payloadBytes, iterator);
    return kAooOk;
}

AooError AOO_CALL aoo_lowLatencyPacketHeaderDecode(
    const AooByte *source,
    AooSize sourceSize,
    AooLowLatencyPacketHeader *header
) {
    if (!source || !header
        || sourceSize < AOO_LOW_LATENCY_PACKET_HEADER_SIZE
        || !std::equal(kMagic.begin(), kMagic.end(), source)) {
        return kAooErrorBadFormat;
    }

    auto iterator = source + kMagic.size();
    AooLowLatencyPacketHeader decoded{};
    decoded.protocolVersion = read<AooUInt16>(iterator);
    decoded.headerSize = read<AooUInt16>(iterator);
    decoded.profile = read<AooByte>(iterator);
    decoded.pcmFormat = read<AooByte>(iterator);
    decoded.flags = read<AooUInt16>(iterator);
    decoded.streamId = read<AooUInt64>(iterator);
    decoded.sequence = read<AooUInt32>(iterator);
    decoded.absoluteSamplePosition = read<AooUInt64>(iterator);
    decoded.sourceTimestamp = read<AooUInt64>(iterator);
    decoded.blockFrames = read<AooUInt16>(iterator);
    decoded.fragmentIndex = read<AooUInt16>(iterator);
    decoded.fragmentCount = read<AooUInt16>(iterator);
    decoded.channelCount = read<AooUInt16>(iterator);
    decoded.payloadBytes = read<AooUInt32>(iterator);

    if (decoded.protocolVersion != AOO_LOW_LATENCY_PROTOCOL_VERSION
        || decoded.headerSize != AOO_LOW_LATENCY_PACKET_HEADER_SIZE
        || decoded.headerSize > sourceSize
        || !validProfile(decoded.profile)
        || !validPcmFormat(decoded.pcmFormat)
        || !validProfileFormat(decoded.profile, decoded.pcmFormat)
        || decoded.blockFrames == 0
        || (decoded.fragmentCount == 0
            ? decoded.fragmentIndex != 0 || decoded.payloadBytes != 0
            : decoded.fragmentIndex >= decoded.fragmentCount)
        || decoded.channelCount == 0
        || decoded.channelCount > AOO_LOW_LATENCY_MAX_CHANNELS
        || decoded.payloadBytes > sourceSize - decoded.headerSize) {
        return kAooErrorBadFormat;
    }
    *header = decoded;
    return kAooOk;
}

AooError AOO_CALL aoo_lowLatencyStreamConfigurationValidate(
    const AooLowLatencyStreamConfiguration *configuration
) {
    if (!configuration
        || configuration->protocolVersion != AOO_LOW_LATENCY_PROTOCOL_VERSION
        || !validProfile(configuration->profile)
        || !validPcmFormat(configuration->pcmFormat)
        || configuration->channelCount == 0
        || configuration->channelCount > AOO_LOW_LATENCY_MAX_CHANNELS
        || configuration->channelMapCount != configuration->channelCount
        || configuration->blockFrames < 16
        || configuration->sampleRate < 8'000
        || configuration->sampleRate > 384'000
        || configuration->datagramBytes < 256
        || configuration->datagramBytes > AOO_MAX_PACKET_SIZE
        || configuration->targetLatencyFrames < configuration->blockFrames
        || (configuration->capabilities & kRequiredCapabilities)
            != kRequiredCapabilities) {
        return kAooErrorBadArgument;
    }
    if (configuration->profile == kAooLowLatencyProfileDeterministicWired
        && configuration->pcmFormat != kAooLowLatencyPcmFloat32) {
        return kAooErrorBadArgument;
    }
    if (configuration->profile == kAooLowLatencyProfileAdaptiveWireless
        && (configuration->pcmFormat != kAooLowLatencyPcmInt24
            || !(configuration->capabilities
                & kAooLowLatencyCapabilityDeadlineResend))) {
        return kAooErrorBadArgument;
    }
    std::array<bool, AOO_LOW_LATENCY_MAX_CHANNELS> used{};
    for (AooUInt16 index = 0; index < configuration->channelMapCount; ++index) {
        auto channel = configuration->channelMap[index];
        if (channel >= AOO_LOW_LATENCY_MAX_CHANNELS || used[channel]) {
            return kAooErrorBadArgument;
        }
        used[channel] = true;
    }
    return kAooOk;
}

AooError AOO_CALL aoo_lowLatencyStreamConfigurationEncode(
    const AooLowLatencyStreamConfiguration *configuration,
    AooByte *destination,
    AooSize destinationSize
) {
    if (!destination
        || destinationSize < AOO_LOW_LATENCY_STREAM_CONFIGURATION_SIZE
        || aoo_lowLatencyStreamConfigurationValidate(configuration) != kAooOk) {
        return kAooErrorBadArgument;
    }
    auto iterator = destination;
    std::copy(kConfigurationMagic.begin(), kConfigurationMagic.end(), iterator);
    iterator += kConfigurationMagic.size();
    write<AooUInt16>(configuration->protocolVersion, iterator);
    write<AooByte>(configuration->profile, iterator);
    write<AooByte>(configuration->pcmFormat, iterator);
    write<AooUInt16>(configuration->channelCount, iterator);
    write<AooUInt16>(configuration->blockFrames, iterator);
    write<AooUInt16>(configuration->datagramBytes, iterator);
    write<AooUInt16>(configuration->channelMapCount, iterator);
    write<AooUInt32>(configuration->sampleRate, iterator);
    write<AooUInt32>(configuration->targetLatencyFrames, iterator);
    write<AooUInt32>(configuration->capabilities, iterator);
    std::copy(
        configuration->channelMap,
        configuration->channelMap + AOO_LOW_LATENCY_MAX_CHANNELS,
        iterator
    );
    return kAooOk;
}

AooError AOO_CALL aoo_lowLatencyStreamConfigurationDecode(
    const AooByte *source,
    AooSize sourceSize,
    AooLowLatencyStreamConfiguration *configuration
) {
    if (!source || !configuration
        || sourceSize != AOO_LOW_LATENCY_STREAM_CONFIGURATION_SIZE
        || !std::equal(
            kConfigurationMagic.begin(),
            kConfigurationMagic.end(),
            source
        )) {
        return kAooErrorBadFormat;
    }
    auto iterator = source + kConfigurationMagic.size();
    AooLowLatencyStreamConfiguration decoded{};
    decoded.protocolVersion = read<AooUInt16>(iterator);
    decoded.profile = read<AooByte>(iterator);
    decoded.pcmFormat = read<AooByte>(iterator);
    decoded.channelCount = read<AooUInt16>(iterator);
    decoded.blockFrames = read<AooUInt16>(iterator);
    decoded.datagramBytes = read<AooUInt16>(iterator);
    decoded.channelMapCount = read<AooUInt16>(iterator);
    decoded.sampleRate = read<AooUInt32>(iterator);
    decoded.targetLatencyFrames = read<AooUInt32>(iterator);
    decoded.capabilities = read<AooUInt32>(iterator);
    std::copy(
        iterator,
        iterator + AOO_LOW_LATENCY_MAX_CHANNELS,
        decoded.channelMap
    );
    if (aoo_lowLatencyStreamConfigurationValidate(&decoded) != kAooOk) {
        return kAooErrorBadFormat;
    }
    *configuration = decoded;
    return kAooOk;
}
