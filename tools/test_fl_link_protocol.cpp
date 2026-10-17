#include "FlLinkProtocol.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <type_traits>

namespace
{
int failures = 0;
void check (bool condition, const char* message)
{
    if (! condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}

bool same (const cuesampler::FlLinkPacket& a, const cuesampler::FlLinkPacket& b)
{
    return a.command == b.command && a.token == b.token && a.port == b.port
           && a.sequence == b.sequence && a.channelZero == b.channelZero
           && a.note == b.note && a.velocity == b.velocity;
}
}

int main()
{
    using namespace cuesampler;
    static_assert (std::is_trivially_copyable_v<FlLinkPacket>);
    static_assert (std::is_trivially_copyable_v<FlLinkReply>);
    static_assert (std::is_trivially_copyable_v<FlLinkConnection>);
    static_assert (noexcept (encodeFlLinkPacket (FlLinkPacket {})));
    static_assert (noexcept (decodeFlLinkPacket (nullptr, 0)));
    static_assert (noexcept (encodeFlLinkReply (FlLinkReply {})));
    static_assert (noexcept (decodeFlLinkReply (nullptr, 0)));

    const FlLinkPacket base { FlLinkCommand::note, 0x123456, 54321, 0x0abcdef0, 0, 0, 100 };
    bool complete = true;
    for (int channel = 0; channel < 16; ++channel)
        for (int note = 0; note < 128; ++note)
            for (const int velocity : { 0, 1, 127 })
            {
                auto packet = base;
                packet.channelZero = static_cast<std::uint8_t> (channel);
                packet.note = static_cast<std::uint8_t> (note);
                packet.velocity = static_cast<std::uint8_t> (velocity);
                const auto encoded = encodeFlLinkPacket (packet);
                if (! encoded) { complete = false; continue; }
                const auto decoded = decodeFlLinkPacket (encoded->data(), encoded->size());
                complete &= decoded && same (*decoded, packet)
                            && std::all_of (encoded->begin(), encoded->end(), [] (auto byte) { return byte < 128; });
            }
    check (complete, "all 128 pitches, 16 channels and velocity edges round-trip as seven-bit SysEx");

    for (const auto command : { FlLinkCommand::hello, FlLinkCommand::note, FlLinkCommand::allOff })
        for (const auto token : { std::uint32_t (1), flLinkMaxToken })
            for (const auto port : { std::uint32_t (1), std::uint32_t (65535) })
                for (const auto sequence : { std::uint32_t (0), flLinkMaxSequence })
                {
                    auto packet = base;
                    packet.command = command; packet.token = token; packet.port = port; packet.sequence = sequence;
                    const auto data = encodeFlLinkPacket (packet);
                    const auto decoded = data ? decodeFlLinkPacket (data->data(), data->size()) : std::nullopt;
                    check (decoded && same (*decoded, packet), "command/token/port/sequence boundaries round-trip");
                }

    const auto valid = *encodeFlLinkPacket (base);
    check (valid[6] == (base.token & 0x7f) && valid[7] == ((base.token >> 7) & 0x7f)
           && valid[9] == (base.port & 0x7f) && valid[12] == (base.sequence & 0x7f),
           "SysEx uses the specified seven-bit little-endian layout");
    check (! decodeFlLinkPacket (nullptr, flLinkPacketSize), "null packet pointer rejected");
    check (! decodeFlLinkPacket (valid.data(), 0)
           && ! decodeFlLinkPacket (valid.data(), flLinkPacketSize - 1)
           && ! decodeFlLinkPacket (valid.data(), flLinkPacketSize + 1), "nonexact packet sizes rejected");
    for (std::size_t i = 0; i < valid.size(); ++i)
    {
        auto corrupted = valid; corrupted[i] = 0x80;
        check (! decodeFlLinkPacket (corrupted.data(), corrupted.size()), "high-bit corruption rejected in every SysEx byte");
    }
    for (std::size_t i = 0; i < 5; ++i)
    {
        auto corrupted = valid; corrupted[i] ^= 1;
        check (! decodeFlLinkPacket (corrupted.data(), corrupted.size()), "wrong manufacturer/magic/version rejected");
    }
    for (const auto field : { 0, 1, 2, 3, 4, 5, 6, 7, 8 })
    {
        auto invalid = base;
        switch (field)
        {
            case 0: invalid.command = static_cast<FlLinkCommand> (0); break;
            case 1: invalid.token = 0; break;
            case 2: invalid.token = flLinkMaxToken + 1; break;
            case 3: invalid.port = 0; break;
            case 4: invalid.port = 65536; break;
            case 5: invalid.sequence = flLinkMaxSequence + 1; break;
            case 6: invalid.channelZero = 16; break;
            case 7: invalid.note = 128; break;
            case 8: invalid.velocity = 128; break;
        }
        check (! encodeFlLinkPacket (invalid), "out-of-range packet field rejected by encoder");
    }
    auto corruptPort = valid; corruptPort[9] = corruptPort[10] = corruptPort[11] = 127;
    check (! decodeFlLinkPacket (corruptPort.data(), corruptPort.size()), "21-bit wire port exceeding UDP bounds rejected");
    auto corruptToken = valid; corruptToken[6] = corruptToken[7] = corruptToken[8] = 0;
    check (! decodeFlLinkPacket (corruptToken.data(), corruptToken.size()), "zero wire token rejected");
    auto corruptCommand = valid; corruptCommand[5] = 4;
    check (! decodeFlLinkPacket (corruptCommand.data(), corruptCommand.size()), "unknown wire command rejected");
    auto corruptChannel = valid; corruptChannel[16] = 16;
    check (! decodeFlLinkPacket (corruptChannel.data(), corruptChannel.size()), "wire channel 17 rejected");

    for (const auto status : { std::uint8_t (0), std::uint8_t (1), std::uint8_t (255) })
    {
        const FlLinkReply reply { flLinkMaxToken, flLinkMaxSequence, FlLinkCommand::hello, status };
        const auto data = encodeFlLinkReply (reply);
        const auto decoded = data ? decodeFlLinkReply (data->data(), data->size()) : std::nullopt;
        check (decoded && decoded->token == reply.token && decoded->sequence == reply.sequence
               && decoded->command == reply.command && decoded->status == status,
               "raw little-endian replies retain accepted and rejected status bytes");
        check (data && (*data)[4] == 255 && (*data)[5] == 255 && (*data)[6] == 31 && (*data)[7] == 0
               && (*data)[8] == 255 && (*data)[9] == 255 && (*data)[10] == 255 && (*data)[11] == 15,
               "reply integers use ordinary eight-bit little-endian, not MIDI seven-bit encoding");
    }
    const auto replyData = *encodeFlLinkReply ({ 123, 42, FlLinkCommand::hello, 0 });
    check (! decodeFlLinkReply (nullptr, flLinkReplySize)
           && ! decodeFlLinkReply (replyData.data(), flLinkReplySize - 1)
           && ! decodeFlLinkReply (replyData.data(), flLinkReplySize + 1), "reply requires exact fixed size and nonnull data");
    for (std::size_t i = 0; i < 4; ++i)
    {
        auto data = replyData; data[i] ^= 1;
        check (! decodeFlLinkReply (data.data(), data.size()), "wrong reply magic/version rejected");
    }
    auto badReply = replyData; badReply[7] = 1;
    check (! decodeFlLinkReply (badReply.data(), badReply.size()), "oversized reply token rejected");
    badReply = replyData; badReply[11] = 16;
    check (! decodeFlLinkReply (badReply.data(), badReply.size()), "oversized reply sequence rejected");
    badReply = replyData; badReply[12] = 0;
    check (! decodeFlLinkReply (badReply.data(), badReply.size()), "unknown reply command rejected");

    FlLinkConnection connection;
    check (! connection.isConnected() && ! connection.noteHelloSent (1), "source creation is never sufficient for readiness");
    check (! connection.begin (0) && ! connection.begin (flLinkMaxToken + 1), "invalid connection tokens rejected");
    check (connection.begin (123) && ! connection.isConnected(), "starting a valid instance remains unconnected");
    check (! connection.acceptReply ({ 123, 42, FlLinkCommand::hello, 0 }, 0), "unsolicited acknowledgement cannot connect");
    check (! connection.noteHelloSent (flLinkMaxSequence + 1), "invalid hello sequence rejected");
    connection.noteHelloSent (42);
    check (! connection.acceptReply ({ 124, 42, FlLinkCommand::hello, 0 }, 10)
           && ! connection.acceptReply ({ 123, 41, FlLinkCommand::hello, 0 }, 10)
           && ! connection.acceptReply ({ 123, 42, FlLinkCommand::note, 0 }, 10)
           && ! connection.isConnected(), "cross-instance/stale/nonhello replies cannot establish readiness");
    check (connection.acceptReply ({ 123, 42, FlLinkCommand::hello, 0 }, 100)
           && connection.isConnected(), "only matching accepted hello connects");
    check (! connection.acceptReply ({ 123, 42, FlLinkCommand::hello, 0 }, 200), "replayed acknowledgement is not fresh evidence");
    connection.tick (1599);
    check (connection.isConnected(), "connection remains live before timeout");
    connection.tick (1600);
    check (! connection.isConnected(), "connection expires exactly at 1500ms without fresh host evidence");
    connection.noteHelloSent (43);
    check (connection.acceptReply ({ 123, 43, FlLinkCommand::hello, 0 }, 1700), "fresh handshake reconnects after timeout");
    connection.noteHelloSent (44);
    check (! connection.acceptReply ({ 124, 44, FlLinkCommand::hello, 1 }, 1800) && connection.isConnected(),
           "another instance cannot disconnect the current link");
    check (! connection.acceptReply ({ 123, 44, FlLinkCommand::hello, 1 }, 1800) && ! connection.isConnected(),
           "matching rejected handshake immediately disconnects");
    connection.noteHelloSent (45);
    connection.acceptReply ({ 123, 45, FlLinkCommand::hello, 0 }, 2000);
    connection.tick (1999);
    check (! connection.isConnected(), "clock reversal never leaves a stale link ready");
    connection.noteHelloSent (46);
    connection.acceptReply ({ 123, 46, FlLinkCommand::hello, 0 }, 3000);
    connection.begin (456);
    check (! connection.isConnected(), "new instance token clears prior connection");
    connection.noteHelloSent (46);
    check (! connection.acceptReply ({ 123, 46, FlLinkCommand::hello, 0 }, 3100), "previous instance acknowledgement rejected after restart");
    check (connection.acceptReply ({ 456, 46, FlLinkCommand::hello, 0 }, 3100), "new instance can connect independently");
    connection.stop();
    check (! connection.isConnected() && ! connection.noteHelloSent (47)
           && ! connection.acceptReply ({ 456, 46, FlLinkCommand::hello, 0 }, 3200), "stop invalidates token and pending acknowledgements");

    if (failures != 0) return EXIT_FAILURE;
    std::cout << "FL link protocol and connection tests passed\n";
    return EXIT_SUCCESS;
}
