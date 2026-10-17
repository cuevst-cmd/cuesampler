# name=CUE SAMPLER Link
# supportedDevices=CUE SAMPLER Link

"""Instance-addressed CUE laptop input for FL's documented controller API.

Candidate integration: automatic input enabling and actual Piano roll recording
have NOT been verified in FL Studio. A reply confirms this script processed a
command, not that FL recorded it. No FL preferences or global routing are changed.
API: https://www.image-line.com/fl-studio-learning/fl-studio-online-manual/html/midi_scripting.htm
"""

import math
import socket
import struct
import time

import channels
import plugins


_MAGIC = bytes((0x7D, ord("C"), ord("U"), ord("E")))
_TOKEN_MAX = 0x1FFFFF
_SEQUENCE_MASK = 0xFFFFFFF
_SEQUENCE_HALF = 0x8000000
_HEARTBEAT_SECONDS = 3.0
_MAX_SESSIONS = 64
_sessions = {}
_reply_socket = None


class _Session:
    def __init__(self, token, port, target):
        self.token = token
        self.port = port
        self.target = target
        self.last_sequence = None
        self.last_request = None
        self.last_status = None
        self.last_seen = time.monotonic()
        # Each key remembers its original target; selection changes are irrelevant.
        self.held = {}


def _decode_7bit(values):
    return sum(value << (7 * index) for index, value in enumerate(values))


def _reply(token, port, sequence, command, status):
    global _reply_socket
    if not (1 <= token <= _TOKEN_MAX and 1 <= port <= 65535):
        return
    try:
        if _reply_socket is None:
            candidate = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            try:
                candidate.setblocking(False)
            except Exception:
                candidate.close()
                raise
            _reply_socket = candidate
        packet = b"CUE1" + struct.pack("<IIBB", token, sequence, command, status)
        _reply_socket.sendto(packet, ("127.0.0.1", port))
    except (OSError, ValueError, struct.error):
        # Lost replies are safe: retransmission is acknowledged without replaying.
        pass


def _matches_instance(index, token):
    try:
        name = plugins.getPluginName(index, -1, 0, True)
        if name.strip().upper() != "CUE SAMPLER":
            return False
        for parameter in range(plugins.getParamCount(index, -1, True)):
            if plugins.getParamName(parameter, index, -1, True) == "CUE Link Instance":
                value = plugins.getParamValue(parameter, index, -1, True)
                return (math.isfinite(value) and 0.0 <= value <= 1.0
                        and int(value * _TOKEN_MAX + 0.5) == token)
    except Exception:
        # Missing/removed/unsupported plugins must never fall back to selection.
        pass
    return False


def _global_channel_indices():
    try:
        # channelCount(1) includes hidden groups; these indices are already global.
        return range(channels.channelCount(1))
    except TypeError:
        # Older APIs only expose the visible group, with an explicit conversion.
        return [channels.getChannelIndex(index)
                for index in range(channels.channelCount())]


def _find_target(token):
    try:
        found = [index for index in _global_channel_indices()
                 if _matches_instance(index, token)]
        # A duplicate token is ambiguous: refuse instead of playing another CUE.
        return found[0] if len(found) == 1 else None
    except Exception:
        return None


def _target(session):
    if not _matches_instance(session.target, session.token):
        session.target = _find_target(session.token)
    return session.target


def _note_off(session, key, original_target):
    target = original_target
    if not _matches_instance(target, session.token):
        target = _find_target(session.token)
    if target is not None:
        # FL's own Fire script uses negative velocity for release, not zero.
        channels.midiNoteOn(target, key[1], -127, key[0])


def _release(session):
    for key, target in tuple(session.held.items()):
        try:
            _note_off(session, key, target)
        except Exception:
            pass
    session.held.clear()


def _expire_sessions():
    now = time.monotonic()
    for token, session in tuple(_sessions.items()):
        if now - session.last_seen > _HEARTBEAT_SECONDS:
            _release(session)
            del _sessions[token]


def _process(token, port, sequence, command, channel, note, velocity):
    _expire_sessions()
    session = _sessions.get(token)
    if command == 1:
        if session is not None and session.port != port:
            _release(session)
            del _sessions[token]
            session = None
        if session is None:
            target = _find_target(token)
            if target is None or len(_sessions) >= _MAX_SESSIONS:
                return 1
            session = _Session(token, port, target)
            _sessions[token] = session
    elif session is None or session.port != port:
        return 1 if session is None else 2

    request = (command, channel, note, velocity)
    if session.last_sequence is not None:
        delta = (sequence - session.last_sequence) & _SEQUENCE_MASK
        if delta == 0:
            # A sequence identifies one command; reusing it for another is invalid.
            return session.last_status if request == session.last_request else 2
        if delta >= _SEQUENCE_HALF:
            return 2

    status = 0
    try:
        target = _target(session)
        if command == 3:
            _release(session)
        elif target is None:
            status = 1
        elif command == 2:
            key = (channel, note)
            if velocity > 0 and key not in session.held:
                channels.midiNoteOn(target, note, velocity, channel)
                session.held[key] = target
            elif velocity == 0 and key in session.held:
                _note_off(session, key, session.held[key])
                del session.held[key]
    except Exception:
        status = 1

    session.last_sequence = sequence
    session.last_request = request
    session.last_status = status
    session.last_seen = time.monotonic()
    return status


def OnSysEx(event):
    try:
        raw = bytes(event.sysex)
    except (TypeError, ValueError):
        return
    framed = raw.startswith(b"\xF0" + _MAGIC)
    if not framed and not raw.startswith(_MAGIC):
        return  # Other manufacturers/products retain FL's usual behavior.
    event.handled = True
    payload = raw[1:-1] if framed else raw
    if (not framed or not raw.endswith(b"\xF7") or len(payload) != 19
            or any(value > 127 for value in payload) or payload[4] != 1):
        # Only reply when the invalid packet still supplies valid addressing.
        address = raw[1:] if framed else raw
        if len(address) >= 16 and all(value <= 127 for value in address[:16]):
            _reply(_decode_7bit(address[6:9]), _decode_7bit(address[9:12]),
                   _decode_7bit(address[12:16]), address[5], 2)
        return
    command = payload[5]
    token = _decode_7bit(payload[6:9])
    port = _decode_7bit(payload[9:12])
    sequence = _decode_7bit(payload[12:16])
    channel, note, velocity = payload[16:19]
    if (command not in (1, 2, 3) or not 1 <= token <= _TOKEN_MAX
            or not 1 <= port <= 65535 or channel > 15
            or (command != 2 and (channel != 0 or note != 0 or velocity != 0))):
        _reply(token, port, sequence, command, 2)
        return
    status = _process(token, port, sequence, command, channel, note, velocity)
    _reply(token, port, sequence, command, status)


def OnIdle():
    _expire_sessions()


def OnDeInit():
    global _reply_socket
    for session in tuple(_sessions.values()):
        _release(session)
    _sessions.clear()
    if _reply_socket is not None:
        _reply_socket.close()
        _reply_socket = None
