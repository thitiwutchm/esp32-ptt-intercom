"""Check that a WAV recorded by tools/sip_phone.py --tone holds its 440 Hz tone (echoed back)."""
import math
import struct
import sys
import wave

w = wave.open(sys.argv[1])
n = w.getnframes()
pcm = struct.unpack("<%dh" % n, w.readframes(n))
secs = n / w.getframerate()
# Goertzel at 440 Hz per 20 ms frame, relative to the total energy
c = 2 * math.cos(2 * math.pi * 440 / w.getframerate())
tone = 0.0
for i in range(0, n - 159, 160):
    s1 = s2 = 0.0
    for x in pcm[i:i + 160]:
        s1, s2 = x + c * s1 - s2, s1
    tone += 2 * (s1 * s1 + s2 * s2 - c * s1 * s2) / 160
ratio = tone / (sum(x * x for x in pcm) or 1)
print("  %s: %.1f s recorded, %.0f%% of the energy at 440 Hz" % (sys.argv[1].split("/")[-1], secs, 100 * ratio))
sys.exit(0 if secs > 1.5 and ratio > 0.8 else 1)
