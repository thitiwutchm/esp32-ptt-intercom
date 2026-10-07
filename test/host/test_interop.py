#!/usr/bin/env python3
"""Check that tools/ptt_peer.py speaks exactly what the firmware's ptt_core speaks.

Loads the C code as a shared library (make -C test/host libptt_core.so) and
compares packet bytes and ADPCM in both directions.
"""
import ctypes
import math
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
import ptt_peer as py  # noqa: E402

lib = ctypes.CDLL(os.path.join(HERE, "libptt_core.so"))


class Hdr(ctypes.Structure):
    _fields_ = [("type", ctypes.c_uint8), ("device_id", ctypes.c_uint32), ("talk_id", ctypes.c_uint32),
                ("seq", ctypes.c_uint16), ("channel", ctypes.c_uint8), ("codec", ctypes.c_uint8),
                ("payload_len", ctypes.c_uint16)]


class AdpcmState(ctypes.Structure):
    _fields_ = [("predictor", ctypes.c_int16), ("index", ctypes.c_uint8)]


failures = 0


def check(cond, what):
    global failures
    if not cond:
        failures += 1
        print("FAIL:", what)


# Packet bytes
payload = bytes(range(10))
h = Hdr(py.AUDIO, 0xA1B2C3D4, 0x11223344, 65535, 9, py.CODEC_ADPCM, len(payload))
buf = ctypes.create_string_buffer(64)
n = lib.ptt_proto_build(buf, 64, ctypes.byref(h), payload)
c_pkt = buf.raw[:n]
py_pkt = py.build(py.AUDIO, 0xA1B2C3D4, 9, 0x11223344, 65535, py.CODEC_ADPCM, payload)
check(c_pkt == py_pkt, "AUDIO packet bytes")
p = py.parse(c_pkt)
check(p and p["seq"] == 65535 and p["payload"] == payload and p["channel"] == 9, "python parses C packet")

# HELLO payload
lib.ptt_hello_encode.restype = ctypes.c_size_t


class Hello(ctypes.Structure):
    _fields_ = [("name", ctypes.c_char * 17), ("battery", ctypes.c_uint8), ("state", ctypes.c_uint8)]


hello = Hello(b"Kitchen", 87, 1)
hb = ctypes.create_string_buffer(32)
n = lib.ptt_hello_encode(hb, 32, ctypes.byref(hello))
check(hb.raw[:n] == py.hello_payload("Kitchen", 87, 1), "HELLO payload bytes")

# ADPCM both ways over several frames (encoder state carries across frames)
st = AdpcmState(0, 0)
enc = py.AdpcmEncoder()
Pcm = ctypes.c_int16 * py.FRAME
for frame in range(10):
    pcm = [int(9000 * math.sin(2 * math.pi * 523 * (frame * py.FRAME + i) / py.RATE)) for i in range(py.FRAME)]
    out = ctypes.create_string_buffer(py.FRAME)
    n = lib.ptt_adpcm_encode(ctypes.byref(st), Pcm(*pcm), py.FRAME, out)
    c_enc = out.raw[:n]
    check(c_enc == enc.encode(pcm), f"ADPCM encode frame {frame}")
    dec = Pcm()
    m = lib.ptt_adpcm_decode(c_enc, len(c_enc), dec, py.FRAME)
    check(m == py.FRAME and list(dec) == py.adpcm_decode(c_enc), f"ADPCM decode frame {frame}")

print("interop:", "OK" if not failures else f"{failures} failures")
sys.exit(1 if failures else 0)
