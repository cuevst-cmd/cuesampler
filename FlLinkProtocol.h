#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace cuesampler
{
// The SysEx body excludes MIDI's F0/F7 framing. All body bytes are seven-bit
// clean; replies use a separate fixed-size datagram with ordinary LE integers.
inline constexpr std::size_t flLinkPacketSize = 19;
inline constexpr std::size_t flLinkReplySize = 14;
inline constexpr std::uint32_t flLinkMaxToken = 0x1fffff;
inline constexpr std::uint32_t flLinkMaxSequence = 0x0fffffff;
inline constexpr std::uint8_t flLinkAcceptedStatus = 0;
inline constexpr std::uint64_t flLinkConnectionTimeoutMs = 1500;

enum class FlLinkCommand : std::uint8_t { hello = 1, note = 2, allOff = 3 };

struct FlLinkPacket
{
    FlLinkCommand command = FlLinkCommand::hello;
    std::uint32_t token = 0;
    std::uint32_t port = 0;
    std::uint32_t sequence = 0;
    std::uint8_t channelZero = 0;
    std::uint8_t note = 0;
    std::uint8_t velocity = 0;
};

struct FlLinkReply
{
    std::uint32_t token = 0;
    std::uint32_t sequence = 0;
    FlLinkCommand command = FlLinkCommand::hello;
    // Zero means accepted. Any nonzero status is an explicit host rejection.
    std::uint8_t status = flLinkAcceptedStatus;
};

constexpr bool isFlLinkCommand (FlLinkCommand command) noexcept
{
    return command == FlLinkCommand::hello || command == FlLinkCommand::note
           || command == FlLinkCommand::allOff;
}

inline std::optional<std::array<std::uint8_t, flLinkPacketSize>>
encodeFlLinkPacket (const FlLinkPacket& packet) noexcept
{
    if (! isFlLinkCommand (packet.command) || packet.token == 0 || packet.token > flLinkMaxToken
        || packet.port == 0 || packet.port > 65535 || packet.sequence > flLinkMaxSequence
        || packet.channelZero > 15 || packet.note > 127 || packet.velocity > 127)
        return std::nullopt;

    std::array<std::uint8_t, flLinkPacketSize> data {
        0x7d, 'C', 'U', 'E', 1, static_cast<std::uint8_t> (packet.command)
    };
    for (std::size_t i = 0; i < 3; ++i)
    {
        data[6 + i] = static_cast<std::uint8_t> ((packet.token >> (7 * i)) & 0x7f);
        data[9 + i] = static_cast<std::uint8_t> ((packet.port >> (7 * i)) & 0x7f);
    }
    for (std::size_t i = 0; i < 4; ++i)
        data[12 + i] = static_cast<std::uint8_t> ((packet.sequence >> (7 * i)) & 0x7f);
    data[16] = packet.channelZero;
    data[17] = packet.note;
    data[18] = packet.velocity;
    return data;
}

inline std::optional<FlLinkPacket> decodeFlLinkPacket (const std::uint8_t* data,
                                                       std::size_t size) noexcept
{
    if (data == nullptr || size != flLinkPacketSize || data[0] != 0x7d || data[1] != 'C'
        || data[2] != 'U' || data[3] != 'E' || data[4] != 1)
        return std::nullopt;
    for (std::size_t i = 0; i < size; ++i)
        if (data[i] > 127) return std::nullopt;

    FlLinkPacket packet;
    packet.command = static_cast<FlLinkCommand> (data[5]);
    for (std::size_t i = 0; i < 3; ++i)
    {
        packet.token |= static_cast<std::uint32_t> (data[6 + i]) << (7 * i);
        packet.port |= static_cast<std::uint32_t> (data[9 + i]) << (7 * i);
    }
    for (std::size_t i = 0; i < 4; ++i)
        packet.sequence |= static_cast<std::uint32_t> (data[12 + i]) << (7 * i);
    packet.channelZero = data[16];
    packet.note = data[17];
    packet.velocity = data[18];
    if (! encodeFlLinkPacket (packet)) return std::nullopt;
    return packet;
}

inline std::optional<std::array<std::uint8_t, flLinkReplySize>>
encodeFlLinkReply (const FlLinkReply& reply) noexcept
{
    if (! isFlLinkCommand (reply.command) || reply.token == 0 || reply.token > flLinkMaxToken
        || reply.sequence > flLinkMaxSequence)
        return std::nullopt;
    std::array<std::uint8_t, flLinkReplySize> data { 'C', 'U', 'E', '1' };
    for (std::size_t i = 0; i < 4; ++i)
    {
        data[4 + i] = static_cast<std::uint8_t> (reply.token >> (8 * i));
        data[8 + i] = static_cast<std::uint8_t> (reply.sequence >> (8 * i));
    }
    data[12] = static_cast<std::uint8_t> (reply.command);
    data[13] = reply.status;
    return data;
}

inline std::optional<FlLinkReply> decodeFlLinkReply (const std::uint8_t* data,
                                                   std::size_t size) noexcept
{
    if (data == nullptr || size != flLinkReplySize || data[0] != 'C' || data[1] != 'U'
        || data[2] != 'E' || data[3] != '1')
        return std::nullopt;
    FlLinkReply reply;
    for (std::size_t i = 0; i < 4; ++i)
    {
        reply.token |= static_cast<std::uint32_t> (data[4 + i]) << (8 * i);
        reply.sequence |= static_cast<std::uint32_t> (data[8 + i]) << (8 * i);
    }
    reply.command = static_cast<FlLinkCommand> (data[12]);
    reply.status = data[13];
    if (! encodeFlLinkReply (reply)) return std::nullopt;
    return reply;
}

// Message-thread state only. A MIDI port's existence does not establish a link;
// only the current instance's pending hello can do that. The caller supplies a
// monotonic clock and calls tick even while no replies arrive.
class FlLinkConnection
{
public:
    bool begin (std::uint32_t instanceToken) noexcept
    {
        stop();
        if (instanceToken == 0 || instanceToken > flLinkMaxToken) return false;
        token = instanceToken;
        return true;
    }

    bool noteHelloSent (std::uint32_t sequence) noexcept
    {
        if (token == 0 || sequence > flLinkMaxSequence) return false;
        pendingSequence = sequence;
        awaitingHello = true;
        return true;
    }

    bool acceptReply (const FlLinkReply& reply, std::uint64_t nowMs) noexcept
    {
        if (token == 0 || ! awaitingHello || reply.token != token
            || reply.command != FlLinkCommand::hello || reply.sequence != pendingSequence)
            return false;
        awaitingHello = false;
        if (reply.status != flLinkAcceptedStatus)
        {
            connected = false;
            return false;
        }
        connected = true;
        lastAcceptedMs = nowMs;
        return true;
    }

    void tick (std::uint64_t nowMs) noexcept
    {
        if (connected && (nowMs < lastAcceptedMs
                          || nowMs - lastAcceptedMs >= flLinkConnectionTimeoutMs))
            connected = false;
    }

    void stop() noexcept
    {
        token = pendingSequence = 0;
        lastAcceptedMs = 0;
        awaitingHello = connected = false;
    }

    bool isConnected() const noexcept { return connected; }

private:
    std::uint32_t token = 0, pendingSequence = 0;
    std::uint64_t lastAcceptedMs = 0;
    bool awaitingHello = false, connected = false;
};
}
