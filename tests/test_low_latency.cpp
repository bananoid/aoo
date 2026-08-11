#include "aoo_low_latency.h"

#include <array>
#include <cstring>

#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (false)

namespace {

AooLowLatencyStreamConfiguration make_configuration(AooByte profile) {
    AooLowLatencyStreamConfiguration configuration{};
    configuration.protocolVersion = AOO_LOW_LATENCY_PROTOCOL_VERSION;
    configuration.profile = profile;
    configuration.pcmFormat = profile == kAooLowLatencyProfileDeterministicWired
        ? kAooLowLatencyPcmFloat32
        : kAooLowLatencyPcmInt24;
    configuration.channelCount = 20;
    configuration.blockFrames = 64;
    configuration.datagramBytes = profile == kAooLowLatencyProfileDeterministicWired
        ? 1400
        : 1200;
    configuration.channelMapCount = configuration.channelCount;
    configuration.sampleRate = 48000;
    configuration.targetLatencyFrames = profile == kAooLowLatencyProfileDeterministicWired
        ? 192
        : 2400;
    configuration.capabilities =
        kAooLowLatencyCapabilityAbsoluteSamplePosition
        | kAooLowLatencyCapabilityChannelMap
        | kAooLowLatencyCapabilityDynamicResampling;
    if (profile == kAooLowLatencyProfileAdaptiveWireless) {
        configuration.capabilities |= kAooLowLatencyCapabilityDeadlineResend;
    }
    for (AooByte channel = 0; channel < configuration.channelCount; ++channel) {
        configuration.channelMap[channel] = channel;
    }
    return configuration;
}

} // namespace

int main() {
    AooLowLatencyPacketHeader expected{};
    expected.protocolVersion = AOO_LOW_LATENCY_PROTOCOL_VERSION;
    expected.headerSize = AOO_LOW_LATENCY_PACKET_HEADER_SIZE;
    expected.profile = kAooLowLatencyProfileDeterministicWired;
    expected.pcmFormat = kAooLowLatencyPcmFloat32;
    expected.streamId = 0x0102030405060708ULL;
    expected.sequence = 0xfffffff0U;
    expected.absoluteSamplePosition = 0x1112131415161718ULL;
    expected.sourceTimestamp = 0x2122232425262728ULL;
    expected.blockFrames = 64;
    expected.fragmentIndex = 2;
    expected.fragmentCount = 4;
    expected.channelCount = 20;
    expected.payloadBytes = 3;

    std::array<AooByte, AOO_LOW_LATENCY_PACKET_HEADER_SIZE + 3> packet{};
    CHECK(aoo_lowLatencyPacketHeaderEncode(
        &expected,
        packet.data(),
        packet.size()
    ) == kAooOk);
    packet[AOO_LOW_LATENCY_PACKET_HEADER_SIZE] = 1;
    packet[AOO_LOW_LATENCY_PACKET_HEADER_SIZE + 1] = 2;
    packet[AOO_LOW_LATENCY_PACKET_HEADER_SIZE + 2] = 3;

    AooLowLatencyPacketHeader decoded{};
    CHECK(aoo_lowLatencyPacketHeaderDecode(
        packet.data(),
        packet.size(),
        &decoded
    ) == kAooOk);
    CHECK(std::memcmp(&expected, &decoded, sizeof(expected)) == 0);

    auto malformed = packet;
    malformed[0] = 'X';
    CHECK(aoo_lowLatencyPacketHeaderDecode(
        malformed.data(),
        malformed.size(),
        &decoded
    ) == kAooErrorBadFormat);
    CHECK(aoo_lowLatencyPacketHeaderDecode(
        packet.data(),
        AOO_LOW_LATENCY_PACKET_HEADER_SIZE - 1,
        &decoded
    ) == kAooErrorBadFormat);

    for (auto profile : {
        kAooLowLatencyProfileDeterministicWired,
        kAooLowLatencyProfileAdaptiveWireless
    }) {
        auto configuration = make_configuration(profile);
        std::array<AooByte, AOO_LOW_LATENCY_STREAM_CONFIGURATION_SIZE> bytes{};
        CHECK(aoo_lowLatencyStreamConfigurationEncode(
            &configuration,
            bytes.data(),
            bytes.size()
        ) == kAooOk);
        AooLowLatencyStreamConfiguration configurationDecoded{};
        CHECK(aoo_lowLatencyStreamConfigurationDecode(
            bytes.data(),
            bytes.size(),
            &configurationDecoded
        ) == kAooOk);
        CHECK(std::memcmp(
            &configuration,
            &configurationDecoded,
            sizeof(configuration)
        ) == 0);
    }

    auto badMap = make_configuration(kAooLowLatencyProfileDeterministicWired);
    badMap.channelMap[1] = badMap.channelMap[0];
    CHECK(aoo_lowLatencyStreamConfigurationValidate(&badMap)
        == kAooErrorBadArgument);

    auto badFormat = make_configuration(kAooLowLatencyProfileAdaptiveWireless);
    badFormat.pcmFormat = kAooLowLatencyPcmFloat32;
    CHECK(aoo_lowLatencyStreamConfigurationValidate(&badFormat)
        == kAooErrorBadArgument);

    auto missingCapability = make_configuration(
        kAooLowLatencyProfileAdaptiveWireless
    );
    missingCapability.capabilities &=
        ~kAooLowLatencyCapabilityDeadlineResend;
    CHECK(aoo_lowLatencyStreamConfigurationValidate(&missingCapability)
        == kAooErrorBadArgument);

    expected.sequence = UINT32_MAX;
    CHECK(aoo_lowLatencyPacketHeaderEncode(
        &expected,
        packet.data(),
        packet.size()
    ) == kAooOk);
    CHECK(aoo_lowLatencyPacketHeaderDecode(
        packet.data(),
        packet.size(),
        &decoded
    ) == kAooOk);
    CHECK(decoded.sequence == UINT32_MAX);
}
