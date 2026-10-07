#!/usr/bin/env python3
"""A PC peer for the ESP32 PTT intercom: test with one board and a laptop.

Python 3.8+, standard library only. The PC must be on the same Wi-Fi/LAN.

  # Show who is online and save every received talk to a WAV file
  python3 tools/ptt_peer.py listen --channel 1

  # Talk: send a WAV file (16 kHz, mono, 16-bit) to every device on channel 1
  python3 tools/ptt_peer.py talk hello.wav --channel 1

  # Talk with a generated test tone instead of a file, to one device
  python3 tools/ptt_peer.py talk --tone 2 --to 192.168.1.42

Protocol: see components/ptt_core/include/ptt_proto.h.
"""
import argparse
import math
import random
import socket
import struct
import sys
import threading
import time
import wave

PORT = 47000
HDR = struct.Struct("<2sBBIIHBBH")  # magic, version, type, device_id, talk_id, seq, channel, codec, len
VERSION = 1
HELLO, TALK_START, AUDIO, TALK_END = 1, 2, 3, 4
CODEC_PCM16, CODEC_ADPCM = 0, 1
RATE = 16000
FRAME = 320  # 20 ms
NAME_LEN = 16

# ------------------------------------------------------------------ IMA ADPCM (same as ptt_adpcm.c)

INDEX_TABLE = [-1, -1, -1, -1, 2, 4, 6, 8] * 2
STEP_TABLE = [
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60,
    66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371,
    408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707,
    1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132,
    7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767,
]


def _step(pred, index, code):
    s = STEP_TABLE[index]
    diff = s >> 3
    if code & 4:
        diff += s
    if code & 2:
        diff += s >> 1
    if code & 1:
        diff += s >> 2
    pred = pred - diff if code & 8 else pred + diff
    pred = max(-32768, min(32767, pred))
    index = max(0, min(88, index + INDEX_TABLE[code]))
    return pred, index


class AdpcmEncoder:
    def __init__(self):
        self.pred = 0
        self.index = 0

    def _sample(self, x):
        s = STEP_TABLE[self.index]
        diff = x - self.pred
        code = 0
        if diff < 0:
            code = 8
            diff = -diff
        if diff >= s:
            code |= 4
            diff -= s
        s >>= 1
        if diff >= s:
            code |= 2
            diff -= s
        s >>= 1
        if diff >= s:
            code |= 1
        self.pred, self.index = _step(self.pred, self.index, code)
        return code

    def encode(self, pcm):
        out = bytearray(struct.pack("<hBB", self.pred, self.index, 0))
        for i in range(0, len(pcm), 2):
            lo = self._sample(pcm[i])
            hi = self._sample(pcm[i + 1]) if i + 1 < len(pcm) else 0
            out.append(lo | (hi << 4))
        return bytes(out)


def adpcm_decode(data):
    if len(data) < 4 or data[2] > 88:
        return []
    pred, index = struct.unpack_from("<h", data)[0], data[2]
    out = []
    for b in data[4:]:
        for code in (b & 0x0F, b >> 4):
            pred, index = _step(pred, index, code)
            out.append(pred)
    return out

# ------------------------------------------------------------------ packets


def build(ptype, device_id, channel, talk_id=0, seq=0, codec=0, payload=b""):
    return HDR.pack(b"PT", VERSION, ptype, device_id, talk_id, seq & 0xFFFF, channel, codec, len(payload)) + payload


def parse(data):
    if len(data) < HDR.size:
        return None
    magic, ver, ptype, dev, talk, seq, ch, codec, plen = HDR.unpack_from(data)
    if magic != b"PT" or ver != VERSION or HDR.size + plen > len(data):
        return None
    return dict(type=ptype, device_id=dev, talk_id=talk, seq=seq, channel=ch, codec=codec,
                payload=data[HDR.size:HDR.size + plen])


def hello_payload(name, battery=255, state=0):
    return name.encode()[:NAME_LEN].ljust(NAME_LEN, b"\0") + bytes([battery, state])


def parse_hello(payload):
    if len(payload) < NAME_LEN + 2:
        return None
    name = payload[:NAME_LEN].split(b"\0")[0].decode(errors="replace")
    return name, payload[NAME_LEN], payload[NAME_LEN + 1]

# ------------------------------------------------------------------ peer


class Peer:
    def __init__(self, args):
        self.args = args
        self.device_id = args.id if args.id is not None else random.getrandbits(32)
        self.peers = {}  # device_id -> dict(ip, name, channel, battery, seen)
        self.lock = threading.Lock()
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        self.sock.bind(("", args.port))
        self.running = True
        self.talks = {}  # (device_id, talk_id) -> dict(frames={seq: pcm}, last=time, name)

    def log(self, msg):
        print(time.strftime("%H:%M:%S"), msg, flush=True)

    def hello_loop(self):
        while self.running:
            pkt = build(HELLO, self.device_id, self.args.channel, payload=hello_payload(self.args.name))
            try:
                self.sock.sendto(pkt, (self.args.broadcast, self.args.port))
            except OSError as e:
                self.log(f"broadcast failed: {e}")
            now = time.time()
            with self.lock:
                for dev in [d for d, p in self.peers.items() if now - p["seen"] > 6]:
                    self.log(f"- {self.peers[dev]['name']} ({dev:08x}) gone")
                    del self.peers[dev]
            self.flush_talks(now)
            time.sleep(2)

    def rx_loop(self):
        while self.running:
            try:
                data, (ip, _) = self.sock.recvfrom(2048)
            except OSError:
                break
            p = parse(data)
            if not p or p["device_id"] == self.device_id:
                continue
            now = time.time()
            dev = p["device_id"]
            with self.lock:
                if p["type"] == HELLO:
                    h = parse_hello(p["payload"])
                    if h:
                        if dev not in self.peers:
                            self.log(f"+ {h[0]} ({dev:08x}) {ip} ch {p['channel']} battery "
                                     f"{'?' if h[1] == 255 else str(h[1]) + '%'}")
                        self.peers[dev] = dict(ip=ip, name=h[0], channel=p["channel"], battery=h[1], seen=now)
                    continue
                if p["channel"] != self.args.channel:
                    continue
                peer = self.peers.setdefault(dev, dict(ip=ip, name=f"{dev:08x}", channel=p["channel"],
                                                       battery=255, seen=now))
                peer["seen"] = now
                key = (dev, p["talk_id"])
                if p["type"] in (TALK_START, AUDIO) and key not in self.talks:
                    self.log(f"> {peer['name']} talking")
                    self.talks[key] = dict(frames={}, last=now, name=peer["name"], ended=False)
                talk = self.talks.get(key)
                if not talk:
                    continue
                talk["last"] = now
                if p["type"] == AUDIO:
                    if p["codec"] == CODEC_ADPCM:
                        pcm = adpcm_decode(p["payload"])
                    else:
                        pcm = list(struct.unpack(f"<{len(p['payload']) // 2}h", p["payload"]))
                    talk["frames"][p["seq"]] = pcm
                elif p["type"] == TALK_END:
                    talk["ended"] = True
            self.flush_talks(now)

    def flush_talks(self, now):
        """Write finished talks (TALK_END or 0.5 s of silence) to WAV."""
        with self.lock:
            done = [k for k, t in self.talks.items() if t["frames"] and (t["ended"] or now - t["last"] > 0.5)]
            done += [k for k, t in self.talks.items() if not t["frames"] and now - t["last"] > 5]
            for key in done:
                t = self.talks.pop(key)
                if not t["frames"]:
                    continue
                seqs = sorted(t["frames"])
                # Order by sequence across the 16-bit wrap.
                base = seqs[0]
                seqs.sort(key=lambda s: (s - base) & 0xFFFF)
                span = ((seqs[-1] - seqs[0]) & 0xFFFF) + 1
                pcm = []
                for i in range(span):
                    pcm += t["frames"].get((seqs[0] + i) & 0xFFFF, [0] * FRAME)
                name = f"talk_{time.strftime('%H%M%S')}_{t['name']}.wav".replace("/", "_")
                with wave.open(name, "wb") as w:
                    w.setnchannels(1)
                    w.setsampwidth(2)
                    w.setframerate(RATE)
                    w.writeframes(struct.pack(f"<{len(pcm)}h", *pcm))
                lost = span - len(t["frames"])
                self.log(f"< {t['name']} done: {span * 20 / 1000:.1f} s, {lost} of {span} frames lost -> {name}")

    def targets(self):
        if self.args.to:
            return self.args.to
        with self.lock:
            return [p["ip"] for p in self.peers.values() if p["channel"] == self.args.channel]

    def talk(self, pcm):
        talk_id = random.getrandbits(32)
        enc = AdpcmEncoder()
        targets = self.targets()
        if not targets:
            self.log("no one online on this channel (use --to IP to send anyway)")
            return
        self.log(f"talking to {', '.join(targets)} for {len(pcm) / RATE:.1f} s")
        frames = [pcm[i:i + FRAME] for i in range(0, len(pcm), FRAME)]
        start = time.monotonic()
        for seq, f in enumerate(frames):
            f = f + [0] * (FRAME - len(f))
            pkts = []
            if seq < 3:
                pkts.append(build(TALK_START, self.device_id, self.args.channel, talk_id))
            if self.args.pcm:
                pkts.append(build(AUDIO, self.device_id, self.args.channel, talk_id, seq, CODEC_PCM16,
                                  struct.pack(f"<{FRAME}h", *f)))
            else:
                pkts.append(build(AUDIO, self.device_id, self.args.channel, talk_id, seq, CODEC_ADPCM, enc.encode(f)))
            for ip in targets:
                for pkt in pkts:
                    self.sock.sendto(pkt, (ip, self.args.port))
            # Pace at real time like the microphone would.
            delay = start + (seq + 1) * 0.02 - time.monotonic()
            if delay > 0:
                time.sleep(delay)
        for _ in range(3):
            for ip in targets:
                self.sock.sendto(build(TALK_END, self.device_id, self.args.channel, talk_id), (ip, self.args.port))
            time.sleep(0.02)
        self.log("done")


def load_pcm(args):
    if args.tone:
        n = int(args.tone * RATE)
        return [int(8000 * math.sin(2 * math.pi * 440 * i / RATE) * (0.6 + 0.4 * math.sin(2 * math.pi * 2 * i / RATE)))
                for i in range(n)]
    with wave.open(args.wav, "rb") as w:
        if w.getnchannels() != 1 or w.getsampwidth() != 2 or w.getframerate() != RATE:
            sys.exit("WAV must be 16 kHz, mono, 16-bit (e.g. ffmpeg -i in.mp3 -ar 16000 -ac 1 out.wav)")
        data = w.readframes(w.getnframes())
    return list(struct.unpack(f"<{len(data) // 2}h", data))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--channel", type=int, default=1)
    ap.add_argument("--name", default="PC")
    ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--broadcast", default="255.255.255.255", help="e.g. 192.168.1.255")
    ap.add_argument("--id", type=lambda s: int(s, 0), help="device id (default random)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("listen", help="announce, list peers, save received talks to WAV")
    t = sub.add_parser("talk", help="send audio as one push-to-talk")
    t.add_argument("wav", nargs="?", help="16 kHz mono 16-bit WAV")
    t.add_argument("--tone", type=float, help="send a test tone of this many seconds instead")
    t.add_argument("--to", nargs="*", help="device IPs (default: everyone seen on the channel)")
    t.add_argument("--pcm", action="store_true", help="send PCM16 instead of ADPCM")
    args = ap.parse_args()
    if not 1 <= args.channel <= 16:
        sys.exit("channel must be 1..16")

    peer = Peer(args)
    threading.Thread(target=peer.rx_loop, daemon=True).start()
    threading.Thread(target=peer.hello_loop, daemon=True).start()
    peer.log(f"{args.name} ({peer.device_id:08x}) on channel {args.channel}, UDP {args.port}")
    try:
        if args.cmd == "listen":
            while True:
                time.sleep(1)
        else:
            if not args.wav and not args.tone:
                sys.exit("give a WAV file or --tone SECONDS")
            pcm = load_pcm(args)
            if not args.to:
                peer.log("discovering devices for 3 s ...")
                time.sleep(3)
            peer.talk(pcm)
            time.sleep(0.5)
    except KeyboardInterrupt:
        pass
    peer.running = False


if __name__ == "__main__":
    main()
