#include "FlRecordingLink.h"

namespace cuesampler
{
FlRecordingLink::FlRecordingLink (std::uint32_t instanceToken) : token (instanceToken)
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());
    if (! connection.begin (token) || ! replies.bindToPort (0, "127.0.0.1")) return;
    replyPort = replies.getBoundPort();
   #if JUCE_MAC
    static std::weak_ptr<juce::MidiOutput> sharedSource;
    source = sharedSource.lock();
    if (source == nullptr)
    {
        source = juce::MidiOutput::createNewDevice ("CUE SAMPLER Link");
        sharedSource = source;
    }
   #endif
    poll();
}

FlRecordingLink::~FlRecordingLink() { releaseAll(); }

void FlRecordingLink::poll()
{
    jassert (juce::MessageManager::getInstance()->isThisTheMessageThread());
    const auto now = static_cast<std::uint64_t> (juce::Time::getMillisecondCounterHiRes());
    // Bound the work even if another local process floods this ephemeral port.
    for (int i = 0; i < 32 && replies.waitUntilReady (true, 0) == 1; ++i)
    {
        std::array<std::uint8_t, flLinkReplySize + 1> bytes {};
        juce::String sender;
        int senderPort = 0;
        const auto size = replies.read (bytes.data(), (int) bytes.size(), false, sender, senderPort);
        if (sender != "127.0.0.1" || size < 0) continue;
        if (const auto reply = decodeFlLinkReply (bytes.data(), (size_t) size))
        {
            if (reply->command == FlLinkCommand::hello) connection.acceptReply (*reply, now);
            // Hello acknowledgements are the readiness authority. A delayed
            // rejection for an older note must not tear down a newer session.
        }
    }
    connection.tick (now);
    if (source != nullptr && replyPort > 0 && (lastHelloMs == 0 || now - lastHelloMs >= 400))
    {
        send (FlLinkCommand::hello);
        lastHelloMs = now;
    }
}

void FlRecordingLink::send (FlLinkCommand command, int channelZero, int note, int velocity)
{
    if (source == nullptr || replyPort <= 0) return;
    sequence = (sequence + 1) & flLinkMaxSequence;
    const FlLinkPacket packet { command, token, (std::uint32_t) replyPort, sequence,
                               (std::uint8_t) channelZero, (std::uint8_t) note, (std::uint8_t) velocity };
    if (const auto bytes = encodeFlLinkPacket (packet))
    {
        if (command == FlLinkCommand::hello) connection.noteHelloSent (sequence);
        source->sendMessageNow (juce::MidiMessage::createSysExMessage (bytes->data(), (int) bytes->size()));
    }
}

bool FlRecordingLink::sendNote (const juce::MidiMessage& message)
{
    if ((! isConnected() && ! message.isNoteOff()) || (! message.isNoteOn() && ! message.isNoteOff())) return false;
    send (FlLinkCommand::note, message.getChannel() - 1, message.getNoteNumber(),
          message.isNoteOn() ? message.getVelocity() : 0);
    return true;
}

void FlRecordingLink::releaseAll() { send (FlLinkCommand::allOff); }
}
