# ESP32 PTT Intercom

วิทยุสื่อสารแบบ **Push-to-Talk** ผ่าน Wi-Fi วง LAN เดียวกัน สำหรับบอร์ด ESP32-S3 (N16R8)
กดค้างเพื่อพูด ปล่อยเพื่อฟัง ใช้ได้โดยไม่ต้องต่ออินเทอร์เน็ต ไม่ต้องมีเซิร์ฟเวอร์

| บอร์ด | จอ | เสียง | ปุ่มพูด |
|---|---|---|---|
| **EchoEar / ESP-VoCat** (ค่าเริ่มต้น) | กลม 1.85" 360×360 ST77916 (QSPI) + ทัช CST816 | ES8311 + ES7210 + แอมป์ NS4150B | กดค้างปุ่มกลางจอ หรือปุ่ม BOOT |
| **CUBE** (xingzhi-cube-1.54tft-wifi) | สี่เหลี่ยม 1.54" 240×240 ST7789 (SPI) | ไมค์ I2S + แอมป์ I2S | กดค้างปุ่ม BOOT |

ขา GPIO ของทั้งสองบอร์ดอ้างอิงจาก [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32)
(`boards/espressif/esp-vocat` และ `boards/nologo/xingzhi-cube-1.54tft-wifi`) และ EchoEar
จะตรวจรุ่นแผงวงจร V1.0 / V1.2 ให้เองเหมือนใน xiaozhi

## การใช้งาน

- **ช่อง 1–16** ทุกเครื่องในช่องเดียวกันได้ยินกันหมด
- กดค้างเพื่อพูด (ขอบจอเป็น **สีแดง**) ปล่อยแล้วอีกฝั่งจะได้ยินเสียง roger beep
- ตอนมีคนพูด ขอบจอเป็น **สีเขียว** และแสดงชื่อคนพูด ถ้ากดพูดตอนนี้จะขึ้น "Channel busy" (สีส้ม)
- ถ้าสองเครื่องกดพร้อมกัน เครื่องที่ device id น้อยกว่าได้พูด อีกเครื่องเปลี่ยนเป็นฟังทันที
- พูดได้ครั้งละไม่เกิน 60 วินาที กันปุ่มค้าง
- จอหรี่เองหลังไม่ได้ใช้ 30 วินาที แล้วสว่างขึ้นเมื่อมีเสียงเข้าหรือกดปุ่ม

| | EchoEar | CUBE |
|---|---|---|
| เปลี่ยนช่อง | แตะ ◀ ▶ ด้านบน | กดค้าง VOL+ / VOL− |
| ระดับเสียง | แตะ − + ด้านล่าง | กด VOL+ / VOL− |

ช่องและระดับเสียงที่ตั้งไว้ถูกจำไว้ใน NVS ข้ามการรีบูต

## Build และ Flash

ต้องใช้ [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/) **v5.4 ขึ้นไป** (ทดสอบการคอมไพล์กับ v5.4.2)

```bash
. $IDF_PATH/export.sh

# 1) ตั้ง Wi-Fi (ต้องเป็น 2.4 GHz) และค่าอื่นๆ: เมนู "PTT Intercom"
idf.py -B build-echoear -D SDKCONFIG=build-echoear/sdkconfig menuconfig

# 2) EchoEar
idf.py -B build-echoear -D SDKCONFIG=build-echoear/sdkconfig build flash monitor

# CUBE
idf.py -B build-cube -D SDKCONFIG=build-cube/sdkconfig \
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.cube" build flash monitor
```

ค่าในเมนู **PTT Intercom**:

| ตัวเลือก | ค่าเริ่มต้น | หมายเหตุ |
|---|---|---|
| Board | EchoEar | หรือ CUBE |
| Wi-Fi SSID / password | – | **ต้องตั้ง** |
| UDP port | 47000 | ทุกเครื่องต้องตรงกัน |
| Channel / volume on first boot | 1 / 70 | |
| Voice codec | IMA ADPCM 64 kbit/s | หรือ PCM16 256 kbit/s ฝั่งรับถอดได้ทั้งสองแบบ |
| Jitter buffer prefill | 3 เฟรม (60 ms) | เพิ่มถ้าเสียงกระตุกบนเครือข่ายที่ช้า |
| Mic gain (EchoEar) / Mic shift (CUBE) | 30 dB / 12 | ปรับถ้าเสียงเบาหรือแตก |

GitHub Actions คอมไพล์ทั้งสองบอร์ดและแนบไฟล์ `.bin` ไว้ใน artifacts ของทุก build

**เราเตอร์:** ต้องปิด *AP / Client isolation* (มักเปิดไว้ใน Guest Wi-Fi) ไม่งั้นเครื่องจะมองไม่เห็นกัน

## ทดสอบกับคอมพิวเตอร์ (ใช้บอร์ดเดียวได้)

`tools/ptt_peer.py` เป็นอีกเครื่องหนึ่งบน PC ใช้ Python 3 ไม่ต้องติดตั้งอะไรเพิ่ม (PC ต้องอยู่วง LAN เดียวกัน)

```bash
# ดูว่าใครออนไลน์ และบันทึกเสียงที่ได้รับเป็นไฟล์ WAV
python3 tools/ptt_peer.py listen --channel 1

# ส่งเสียงทดสอบ 2 วินาทีไปให้ทุกเครื่องในช่อง 1
python3 tools/ptt_peer.py talk --tone 2 --channel 1

# ส่งไฟล์ WAV (16 kHz mono 16-bit) ไปที่บอร์ดเครื่องเดียว
python3 tools/ptt_peer.py talk voice.wav --to 192.168.1.42
```

ถ้า broadcast ไม่ผ่าน ให้ใส่ `--broadcast 192.168.1.255` (ใช้ subnet ของคุณ)

## โครงสร้าง

```
components/ptt_core/      ตรรกะล้วน (ไม่พึ่ง ESP-IDF) ทดสอบบน PC ได้
  ptt_proto      รูปแบบแพ็กเก็ต UDP
  ptt_floor      ใครได้พูด: state machine IDLE / TX / RX + การชนกัน + timeout
  ptt_peers      ตารางเครื่องที่ออนไลน์ (จาก HELLO)
  ptt_jitter     jitter buffer เรียงเฟรมตาม seq, รายงานเฟรมหาย, ตัด latency หลัง Wi-Fi สะดุด
  ptt_adpcm      IMA ADPCM 4:1 แต่ละเฟรมถอดได้ด้วยตัวเอง
main/
  ptt_app.c      รวมทุกอย่าง: task ส่งเสียง / เล่นเสียง / ปุ่ม / HELLO
  board/         ขา GPIO, จอ, ทัช, แบต ของแต่ละบอร์ด
  audio/         I2S + ES8311/ES7210 (esp_codec_dev) หรือ I2S ธรรมดา, เสียง beep
  net/           Wi-Fi station, UDP socket
  ui/            หน้าจอ LVGL (ปรับตามขนาดและรูปทรงจอ)
tools/ptt_peer.py         peer บน PC สำหรับทดสอบ
test/host/                unit test (make -C test/host)
```

### Task

| Core | Task | Priority | หน้าที่ |
|---|---|---|---|
| 1 | `audio_tx` | 20 | อ่านไมค์ทุก 20 ms → encode → unicast ไปทุกเครื่องในช่อง |
| 1 | `audio_rx` | 19 | jitter buffer → decode → ลำโพง (เปิดแอมป์เฉพาะตอนเล่น) |
| 0 | `net_rx` | 18 | รับ UDP → floor control → jitter buffer |
| 0 | `app` | 5 | ปุ่ม/ทัช, timeout, HELLO ทุก 2 วินาที, แบต, อัปเดตจอ |

### โปรโตคอล (UDP พอร์ตเดียว)

header 18 ไบต์ little-endian: `'P' 'T'`, version, type, device_id(4), talk_id(4), seq(2), channel, codec, payload_len(2)

| type | ส่งเมื่อ | ไปที่ |
|---|---|---|
| HELLO | ทุก 2 วินาที (ชื่อ, % แบต, สถานะ) | broadcast |
| TALK_START | 3 เฟรมแรกของการกดพูด | unicast ทุกเครื่องในช่อง |
| AUDIO | ทุก 20 ms (ADPCM 164 ไบต์ หรือ PCM 640 ไบต์) | unicast ทุกเครื่องในช่อง |
| TALK_END | 3 ครั้งหลังปล่อยปุ่ม | unicast ทุกเครื่องในช่อง |

เสียงใช้ unicast ไม่ใช้ broadcast/multicast เพราะบน Wi-Fi แพ็กเก็ตแบบนั้นส่งด้วยความเร็วต่ำสุด
และไม่มี ACK จึงหายบ่อย ส่วน DSCP ตั้งเป็น EF เพื่อให้ Wi-Fi จัดเข้าคิวเสียง (WMM voice)

ดีเลย์โดยประมาณ: เฟรม 20 + ส่ง 5–20 + jitter 60 + เล่น ~20 ≈ **110–130 ms**

## ทดสอบ

```bash
make -C test/host     # unit test ของ ptt_core + ตรวจว่า tools/ptt_peer.py ตรงกับโค้ด C ทุกไบต์
```

## สถานะและสิ่งที่ยังไม่ได้ทำ

- โค้ดนี้คอมไพล์ผ่านทั้งสองบอร์ด และ unit test ผ่าน แต่ **ยังไม่ได้ทดสอบบนฮาร์ดแวร์จริง**
  ถ้าเจอปัญหา ให้ดู log ผ่าน `idf.py monitor` ก่อน
- ตั้ง Wi-Fi ผ่าน menuconfig อย่างเดียว (ยังไม่มีการตั้งผ่านมือถือ)
- ยังไม่มี Opus: ADPCM ใช้แบนด์วิดท์ 64 kbit/s ต่อผู้ฟังหนึ่งคน ซึ่งพอสำหรับ LAN
- UI เป็นภาษาอังกฤษ เพราะฟอนต์ในตัวของ LVGL ไม่มีอักษรไทย
- ไม่มีการเข้ารหัส: ใครอยู่ใน LAN เดียวกันก็ฟังได้

## เครดิต

ลำดับคำสั่ง init ของจอ EchoEar (`main/board/echoear_lcd_init.h`) และขา GPIO
นำมาจาก xiaozhi-esp32 (MIT License, Copyright (c) 2025 Shenzhen Xinzhi Future Technology Co., Ltd. and Project Contributors)
