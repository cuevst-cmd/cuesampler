// Standalone diagnostic source. It never changes FL settings or project data.
// Start only for an authorized live FL test. Input: "on CHANNEL NOTE",
// "off CHANNEL NOTE", "quit". Port exists only while this process runs.
#import <CoreMIDI/CoreMIDI.h>
#import <Foundation/Foundation.h>
#include <iostream>
#include <string>

int main()
{
    @autoreleasepool
    {
        MIDIClientRef client = 0;
        MIDIEndpointRef source = 0;
        const auto clientStatus = MIDIClientCreate (CFSTR("CUE Link Probe"), nullptr, nullptr, &client);
        if (clientStatus != noErr) { std::cerr << "MIDI client error " << clientStatus << '\n'; return 1; }
        const auto sourceStatus = MIDISourceCreate (client, CFSTR("CUE SAMPLER Link Probe"), &source);
        if (sourceStatus != noErr) { MIDIClientDispose (client); std::cerr << "MIDI source error " << sourceStatus << '\n'; return 1; }
        std::cout << "Probe source created; no FL settings were changed.\n" << std::flush;
        std::string command;
        int channel = 0, note = 0;
        while (std::cin >> command && command != "quit")
        {
            if (! (std::cin >> channel >> note)) break;
            if ((command != "on" && command != "off") || channel < 1 || channel > 16 || note < 0 || note > 127)
            { std::cout << "Invalid event\n" << std::flush; continue; }
            const Byte bytes[] = { (Byte) ((command == "on" ? 0x90 : 0x80) | (channel - 1)), (Byte) note,
                                   (Byte) (command == "on" ? 100 : 0) };
            MIDIPacketList packets {};
            MIDIPacketListAdd (&packets, sizeof (packets), MIDIPacketListInit (&packets), 0, sizeof (bytes), bytes);
            const auto status = MIDIReceived (source, &packets);
            std::cout << "Sent " << command << " channel " << channel << " note " << note << ": " << status << '\n' << std::flush;
        }
        // Termination releases every diagnostic channel before removing source.
        for (int channelIndex = 0; channelIndex < 16; ++channelIndex)
        {
            const Byte bytes[] = { (Byte) (0xB0 | channelIndex), 123, 0 };
            MIDIPacketList packets {};
            MIDIPacketListAdd (&packets, sizeof (packets), MIDIPacketListInit (&packets), 0, sizeof (bytes), bytes);
            MIDIReceived (source, &packets);
        }
        MIDIEndpointDispose (source);
        MIDIClientDispose (client);
    }
}
