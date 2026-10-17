# name=CUE SAMPLER Link Probe
# supportedDevices=CUE SAMPLER Link Probe

# Diagnostic candidate only: not included in the plugin or installed by a
# release build. Actual auto-enable and recorded colors must be checked in FL.
# Official API: https://www.image-line.com/fl-studio-learning/fl-studio-online-manual/html/midi_scripting.htm

def OnInit():
    print("CUE Link probe: automatic controller assignment reached OnInit")


def OnMidiMsg(event):
    if event.midiId in (0x80, 0x90):
        # Preserve FL's extra flags while retaining the incoming note color.
        event.midiChanEx = (event.midiChanEx & ~0xF) | (event.midiChan & 0xF)
    # FL handles playback and recording through its normal input pipeline.
    event.handled = False
