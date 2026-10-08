#!/usr/bin/env python3
"""A tiny SIP phone (and mini PBX) for testing the intercom from a laptop.

No account, no Asterisk. Set the device's "PBX address" (phone setup page)
to this computer's IP, then:

  # call the device (it rings; tap BOOT to answer)
  python3 tools/sip_phone.py call 192.168.1.50

  # wait for the device to call (tap BOOT on the device); answers by itself
  python3 tools/sip_phone.py answer

While this runs the device also registers here, so its phone icon shows OK.

Audio: with the `sounddevice` package (pip install sounddevice) you talk
through the laptop's microphone and speaker; use headphones, or the device
hears itself back. Without it, or with --tone, a test tone is sent instead.
--record saves what the device sends to a WAV file. Ctrl+C hangs up.

Python 3.8+. Standard library only, sounddevice optional.
"""
import argparse
import math
import queue
import random
import signal
import socket
import struct
import sys
import threading
import time
import wave

RATE = 8000
FRAME = 160  # 20 ms of G.711

# ------------------------------------------------------------------ G.711 (pure Python; audioop is gone in 3.13)

def _ulaw_encode_sample(x):
    x = max(-32768, min(32767, x)) >> 2
    if x < 0:
        x, mask = -x, 0x7F
    else:
        mask = 0xFF
    x = min(x, 8159) + (0x84 >> 2)
    seg = 0
    for end in (0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF, 0x1FFF):
        if x <= end:
            break
        seg += 1
    if seg >= 8:
        return 0x7F ^ mask
    return ((seg << 4) | ((x >> (seg + 1)) & 0x0F)) ^ mask


def _ulaw_decode_sample(u):
    u = ~u & 0xFF
    t = (((u & 0x0F) << 3) + 0x84) << ((u & 0x70) >> 4)
    return (0x84 - t) if (u & 0x80) else (t - 0x84)


def _alaw_decode_sample(a):
    a ^= 0x55
    t = (a & 0x0F) << 4
    seg = (a & 0x70) >> 4
    if seg == 0:
        t += 8
    elif seg == 1:
        t += 0x108
    else:
        t = (t + 0x108) << (seg - 1)
    return t if a & 0x80 else -t


ULAW_DEC = [_ulaw_decode_sample(i) for i in range(256)]
ALAW_DEC = [_alaw_decode_sample(i) for i in range(256)]
ULAW_ENC = bytes(_ulaw_encode_sample(i - 32768) for i in range(65536))


def ulaw_encode(samples):
    return bytes(ULAW_ENC[s + 32768] for s in samples)


def alaw_encode(samples):
    out = bytearray()
    for x in samples:
        x = max(-32768, min(32767, x)) >> 3
        mask = 0xD5 if x >= 0 else 0x55
        if x < 0:
            x = -x - 1
        seg = 0
        for end in (0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF):
            if x <= end:
                break
            seg += 1
        if seg >= 8:
            out.append(0x7F ^ mask)
            continue
        a = seg << 4
        a |= ((x >> 1) if seg < 2 else (x >> seg)) & 0x0F
        out.append(a ^ mask)
    return bytes(out)


def g711_decode(pt, data):
    table = ALAW_DEC if pt == 8 else ULAW_DEC
    return [table[b] for b in data]

# ------------------------------------------------------------------ SIP helpers


def rand_hex(n=8):
    return "%0*x" % (n, random.getrandbits(4 * n))


def parse(data):
    try:
        text = data.decode("utf-8", "replace")
    except Exception:
        return None
    head, _, body = text.partition("\r\n\r\n")
    lines = head.split("\r\n")
    first = lines[0].split(" ", 2)
    msg = {"body": body, "hdrs": []}
    if first[0] == "SIP/2.0":
        msg["status"] = int(first[1])
    elif len(first) == 3:
        msg["method"], msg["uri"] = first[0], first[1]
    else:
        return None
    for line in lines[1:]:
        if ":" in line:
            k, v = line.split(":", 1)
            msg["hdrs"].append((k.strip(), v.strip()))
    return msg


COMPACT = {"v": "via", "f": "from", "t": "to", "i": "call-id", "m": "contact", "l": "content-length"}


def hget(msg, name, all_=False):
    name = name.lower()
    vals = [v for k, v in msg["hdrs"] if COMPACT.get(k.lower(), k.lower()) == name]
    return vals if all_ else (vals[0] if vals else "")


def tag_of(value):
    for part in value.split(";")[1:]:
        if part.strip().startswith("tag="):
            return part.strip()[4:]
    return ""


def uri_of(value):
    if "<" in value:
        return value[value.index("<") + 1:value.index(">")]
    return value.split(";")[0].strip()


def sdp(local_ip, rtp_port, pts=(0, 8)):
    names = {0: "PCMU", 8: "PCMA"}
    sid = random.getrandbits(31)
    return ("v=0\r\no=- %d %d IN IP4 %s\r\ns=sip_phone\r\nc=IN IP4 %s\r\nt=0 0\r\n"
            "m=audio %d RTP/AVP %s\r\n%sa=ptime:20\r\na=sendrecv\r\n" % (
                sid, sid, local_ip, local_ip, rtp_port, " ".join(map(str, pts)),
                "".join("a=rtpmap:%d %s/8000\r\n" % (p, names[p]) for p in pts)))


def parse_sdp(body):
    ip, port, pts = None, None, []
    for line in body.splitlines():
        if line.startswith("c=IN IP4 "):
            ip = line[9:].split("/")[0].strip()
        elif line.startswith("m=audio "):
            f = line.split()
            port = int(f[1])
            pts = [int(p) for p in f[3:] if p.isdigit()]
    pt = next((p for p in pts if p in (0, 8)), None)
    return ip, port, pt

# ------------------------------------------------------------------ audio


class Audio:
    """Mic/speaker through sounddevice, or a test tone; optional WAV recording."""

    def __init__(self, tone, record):
        self.tone = tone
        self.phase = 0
        self.out_q = queue.Queue(maxsize=50)
        self.in_q = queue.Queue(maxsize=50)
        self.stream = None
        self.rec = None
        if record:
            self.rec = wave.open(record, "wb")
            self.rec.setnchannels(1)
            self.rec.setsampwidth(2)
            self.rec.setframerate(RATE)
        if tone:
            return
        try:
            import sounddevice as sd  # noqa
        except Exception:
            print("  (sounddevice not installed: sending a test tone; pip install sounddevice to talk)")
            self.tone = True
            return
        self._open(sd)

    def _open(self, sd):
        """8 kHz if the sound card allows it, else 48 kHz with a simple 6:1 conversion."""
        for rate in (8000, 48000):
            try:
                self.ratio = rate // RATE
                self.stream = sd.RawStream(samplerate=rate, channels=1, dtype="int16",
                                           blocksize=FRAME * self.ratio, callback=self._cb)
                self.stream.start()
                print("  talking through the laptop mic/speaker (%d Hz). Use headphones." % rate)
                return
            except Exception as e:
                last = e
        print("  sound card not usable (%s): sending a test tone" % last)
        self.tone = True

    def _cb(self, indata, outdata, frames, t, status):
        mic = struct.unpack("<%dh" % frames, bytes(indata))
        # decimate: average each group of `ratio` samples
        r = self.ratio
        frame = [sum(mic[i:i + r]) // r for i in range(0, frames, r)] if r > 1 else list(mic)
        try:
            self.in_q.put_nowait(frame)
        except queue.Full:
            pass
        try:
            far = self.out_q.get_nowait()
        except queue.Empty:
            far = [0] * FRAME
        if r > 1:
            far = [s for s in far for _ in range(r)]
        outdata[:] = struct.pack("<%dh" % len(far), *far)

    def mic_frame(self):
        if self.tone:
            f = [int(6000 * math.sin(2 * math.pi * 440 * (self.phase + i) / RATE)) for i in range(FRAME)]
            self.phase += FRAME
            time.sleep(0.02)
            return f
        return self.in_q.get()

    def play(self, samples):
        if self.rec:
            self.rec.writeframes(struct.pack("<%dh" % len(samples), *samples))
        if self.stream:
            try:
                self.out_q.put_nowait(samples)
            except queue.Full:
                pass

    def close(self):
        if self.stream:
            self.stream.stop()
            self.stream.close()
        if self.rec:
            self.rec.close()

# ------------------------------------------------------------------ the phone


class Phone:
    def __init__(self, args):
        self.args = args
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("", args.port))
        self.rtp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.rtp.bind(("", args.rtp_port))
        self.rtp.settimeout(0.2)
        self.local_ip = args.local_ip
        self.call = None  # dict while a call exists
        self.active = threading.Event()
        self.ended = threading.Event()
        self.lock = threading.Lock()

    def log(self, s):
        print(time.strftime("%H:%M:%S"), s, flush=True)

    def my_ip_towards(self, ip):
        if self.local_ip:
            return self.local_ip
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect((ip, 9))
        a = s.getsockname()[0]
        s.close()
        return a

    def send(self, text, addr):
        if self.args.trace:
            print(">>>> %s\n%s" % (addr, text))
        self.sock.sendto(text.encode(), addr)

    def via(self, ip):
        return "SIP/2.0/UDP %s:%d;branch=z9hG4bK%s;rport" % (ip, self.args.port, rand_hex(12))

    def respond(self, req, addr, code, reason, to_tag="", extra="", body=""):
        lines = ["SIP/2.0 %d %s" % (code, reason)]
        lines += ["Via: " + v for v in hget(req, "via", True)]
        lines.append("From: " + hget(req, "from"))
        to = hget(req, "to")
        if to_tag and "tag=" not in to:
            to += ";tag=" + to_tag
        lines.append("To: " + to)
        lines.append("Call-ID: " + hget(req, "call-id"))
        lines.append("CSeq: " + hget(req, "cseq"))
        lines.append("User-Agent: sip_phone.py")
        if extra:
            lines.append(extra)
        if body:
            lines.append("Content-Type: application/sdp")
        lines.append("Content-Length: %d" % len(body.encode()))
        text = "\r\n".join(lines) + "\r\n\r\n" + body
        self.send(text, addr)
        return text

    def request(self, method, cseq, body="", branch_via=None):
        c = self.call
        ip = c["local_ip"]
        lines = ["%s %s SIP/2.0" % (method, c["target"]),
                 "Via: " + (branch_via or self.via(ip)),
                 "Max-Forwards: 70",
                 "From: %s;tag=%s" % (c["local_uri"], c["local_tag"]),
                 "To: %s%s" % (c["remote_uri"], (";tag=" + c["remote_tag"]) if c.get("remote_tag") else ""),
                 "Call-ID: " + c["callid"],
                 "CSeq: %d %s" % (cseq, method),
                 "Contact: <sip:100@%s:%d>" % (ip, self.args.port),
                 "User-Agent: sip_phone.py"]
        if body:
            lines.append("Content-Type: application/sdp")
        lines.append("Content-Length: %d" % len(body.encode()))
        self.send("\r\n".join(lines) + "\r\n\r\n" + body, c["addr"])

    # -------------------------------------------------------------- media

    def media(self):
        c = self.call
        audio = Audio(self.args.tone, self.args.record)
        seq, ts, ssrc = random.getrandbits(16), random.getrandbits(32), random.getrandbits(32)
        stop = threading.Event()
        got = [0]

        def rx():
            while not stop.is_set():
                try:
                    data, _ = self.rtp.recvfrom(2048)
                except socket.timeout:
                    continue
                except OSError:
                    break
                if len(data) < 12 or data[0] >> 6 != 2:
                    continue
                pt = data[1] & 0x7F
                if pt not in (0, 8):
                    continue
                off = 12 + 4 * (data[0] & 0x0F)
                got[0] += 1
                audio.play(g711_decode(pt, data[off:]))

        t = threading.Thread(target=rx, daemon=True)
        t.start()
        dest = (c["media_ip"], c["media_port"])
        last_report = time.time()
        try:
            while not self.ended.is_set():
                frame = audio.mic_frame()
                payload = ulaw_encode(frame) if c["pt"] == 0 else alaw_encode(frame)
                hdr = struct.pack("!BBHII", 0x80, c["pt"], seq & 0xFFFF, ts & 0xFFFFFFFF, ssrc)
                self.rtp.sendto(hdr + payload, dest)
                seq += 1
                ts += FRAME
                if time.time() - last_report > 5:
                    last_report = time.time()
                    self.log("  audio: %d frames received from the device" % got[0])
        finally:
            stop.set()
            audio.close()

    # -------------------------------------------------------------- calls

    def start_call(self, dest, number):
        ip, _, port = dest.partition(":")
        local_ip = self.my_ip_towards(ip)
        self.call = {
            "uac": True, "addr": (ip, int(port or 5060)), "local_ip": local_ip,
            "target": "sip:%s@%s" % (number, ip),
            "local_uri": '"Laptop" <sip:100@%s>' % local_ip, "remote_uri": "<sip:%s@%s>" % (number, ip),
            "local_tag": rand_hex(), "callid": "%s@%s" % (rand_hex(16), local_ip), "cseq": 1,
        }
        self.call["invite_via"] = self.via(local_ip)
        self.request("INVITE", 1, sdp(local_ip, self.args.rtp_port), self.call["invite_via"])
        self.log("calling sip:%s@%s ... (the device should ring; tap BOOT to answer)" % (number, ip))

    def hangup(self):
        c = self.call
        if not c or self.ended.is_set():
            return
        if self.active.is_set():
            c["cseq"] += 1
            self.request("BYE", c["cseq"])
            self.log("hung up")
        elif c.get("uac"):
            self.request("CANCEL", 1, branch_via=c["invite_via"])
            self.log("cancelled")
        self.ended.set()

    def handle(self, msg, addr):
        c = self.call
        if "status" in msg:  # response to our INVITE / BYE
            if not c or hget(msg, "call-id") != c["callid"]:
                return
            cseq_method = hget(msg, "cseq").split()[-1]
            code = msg["status"]
            if cseq_method != "INVITE":
                return
            if code in (180, 183):
                self.log("ringing on the device")
            elif 200 <= code < 300:
                c["remote_tag"] = tag_of(hget(msg, "to"))
                contact = uri_of(hget(msg, "contact")) if hget(msg, "contact") else c["target"]
                c["target"] = contact
                self.request("ACK", 1)
                if self.active.is_set():
                    return
                ip, port, pt = parse_sdp(msg["body"])
                if pt is None:
                    self.log("device answered without a G.711 codec?")
                    return
                c.update(media_ip=ip, media_port=port, pt=pt)
                self.log("answered (%s) - talk now, Ctrl+C to hang up" % ("PCMU" if pt == 0 else "PCMA"))
                self.active.set()
            elif code >= 300:
                # ACK the failure with the INVITE's branch
                c["remote_tag"] = tag_of(hget(msg, "to"))
                self.request("ACK", 1, branch_via=c["invite_via"])
                self.log("call failed: %d" % code)
                self.ended.set()
            return

        method = msg["method"]
        if method == "REGISTER":
            exp = hget(msg, "expires") or "300"
            self.respond(msg, addr, 200, "OK", rand_hex(),
                         "Contact: %s;expires=%s" % (hget(msg, "contact"), exp))
            if not getattr(self, "_reg_logged", False):
                self.log("the device registered here (%s)" % uri_of(hget(msg, "contact")))
                self._reg_logged = True
        elif method == "OPTIONS":
            self.respond(msg, addr, 200, "OK")
        elif method == "INVITE":
            if c and hget(msg, "call-id") == c["callid"]:
                if c.get("ok"):  # retransmission: our 200 got lost
                    self.send(c["ok"], addr)
                return
            if c:
                self.respond(msg, addr, 486, "Busy Here", rand_hex())
                return
            ip, port, pt = parse_sdp(msg["body"])
            if pt is None:
                self.respond(msg, addr, 488, "Not Acceptable Here", rand_hex())
                self.log("incoming call without PCMU/PCMA, refused")
                return
            local_ip = self.my_ip_towards(addr[0])
            tag = rand_hex()
            self.call = {
                "uac": False, "addr": addr, "local_ip": local_ip,
                "target": uri_of(hget(msg, "contact")) or "sip:200@%s" % addr[0],
                "local_uri": hget(msg, "to").split(";tag=")[0], "remote_uri": hget(msg, "from").split(";tag=")[0],
                "local_tag": tag, "remote_tag": tag_of(hget(msg, "from")),
                "callid": hget(msg, "call-id"), "cseq": 1, "media_ip": ip, "media_port": port, "pt": pt,
            }
            self.respond(msg, addr, 180, "Ringing", tag)
            self.log("incoming call from %s - answering" % hget(msg, "from").split("<")[0].strip(' "'))
            time.sleep(0.5)
            self.call["ok"] = self.respond(msg, addr, 200, "OK", tag, "Contact: <sip:100@%s:%d>" % (local_ip, self.args.port),
                         sdp(local_ip, self.args.rtp_port, (pt,)))
            self.active.set()
        elif method == "ACK":
            pass
        elif method == "BYE":
            self.respond(msg, addr, 200, "OK")
            if c and hget(msg, "call-id") == c["callid"]:
                self.log("the device hung up")
                self.ended.set()
        elif method == "CANCEL":
            self.respond(msg, addr, 200, "OK")
            self.log("the device cancelled the call")
            self.ended.set()
        else:
            self.respond(msg, addr, 501, "Not Implemented")

    def sip_loop(self):
        while True:
            try:
                data, addr = self.sock.recvfrom(4096)
            except OSError:
                return
            if self.args.trace:
                print("<<<< %s\n%s" % (addr, data.decode("utf-8", "replace")))
            msg = parse(data)
            if msg:
                with self.lock:
                    self.handle(msg, addr)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=5060, help="local SIP port (the device sends to 5060)")
    ap.add_argument("--rtp-port", type=int, default=40100)
    ap.add_argument("--local-ip", help="this computer's IP if the automatic guess is wrong")
    ap.add_argument("--tone", action="store_true", help="send a 440 Hz tone instead of the microphone")
    ap.add_argument("--record", metavar="WAV", help="save what the device sends")
    ap.add_argument("--trace", action="store_true", help="print SIP messages")
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("call", help="call the device")
    c.add_argument("device_ip", help="the device's IP (shown on its screen), optionally IP:port")
    c.add_argument("--number", default="200", help="the device's extension")
    sub.add_parser("answer", help="wait for the device to call, answer automatically")
    args = ap.parse_args()

    try:
        phone = Phone(args)
    except OSError as e:
        sys.exit("cannot open UDP port %d (%s). Close other SIP apps (Linphone...) or use --port." % (args.port, e))
    threading.Thread(target=phone.sip_loop, daemon=True).start()

    def stop(*_):
        raise KeyboardInterrupt

    signal.signal(signal.SIGTERM, stop)  # `kill` hangs up like Ctrl+C
    try:
        if args.cmd == "call":
            with phone.lock:
                phone.start_call(args.device_ip, args.number)
        else:
            print("waiting for the device: set its PBX address to this computer, then tap BOOT on it. Ctrl+C to quit")
        while not phone.ended.is_set():
            if phone.active.wait(0.2):
                phone.media()
                break
    except KeyboardInterrupt:
        pass
    with phone.lock:
        phone.hangup()
    time.sleep(0.3)
    if args.record:
        print("recorded to", args.record)


if __name__ == "__main__":
    main()
