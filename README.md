# ESP32 PTT Intercom

อินเตอร์คอมบนโต๊ะสำหรับบอร์ด ESP32-S3 (N16R8) ทำได้ 2 อย่างในเครื่องเดียว:

1. **วอ Push-to-Talk ในวง LAN:** กดค้างเพื่อพูด ปล่อยเพื่อฟัง ไม่ต้องมีเซิร์ฟเวอร์
2. **โทรศัพท์ SIP ผ่าน Asterisk:** เป็นเบอร์ภายในของ PBX โทรเข้าออกกับ Home Assistant dashboard ได้
   (ผ่าน [SIP Core](https://github.com/TECH7Fox/sipcore-hass-integration)) กดรับแล้วคุยพร้อมกันสองทาง
   ไม่ต้องกดพูด มีตัวตัดเสียงสะท้อน (AEC)

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

### โทรศัพท์ (SIP)

| สถานะ | ปุ่ม BOOT | จอสัมผัส (EchoEar) |
|---|---|---|
| ว่าง | **แตะ** = โทรหาเบอร์ที่ตั้งไว้ (เช่น HA) · **กดค้าง** = พูดวอ · **ค้าง 8 วินาที** = ตั้งค่าผ่านมือถือ | ปุ่ม 📞 ด้านล่าง = โทร · กดค้างปุ่มกลาง = พูดวอ |
| มีสายเข้า (เสียงเรียก, ขอบจอสีน้ำเงิน) | **แตะ** = รับสาย · **กดค้าง** = ปฏิเสธ | ปุ่มกลาง = รับ · ปุ่มแดงด้านล่าง = ปฏิเสธ |
| กำลังโทรออก / กำลังคุย | **แตะ** = วางสาย | ปุ่มกลางหรือปุ่มแดง = วางสาย |

- ระหว่างคุย ขอบจอเป็นสีเขียว และจอแสดงชื่อปลายสายกับเวลาที่คุย
- ระหว่างคุยไม่รับสัญญาณวอ และระหว่างใช้วอ สายที่โทรเข้ามาจะได้สัญญาณไม่ว่าง (486)
- สายที่ไม่ได้รับจะขึ้น "Missed: ชื่อ" ค้างไว้ 10 วินาที
- เปิดตัวเลือก *auto answer* ได้: ถ้า PBX ส่ง header `Call-Info: answer-after=0` มาด้วย เครื่องจะรับสายเองทันที

## ติดตั้งผ่านเว็บ (ไม่ต้องลง ESP-IDF)

1. ดาวน์โหลดไฟล์ factory ของบอร์ด: `esp32-ptt-intercom-echoear-factory.bin` หรือ `esp32-ptt-intercom-cube-factory.bin`
   จากหน้า **Releases** ของ repo (หรือจาก artifacts ของ GitHub Actions)
2. เปิด <https://web.esphome.io> ด้วย Chrome หรือ Edge บนคอมพิวเตอร์ ต่อบอร์ดด้วย USB แล้วกด **Connect**
3. กดไอคอน **⬆ (Install)** แล้วเลือกไฟล์ `.bin` จากข้อ 1 แล้วกด **Install**
   - **อย่ากด** "Prepare for first use" เพราะปุ่มนั้นจะลงเฟิร์มแวร์ของ ESPHome แทน
   - **อย่าใช้** ไอคอนตั้ง Wi-Fi ของหน้าเว็บนั้น ให้ตั้งผ่านมือถือตามหัวข้อถัดไปแทน
4. บอร์ดรีสตาร์ทแล้วเข้าโหมดตั้งค่าผ่านมือถือเอง

ไฟล์ factory เป็นไฟล์เดียวที่รวม bootloader, partition table และแอปไว้แล้ว เขียนที่ตำแหน่ง 0x0

## ตั้งค่า Wi-Fi และ SIP ผ่านมือถือ

ไม่ต้องแก้ค่าในโค้ด บอร์ดจะเข้าโหมดตั้งค่าเองเมื่อ:
- เปิดเครื่องครั้งแรก (ยังไม่มี Wi-Fi)
- ต่อ Wi-Fi ไม่ได้เลยภายใน 90 วินาทีหลังเปิดเครื่อง (เช่น รหัสผิด หรือย้ายบ้าน)
- **กดค้างปุ่ม BOOT 8 วินาที** (เข้าได้ทุกเมื่อ)

ขั้นตอน:
1. จอจะแสดง **QR code** พร้อมชื่อ Wi-Fi (`PTT-XXXX-Setup`) และรหัสผ่าน 8 หลักที่สุ่มใหม่ทุกครั้ง
2. สแกน QR ด้วยกล้องมือถือเพื่อต่อ Wi-Fi ของบอร์ด หน้าตั้งค่าจะเด้งขึ้นมาเอง
   (ถ้าไม่เด้ง ให้เปิด `http://192.168.4.1`)
3. เลือก Wi-Fi บ้าน ใส่รหัสผ่าน แล้วกรอก SIP: ที่อยู่ PBX, เบอร์ภายใน, รหัสผ่าน, เบอร์ที่จะให้ BOOT โทรไป และ auto answer
4. กด **บันทึกและรีสตาร์ท** บอร์ดจะเก็บค่าไว้แล้วต่อ Wi-Fi บ้าน

ข้อควรรู้:
- ถ้าบอร์ดต่อ Wi-Fi บ้านอยู่แล้ว หน้าเดียวกันเปิดจาก IP ของบอร์ดในบ้านได้ด้วย (จอจะแสดงที่อยู่ให้)
- หน้าตั้งค่าไม่ส่งรหัสผ่านเดิมกลับมาแสดง ถ้าเว้นช่องรหัสว่างไว้ จะใช้รหัสเดิม
- ออกจากโหมดตั้งค่าโดยไม่บันทึกได้ด้วยการกด BOOT สั้นๆ, กด ✕ บนจอ (EchoEar) หรือกดปุ่มในหน้าเว็บ
  ถ้าไม่ได้ใช้ 15 นาที โหมดนี้จะปิดเอง
- ค่าที่ตั้งจากมือถือจะแทนค่าใน menuconfig ทั้งหมด (ค่าใน menuconfig เป็นแค่ค่าเริ่มต้นจากโรงงาน)

## Build และ Flash

ต้องใช้ [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/) **v5.4 ขึ้นไป** (ทดสอบการคอมไพล์กับ v5.4.2)

```bash
. $IDF_PATH/export.sh

# 1) (ไม่บังคับ) ค่าเริ่มต้นต่างๆ: เมนู "PTT Intercom" ส่วน Wi-Fi และ SIP ตั้งผ่านมือถือได้
idf.py -B build-echoear -D SDKCONFIG=build-echoear/sdkconfig menuconfig

# 2) EchoEar
idf.py -B build-echoear -D SDKCONFIG=build-echoear/sdkconfig build flash monitor

# CUBE
idf.py -B build-cube -D SDKCONFIG=build-cube/sdkconfig \
       -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.cube" build flash monitor

# ไฟล์ factory ไฟล์เดียวสำหรับ web.esphome.io
cd build-cube && python -m esptool --chip esp32s3 merge_bin -o ../cube-factory.bin @flash_args
```

ค่าในเมนู **PTT Intercom**:

| ตัวเลือก | ค่าเริ่มต้น | หมายเหตุ |
|---|---|---|
| Board | EchoEar | หรือ CUBE |
| Wi-Fi SSID / password | ว่าง | ว่าง = ตั้งผ่านมือถือ |
| UDP port | 47000 | ทุกเครื่องต้องตรงกัน |
| Channel / volume on first boot | 1 / 70 | |
| Voice codec | IMA ADPCM 64 kbit/s | หรือ PCM16 256 kbit/s ฝั่งรับถอดได้ทั้งสองแบบ |
| Jitter buffer prefill | 3 เฟรม (60 ms) | เพิ่มถ้าเสียงกระตุกบนเครือข่ายที่ช้า |
| Mic gain (EchoEar) / Mic shift (CUBE) | 30 dB / 12 | ปรับถ้าเสียงเบาหรือแตก |

เมนูย่อย **SIP intercom (Asterisk / Home Assistant)**:

| ตัวเลือก | ค่าเริ่มต้น | หมายเหตุ |
|---|---|---|
| Register to a SIP PBX | เปิด | ปิดแล้วจะเหลือแค่วอ และ BOOT กลับไปเป็นปุ่มพูดทันทีที่กด |
| PBX address | 192.168.1.10 | IP ของ Asterisk (เช่น เครื่องที่รัน HA) ชื่อแบบ `.local` ใช้ไม่ได้ |
| Extension / Password | 200 / – | ต้องตรงกับที่ตั้งใน Asterisk |
| Caller name | Desk | ชื่อที่ปลายสายเห็น |
| Number BOOT calls | 100 | เบอร์ของ HA dashboard หรือ ring group |
| Auto answer | ปิด | เปิดแล้วรับเองเมื่อ PBX ขอ |
| Echo handling | AEC | หรือ Ducking (ลดเสียงไมค์ตอนปลายสายพูด) หรือ None |

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

## ทดสอบโทรด้วยโน้ตบุ๊ก (ยังไม่ต้องมี Asterisk)

**ตั้งบอร์ดก่อน** (เหมือนกันทุกโปรแกรม):

1. โน้ตบุ๊กกับบอร์ดต้องอยู่ Wi-Fi วงเดียวกัน ดู IP ของโน้ตบุ๊ก (เช่น `192.168.1.20`)
   ส่วน IP ของบอร์ดดูได้จากบรรทัดล่างสุดบนจอ
2. ตั้งบอร์ด (กดค้าง BOOT 8 วินาที): เปิด *ใช้งาน SIP*, ที่อยู่ PBX = **IP ของโน้ตบุ๊ก**, พอร์ต 5060,
   เบอร์ภายใน `200`, รหัสว่างได้, เบอร์ที่ BOOT โทรไป = `100`
   (บอร์ดรับสายเฉพาะที่มาจากที่อยู่ PBX)

### แบบ ก: `tools/sip_phone.py` (ไม่ต้องตั้งบัญชี)

สคริปต์ Python 3 ในโปรเจกต์นี้ ทำตัวเป็นทั้งโทรศัพท์และ PBX จิ๋ว บอร์ดจึงลงทะเบียนผ่าน (ไอคอน 📞 เป็นปกติ)
ปิดโปรแกรม SIP อื่น (Linphone ฯลฯ) ก่อน เพราะใช้พอร์ต 5060 เหมือนกัน

```sh
pip install sounddevice                      # ไม่บังคับ: ใช้ไมค์/ลำโพงโน้ตบุ๊ก ถ้าไม่ลงจะส่งเสียงทดสอบ 440 Hz แทน
python3 tools/sip_phone.py call 192.168.1.50 # โน้ตบุ๊กโทรหาบอร์ด (IP บอร์ด) บอร์ดดัง แตะ BOOT เพื่อรับ
python3 tools/sip_phone.py answer            # รอให้บอร์ดโทรมา (แตะ BOOT ที่บอร์ด) สคริปต์รับสายเอง
```

Ctrl+C = วางสาย ใช้หูฟังกับโน้ตบุ๊ก ไม่งั้นบอร์ดจะได้ยินเสียงตัวเองย้อนกลับ
`--tone` บังคับส่งเสียงทดสอบ, `--record เสียง.wav` อัดเสียงที่บอร์ดส่งมา, `--trace` แสดงข้อความ SIP
Windows: ถ้าไฟร์วอลล์ถามให้กด *อนุญาต* (UDP 5060 และ 40100)

### แบบ ข: softphone

* [MicroSIP](https://www.microsip.org/) (Windows) ไม่ต้องตั้งบัญชี พิมพ์ `200@<IP บอร์ด>` ในช่องโทรแล้วกดโทร
* [Linphone](https://www.linphone.org/) รุ่น 6 บังคับให้มีบัญชี: เลือก *Third-party SIP account*
  ชื่อผู้ใช้ `100`, โดเมน = IP บอร์ด, รหัสอะไรก็ได้, transport **UDP** แล้วโทร `sip:200@<IP บอร์ด>`
  (ขึ้นว่าลงทะเบียนไม่ได้ก็โทรได้)

บอร์ดจะมีเสียงเรียกและขึ้นชื่อผู้โทร แตะ BOOT เพื่อรับ แตะอีกครั้งเพื่อวางสาย
ถ้าให้บอร์ดโทรหา softphone ได้ softphone ต้องฟังที่ UDP 5060 (ไอคอน 📞 ! บนบอร์ดเป็นปกติ เพราะ softphone ไม่ใช่ PBX)
ถ้าไม่ติด: ให้ไฟร์วอลล์อนุญาต UDP 5060 และพอร์ตเสียง และเปิด codec **PCMU/PCMA (G.711)**

## ตั้งค่า Asterisk และ Home Assistant

```
HA Dashboard (SIP Core, เบอร์ 100) ◄─ WebRTC ─► Asterisk ◄─ SIP UDP 5060 + RTP G.711 ─► CUBE / EchoEar (เบอร์ 200)
```

Asterisk แปลงเสียงระหว่าง WebRTC (Opus แบบเข้ารหัส) ของเบราว์เซอร์ กับ G.711 แบบ RTP ธรรมดาของบอร์ดให้เอง

**1. เพิ่มบอร์ดเป็นเบอร์ใน Asterisk** (`pjsip.conf`, ถ้าใช้ Asterisk add-on ของ HA ให้ใส่ในไฟล์ config เพิ่มเติมตามเอกสารของ add-on)

```ini
[200]
type=endpoint
context=default          ; context เดียวกับเบอร์ของ HA
disallow=all
allow=ulaw,alaw
auth=200-auth
aors=200
direct_media=no
rtp_symmetric=yes
callerid="Desk" <200>

[200-auth]
type=auth
auth_type=userpass
username=200
password=ตั้งรหัสที่นี่

[200]
type=aor
max_contacts=1
remove_existing=yes
qualify_frequency=30
```

**2. dialplan** (`extensions.conf`) ให้โทรหาบอร์ดได้ ถ้าอยากให้บอร์ดรับเองทันที ให้ใส่ header auto-answer แบบนี้
แล้วเปิดตัวเลือก *Auto answer* ในบอร์ดด้วย:

```ini
exten => 200,1,Dial(PJSIP/200,30,b(autoanswer^s^1))
[autoanswer]
exten => s,1,Set(PJSIP_HEADER(add,Call-Info)=<sip:pbx>\;answer-after=0)
 same => n,Return()
```

**3. Home Assistant:** ติดตั้ง SIP Core ผ่าน HACS แล้วเพิ่มการ์ด `sip-contacts-card` ที่มีรายชื่อเบอร์ 200
HA ต้องเปิดผ่าน **HTTPS** เพราะเบราว์เซอร์ไม่ให้ใช้ไมค์บนหน้าเว็บที่ไม่ใช่ HTTPS ส่วนตอนบอร์ดโทรเข้า HA
ต้องมีคนเปิดหน้า dashboard หรือแอปไว้ถึงจะดัง

### เสียงสะท้อน (AEC)

- **EchoEar:** ES7210 อัดเสียงที่ส่งออกลำโพงกลับเข้ามาให้ (สัญญาณอ้างอิงจากฮาร์ดแวร์) AEC ของ ESP-SR จึงตัดเสียงสะท้อนได้ตรงจุด
- **CUBE:** ไมค์กับลำโพงใช้ I2S คนละพอร์ต ไม่มีสัญญาณอ้างอิงจากฮาร์ดแวร์ เฟิร์มแวร์จึงจำเสียงที่เล่นไว้เองเป็นสัญญาณอ้างอิง
  ตอนต่อสายติดจะเล่นเสียงกวาดความถี่สั้นๆ (ราว 0.3 วินาที) เพื่อวัดว่าเสียงจากลำโพงใช้เวลากี่ ms ถึงไมค์
  ระหว่างวัด ปลายสายจะไม่ได้ยินเสียงประมาณ 1 วินาที ถ้าวัดไม่สำเร็จ สายนั้นจะใช้โหมดลดเสียงไมค์ (ducking) แทน
  ดู log `echo path ... ms, confidence ...` ใน `idf.py monitor`
- **ถ้ายังมีเสียงสะท้อน:** ลดระดับเสียงลำโพง ลด gain ไมค์ (Mic shift ให้มากขึ้น) และใส่โฟมกั้นระหว่างลำโพงกับไมค์ในกล่อง

## โครงสร้าง

```
components/ptt_core/      ตรรกะล้วน (ไม่พึ่ง ESP-IDF) ทดสอบบน PC ได้
  ptt_proto      รูปแบบแพ็กเก็ต UDP
  ptt_floor      ใครได้พูด: state machine IDLE / TX / RX + การชนกัน + timeout
  ptt_peers      ตารางเครื่องที่ออนไลน์ (จาก HELLO)
  ptt_jitter     jitter buffer เรียงเฟรมตาม seq, รายงานเฟรมหาย, ตัด latency หลัง Wi-Fi สะดุด
  ptt_adpcm      IMA ADPCM 4:1 แต่ละเฟรมถอดได้ด้วยตัวเอง
components/voip_core/     SIP/RTP แบบ C ล้วน ทดสอบบน PC และกับ Asterisk จริงได้
  sip_ua         SIP UA: REGISTER (digest), โทรเข้า/ออก, CANCEL, BYE, re-INVITE, OPTIONS
  sip_msg, sdp   แยก message SIP และ SDP
  rtp, g711      RTP + G.711 µ-law / A-law
  resample       แปลง 16 kHz <-> 8 kHz
  echo_ref       สัญญาณอ้างอิงจากซอฟต์แวร์ + หา delay ด้วย cross-correlation
main/
  ptt_app.c      รวมทุกอย่าง: task ส่งเสียง / เล่นเสียง / ปุ่ม / HELLO / สถานะสาย
  call/          sip_client (socket + task) และ call_audio (AEC, jitter, G.711)
  setup/         โหมดตั้งค่าผ่านมือถือ: Wi-Fi AP, captive DNS, หน้าเว็บ (setup_page.html)
components/setup_core/    ตรวจค่าจากฟอร์ม, JSON, ตอบ DNS ของ captive portal (C ล้วน ทดสอบบน PC)
  board/         ขา GPIO, จอ, ทัช, แบต ของแต่ละบอร์ด
  audio/         I2S + ES8311/ES7210 (esp_codec_dev) หรือ I2S ธรรมดา, เสียง beep
  net/           Wi-Fi station, UDP socket
  ui/            หน้าจอ LVGL (ปรับตามขนาดและรูปทรงจอ)
tools/ptt_peer.py         peer บน PC สำหรับทดสอบ
tools/sip_phone.py        โทรศัพท์ SIP + PBX จิ๋วบนโน้ตบุ๊ก สำหรับทดสอบโทร
test/host/                unit test (make -C test/host)
```

### Task

| Core | Task | Priority | หน้าที่ |
|---|---|---|---|
| 1 | `audio_tx` | 20 | อ่านไมค์ทุก 20 ms → encode → unicast ไปทุกเครื่องในช่อง |
| 1 | `audio_rx` | 19 | jitter buffer → decode → ลำโพง (เปิดแอมป์เฉพาะตอนเล่น) |
| 0 | `net_rx` | 18 | รับ UDP → floor control → jitter buffer |
| 0 | `sip` | 17 | SIP + RTP: สถานะสาย, retransmit, ต่ออายุการลงทะเบียน |
| 0 | `echo_cal` | 3 | หา delay ลำโพง→ไมค์ ตอนเริ่มสาย (CUBE) |
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
make -C test/host        # unit test ของ ptt_core, voip_core, setup_core + ตรวจว่า tools/ptt_peer.py ตรงกับโค้ด C ทุกไบต์
test/asterisk/run.sh     # เปิด Asterisk จริง (apt install asterisk) แล้วลอง SIP ทุกกรณี
test/softphone/run.sh    # tools/sip_phone.py กับโค้ด SIP ของบอร์ด โทรทั้งสองทางแล้วตรวจว่าเสียงกลับมาครบ
```

`test/asterisk/run.sh` รันโค้ด SIP ชุดเดียวกับในบอร์ดกับ Asterisk 20 จริง ครอบคลุม:
ลงทะเบียนพร้อม digest auth, โทรไปเบอร์ echo แล้วเช็กว่าเสียง 1 kHz ที่ส่งไปกลับมา,
สายไม่ว่าง (486), ปลายสายวางเอง, สายเข้าพร้อม header auto-answer, ผู้โทรยกเลิกก่อนรับ และการปฏิเสธสาย

## สถานะและสิ่งที่ยังไม่ได้ทำ

- โค้ดนี้คอมไพล์ผ่านทั้งสองบอร์ด unit test ผ่าน และส่วน SIP ผ่านการทดสอบกับ Asterisk จริงแล้ว
  แต่ **ยังไม่ได้ทดสอบบนฮาร์ดแวร์จริง** โดยเฉพาะคุณภาพของ AEC บน CUBE ที่ต้องลองกับเครื่องจริงเท่านั้น
  ถ้าเจอปัญหา ให้ดู log ผ่าน `idf.py monitor` ก่อน
- SIP ใช้ UDP ในวง LAN เท่านั้น ยังไม่รองรับ TLS/SRTP, NAT, DTMF และ codec G.722
- ยังไม่มี Opus: ADPCM ใช้แบนด์วิดท์ 64 kbit/s ต่อผู้ฟังหนึ่งคน ซึ่งพอสำหรับ LAN
- UI เป็นภาษาอังกฤษ เพราะฟอนต์ในตัวของ LVGL ไม่มีอักษรไทย
- ไม่มีการเข้ารหัส: ใครอยู่ใน LAN เดียวกันก็ฟังได้

## เครดิต

ลำดับคำสั่ง init ของจอ EchoEar (`main/board/echoear_lcd_init.h`) และขา GPIO
นำมาจาก xiaozhi-esp32 (MIT License, Copyright (c) 2025 Shenzhen Xinzhi Future Technology Co., Ltd. and Project Contributors)
