#!/usr/bin/env python3
"""Controller protocol tests with FL API stubs; never starts or changes FL.

These verify addressing and release behavior only. FL automatic input enabling,
actual recorded notes, and recorded channel colors still require host testing.
"""

import importlib.util
from pathlib import Path
import struct
import sys
import types
import unittest
from unittest import mock


SCRIPT = (Path(__file__).resolve().parents[1] / "assets" / "fl-studio"
          / "CUE SAMPLER Link" / "device_CueSamplerLink.py")
TOKEN_A = 123456
TOKEN_B = 654321
TOKEN_MAX = 0x1FFFFF
SEQUENCE_MASK = 0xFFFFFFF


def encoded(value, size):
    return [(value >> (7 * index)) & 127 for index in range(size)]


def packet(token=TOKEN_A, port=31234, sequence=1, command=1,
           channel=0, note=0, velocity=0):
    return bytes([0xF0, 0x7D, ord("C"), ord("U"), ord("E"), 1, command]
                 + encoded(token, 3) + encoded(port, 3)
                 + encoded(sequence, 4) + [channel, note, velocity, 0xF7])


class Host:
    def __init__(self):
        self.instances = {0: ("Other synth", 1), 1: ("CUE SAMPLER", TOKEN_A),
                          2: ("CUE SAMPLER", TOKEN_B)}
        self.events = []
        self.api_calls = []
        self.visible = [2]
        self.channels = types.ModuleType("channels")
        self.plugins = types.ModuleType("plugins")
        self.channels.channelCount = self.channel_count
        self.channels.getChannelIndex = lambda index: self.visible[index]
        self.channels.midiNoteOn = self.midi_note
        self.plugins.getPluginName = self.plugin_name
        self.plugins.getParamCount = self.parameter_count
        self.plugins.getParamName = self.parameter_name
        self.plugins.getParamValue = self.parameter_value

    def channel_count(self, global_count=0):
        self.api_calls.append(("channelCount", global_count))
        return max(self.instances, default=-1) + 1 if global_count else len(self.visible)

    def midi_note(self, index, note, velocity, channel=-1):
        if index not in self.instances:
            raise ValueError("removed plugin")
        self.events.append((index, note, velocity, channel))

    def plugin_name(self, index, slot=-1, user_name=0, use_global=False):
        assert slot == -1 and user_name == 0 and use_global
        self.api_calls.append(("getPluginName", index))
        return self.instances[index][0]

    def parameter_count(self, index, slot=-1, use_global=False):
        assert slot == -1 and use_global
        return 2

    def parameter_name(self, parameter, index, slot=-1, use_global=False):
        assert slot == -1 and use_global
        return "Unrelated control" if parameter == 0 else "CUE Link Instance"

    def parameter_value(self, parameter, index, slot=-1, use_global=False):
        assert parameter == 1 and slot == -1 and use_global
        return self.instances[index][1] / TOKEN_MAX


class ControllerTests(unittest.TestCase):
    def setUp(self):
        self.host = Host()
        spec = importlib.util.spec_from_file_location("cue_link_controller_under_test", SCRIPT)
        self.controller = importlib.util.module_from_spec(spec)
        with mock.patch.dict(sys.modules, {"channels": self.host.channels,
                                          "plugins": self.host.plugins}):
            spec.loader.exec_module(self.controller)
        self.now = 0.0
        self.controller.time = types.SimpleNamespace(monotonic=lambda: self.now)
        self.replies = []
        self.original_reply = self.controller._reply
        self.controller._reply = lambda *args: self.replies.append(args)

    def tearDown(self):
        self.controller.OnDeInit()

    def send(self, raw=None, **kwargs):
        event = types.SimpleNamespace(sysex=packet(**kwargs) if raw is None else raw,
                                      handled=False)
        self.controller.OnSysEx(event)
        return event

    def status(self):
        return self.replies[-1][-1]

    def test_all_pitches_and_channels_keep_exact_pairing(self):
        self.send()
        sequence = 1
        for channel in range(16):
            for note in range(128):
                sequence += 1
                self.send(sequence=sequence, command=2, channel=channel,
                          note=note, velocity=100)
                sequence += 1
                self.send(sequence=sequence, command=2, channel=channel, note=note)
                self.assertEqual(self.status(), 0)
                self.assertEqual(self.host.events[-2:], [(1, note, 100, channel),
                                                        (1, note, -127, channel)])
        self.assertEqual(len(self.host.events), 16 * 128 * 2)

    def test_hello_addresses_hidden_global_instance_without_selection(self):
        self.send()
        self.assertEqual(self.status(), 0)
        self.assertIn(("channelCount", 1), self.host.api_calls)
        self.send(sequence=2, command=2, note=48, velocity=100)
        self.assertEqual(self.host.events, [(1, 48, 100, 0)])
        self.assertEqual(self.host.visible, [2])

    def test_legacy_group_indices_are_converted(self):
        def old_count():
            return len(self.host.visible)
        self.host.channels.channelCount = old_count
        self.send(token=TOKEN_B)
        self.send(token=TOKEN_B, sequence=2, command=2, note=120, velocity=127)
        self.assertEqual(self.host.events, [(2, 120, 127, 0)])

    def test_separate_instances_do_not_follow_selection(self):
        self.send(token=TOKEN_A)
        self.send(token=TOKEN_B, port=31235)
        self.send(token=TOKEN_A, sequence=2, command=2, note=60, velocity=80)
        self.send(token=TOKEN_B, port=31235, sequence=2, command=2, note=60, velocity=90)
        self.host.visible = [0]
        self.send(token=TOKEN_A, sequence=3, command=2, note=60)
        self.send(token=TOKEN_B, port=31235, sequence=3, command=2, note=60)
        self.assertEqual(self.host.events, [(1, 60, 80, 0), (2, 60, 90, 0),
                                           (1, 60, -127, 0), (2, 60, -127, 0)])

    def test_missing_or_ambiguous_instance_never_plays_another_plugin(self):
        self.send(token=98765)
        self.assertEqual(self.status(), 1)
        self.send(token=98765, sequence=2, command=2, note=48, velocity=100)
        self.assertEqual(self.status(), 1)
        self.host.instances[3] = ("CUE SAMPLER", TOKEN_A)
        self.send()
        self.assertEqual(self.status(), 1)
        self.assertEqual(self.host.events, [])

    def test_reordered_or_removed_target_is_not_cross_routed(self):
        self.send()
        self.send(sequence=2, command=2, channel=2, note=48, velocity=100)
        self.host.instances[1], self.host.instances[2] = (self.host.instances[2],
                                                       self.host.instances[1])
        self.send(sequence=3, command=2, channel=2, note=48)
        self.assertEqual(self.host.events[-1], (2, 48, -127, 2))
        self.send(sequence=4, command=2, note=60, velocity=100)
        del self.host.instances[2]
        self.send(sequence=5, command=2, note=60)
        self.assertEqual(self.status(), 1)
        self.assertEqual(len(self.host.events), 3)

    def test_notes_require_hello_and_matching_reply_port(self):
        self.send(command=2, note=48, velocity=100)
        self.assertEqual(self.status(), 1)
        self.send()
        self.send(port=31235, sequence=2, command=2, note=48, velocity=100)
        self.assertEqual(self.status(), 2)
        self.assertEqual(self.host.events, [])

    def test_retries_autorepeat_old_packets_and_wrap(self):
        self.send(sequence=SEQUENCE_MASK - 1)
        note_on = packet(sequence=SEQUENCE_MASK, command=2, note=48, velocity=100)
        self.send(raw=note_on)
        self.send(raw=note_on)
        self.assertEqual(self.status(), 0)
        self.assertEqual(len(self.host.events), 1)
        self.send(sequence=SEQUENCE_MASK, command=2, note=49, velocity=100)
        self.assertEqual(self.status(), 2)
        self.send(sequence=0, command=2, note=48, velocity=100)
        self.assertEqual(self.status(), 0)
        self.assertEqual(len(self.host.events), 1)
        self.send(sequence=SEQUENCE_MASK, command=2, note=48)
        self.assertEqual(self.status(), 2)
        self.send(sequence=1, command=2, note=48)
        self.assertEqual(self.host.events[-1], (1, 48, -127, 0))
        self.send(sequence=1, command=2, note=48)
        self.assertEqual(len(self.host.events), 2)

    def test_all_off_only_releases_owned_notes(self):
        self.send()
        self.send(sequence=2, command=2, channel=5, note=127, velocity=100)
        self.send(sequence=3, command=2, channel=1, note=0, velocity=100)
        self.send(sequence=4, command=3)
        self.assertEqual(self.status(), 0)
        self.assertEqual(self.host.events[-2:], [(1, 127, -127, 5), (1, 0, -127, 1)])
        self.send(sequence=5, command=3)
        self.assertEqual(len(self.host.events), 4)

    def test_heartbeat_timeout_and_deinit_release_original_notes(self):
        self.send()
        self.send(sequence=2, command=2, channel=15, note=120, velocity=100)
        self.now = 2.9
        self.send(sequence=3)  # Real hello heartbeat keeps this session alive.
        self.now = 5.8
        self.controller.OnIdle()
        self.assertEqual(len(self.host.events), 1)
        self.now = 6.0
        self.controller.OnIdle()
        self.assertEqual(self.host.events[-1], (1, 120, -127, 15))
        self.assertEqual(self.controller._sessions, {})
        self.send(sequence=4)
        self.send(sequence=5, command=2, note=0, velocity=1)
        self.controller.OnDeInit()
        self.assertEqual(self.host.events[-1], (1, 0, -127, 0))
        self.assertEqual(self.controller._sessions, {})

    def test_replayed_heartbeat_does_not_keep_notes_alive(self):
        self.send()
        self.send(sequence=2, command=2, note=48, velocity=100)
        self.now = 2.9
        self.send(sequence=2, command=2, note=48, velocity=100)
        self.now = 3.1
        self.controller.OnIdle()
        self.assertEqual(self.host.events[-1], (1, 48, -127, 0))

    def test_new_reply_port_releases_previous_session(self):
        self.send()
        self.send(sequence=2, command=2, note=48, velocity=100)
        self.send(port=31235, sequence=1)
        self.assertEqual(self.host.events[-1], (1, 48, -127, 0))
        self.send(port=31235, sequence=2, command=2, note=49, velocity=100)
        self.assertEqual(self.host.events[-1], (1, 49, 100, 0))

    def test_unrelated_sysex_unchanged_and_invalid_protocol_consumed(self):
        unrelated = bytes((0xF0, 0x7D, ord("O"), ord("T"), ord("H"), 0xF7))
        self.assertFalse(self.send(raw=unrelated).handled)
        self.assertEqual(self.replies, [])
        invalid_packets = [packet()[:-1], packet()[1:], packet() + b"\x00"]
        for index, value in ((5, 2), (6, 127), (17, 16), (18, 128)):
            changed = bytearray(packet())
            changed[index] = value
            invalid_packets.append(bytes(changed))
        invalid_packets.extend([packet(token=0), packet(port=0), packet(port=65536),
                                packet(command=1, note=1)])
        for raw in invalid_packets:
            with self.subTest(raw=raw):
                before = len(self.replies)
                self.assertTrue(self.send(raw=raw).handled)
                if len(self.replies) > before:
                    self.assertEqual(self.status(), 2)
        self.assertEqual(self.host.events, [])

    def test_host_errors_return_unavailable_without_propagating(self):
        self.send()
        self.host.channels.midiNoteOn = mock.Mock(side_effect=RuntimeError("unsafe now"))
        self.send(sequence=2, command=2, note=48, velocity=100)
        self.assertEqual(self.status(), 1)
        self.assertEqual(self.controller._sessions[TOKEN_A].held, {})

    def test_udp_ack_has_exact_loopback_wire_format(self):
        endpoint = mock.Mock()
        network = types.SimpleNamespace(AF_INET=2, SOCK_DGRAM=2,
                                        socket=mock.Mock(return_value=endpoint))
        self.controller.socket = network
        self.controller._reply = self.original_reply
        self.send(port=31234, sequence=SEQUENCE_MASK)
        network.socket.assert_called_once_with(network.AF_INET, network.SOCK_DGRAM)
        endpoint.setblocking.assert_called_once_with(False)
        endpoint.sendto.assert_called_once_with(
            b"CUE1" + struct.pack("<IIBB", TOKEN_A, SEQUENCE_MASK, 1, 0),
            ("127.0.0.1", 31234))
        self.controller.OnDeInit()
        endpoint.close.assert_called_once_with()

    def test_lost_udp_reply_does_not_repeat_note(self):
        endpoint = mock.Mock()
        endpoint.sendto.side_effect = BlockingIOError("queue full")
        self.controller.socket = types.SimpleNamespace(
            AF_INET=2, SOCK_DGRAM=2, socket=mock.Mock(return_value=endpoint))
        self.controller._reply = self.original_reply
        self.send()
        self.send(sequence=2, command=2, note=48, velocity=100)
        self.send(sequence=2, command=2, note=48, velocity=100)
        self.assertEqual(self.host.events, [(1, 48, 100, 0)])


if __name__ == "__main__":
    unittest.main()
