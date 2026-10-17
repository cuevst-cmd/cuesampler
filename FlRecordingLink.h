#pragma once

#include "FlLinkProtocol.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_core/juce_core.h>

namespace cuesampler
{
// Editor/message thread only. A created port is not evidence that FL receives
// it: only the controller's matching instance acknowledgement establishes that.
class FlRecordingLink
{
public:
    explicit FlRecordingLink (std::uint32_t instanceToken);
    ~FlRecordingLink();
    void poll();
    bool isConnected() const noexcept { return connection.isConnected(); }
    bool sendNote (const juce::MidiMessage&);
    void releaseAll();

private:
    void send (FlLinkCommand, int channelZero = 0, int note = 0, int velocity = 0);
    std::shared_ptr<juce::MidiOutput> source;
    juce::DatagramSocket replies { false };
    FlLinkConnection connection;
    const std::uint32_t token;
    std::uint32_t sequence = 0;
    std::uint64_t lastHelloMs = 0;
    int replyPort = 0;
    JUCE_DECLARE_NON_COPYABLE (FlRecordingLink)
};
}
