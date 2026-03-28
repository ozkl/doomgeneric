## Compatible Hardware

Based on the MediaTek MT7628 SoC, this port should work on any router running OpenWrt with this chipset. The following models are expected to work.

**Tip:** To check your router's SoC, run via SSH:
```bash
cat /proc/cpuinfo | grep system type
```
If it shows `MT7628`, `MT7688`, or `MT7620`, give it a try!

### Confirmed Working
| Model | Status |
|-------|--------|
| **TP-Link TL-WR840N v6** | ✅ **It Runs DOOM!** |

### Help Expand This List

**Did you successfully run DOOM on a router that's not listed?** Your contribution would be greatly appreciated!

If your device works, please:
- **Open an Issue** on GitHub with your device model and any relevant details
- **Or submit a Pull Request** to update this table

### Expected to work

The current list of MT7628 based routers with OpenWrt support is available in the official [Table of Hardware](https://toh.openwrt.org/?view=normal) (use the CPU filter to for precise search).

The DOOM port is expected to work out of the box on the following models:

| Brand | Model |
| :--- | :--- |
| 7Links | WLR-1230 |
| 7Links | WLR-1240 |
| ALFA Network | AWUSFREE1 |
| ASUS | RT-AC1200 v2 |
| ASUS | RT-AC1200 v1 |
| ASUS | RT-N10P v3 |
| ASUS | RT-N11P B1 |
| ASUS | RT-N12 VP B1 |
| ASUS | RT-N12+ B1 |
| AUKEY | WF-R8 |
| Buffalo | WCR-1166DS |
| COMFAST | CF-WR617AC |
| COMFAST | CF-WR758AC v1 |
| COMFAST | CF-WR758AC v2 |
| Cudy | LT400E v1 |
| Cudy | M1200 v1 |
| Cudy | RE1200 Outdoor v1 |
| Cudy | TR1200 v1 |
| Cudy | WR1000 v1 |
| D-Link | DAP-1325 A1 |
| D-Team | PandoraBox PBR-D1 |
| DuZun | DM06 |
| ELECOM | WRC-1167FS |
| GL.iNet | GL-MT300N (Mango) v2 |
| GL.iNet | microuter-N300 |
| GL.iNet | VIXMINI |
| HAK5 | WiFi Pineapple Mark 7 |
| Hi-Link | HLK-7628N |
| HiWiFi/Gee | HC5611 |
| HiWiFi/Gee | HC5661A |
| HiWiFi/Gee | HC5761A |
| HiWiFi/Gee | HC5861B |
| Hongdian | H7920 v40 |
| Hongdian | H8850 v20 |
| ipTIME | A3 |
| ipTIME | A604M |
| Joowin | JW-WR758AC v2 |
| Jotale | JS7628 32MB |
| Jotale | JS7628 16MB |
| Jotale | JS7628 8MB |
| Keenetic | KN-1110 (Start) |
| Keenetic | KN-1111 (Start/Starter) |
| Keenetic | KN-1112 (Start/Starter) |
| Keenetic | KN-1210 (4G) |
| Keenetic | KN-1211 (4G) |
| Keenetic | KN-1212 (4G) |
| Keenetic | KN-1221 (Launcher) |
| Keenetic | KN-1613 (Explorer) |
| Keenetic | KN-1711 (Extra) |
| Keenetic | KN-1713 (Carrier) |
| Keenetic | KN-3211 (Buddy 4) |
| KROKS | Rt-Brd DS e (KNdRt31R4) |
| KROKS | Rt-Brd e (KNdRt31R34) |
| KROKS | Rt-Brd RSIM DS eQ-EP (KNdRt31R19) |
| KROKS | Rt-Brd RSIM e (KNdRt31R33) |
| KROKS | Rt-Cse m6 (KNdRt31R27) |
| KROKS | Rt-Cse SIM Injector DS (KNdRt31R16) |
| Linksys | E2500 v4 |
| Linksys | E5300 |
| Linksys | E5350 |
| Linksys | E5400 |
| Maginon | WLR-755 |
| Mercury | MAC1200R v2 |
| MERCUSYS | MB130-4G V1 |
| MINEW | G1-C |
| Motorola | MWR03 (C1) |
| NETGEAR | R6020 |
| NETGEAR | R6080 |
| NETGEAR | R6120 |
| OrayBox | X1 |
| Qding | QC202 |
| Rakwireless | RAK633 |
| RavPower | RP-WD009 v1.0 |
| SilverCrest | SWV 733 A2 |
| SilverCrest | SWV 733 B1 |
| Skylab | SKW92A EVB E16 |
| Teltonika | RUT200 |
| Teltonika | RUT241 |
| Teltonika | RUT901 |
| Teltonika | RUT906 |
| Teltonika | RUT951 |
| Teltonika | RUT956 |
| Teltonika | RUT976 |
| TOTOLINK | A3 |
| TOTOLINK | LR1200 |
| TP-Link | Archer A5 v6 (CA/EU/RU) |
| TP-Link | Archer A5 v5 |
| TP-Link | Archer C20 v5 |
| TP-Link | Archer C20 AC750 v4 |
| TP-Link | Archer C50 v4 |
| TP-Link | Archer C50 v3 |
| TP-Link | Archer C50 v6 |
| TP-Link | Archer C50 v5 |
| TP-Link | Archer MR200 v6 |
| TP-Link | Archer MR200 v5 |
| TP-Link | RE200 v2 |
| TP-Link | RE200 v3 |
| TP-Link | RE200 v4 |
| TP-Link | RE205 v3 |
| TP-Link | RE220 v2 |
| TP-Link | RE305 v1 |
| TP-Link | RE305 v3 |
| TP-Link | RE365 v1 |
| TP-Link | TL-MR3020 v3 |
| TP-Link | TL-MR3420 v5 |
| TP-Link | TL-MR6400 v4 |
| TP-Link | TL-MR6400 v5 |
| TP-Link | TL-WA801ND v5 |
| TP-Link | TL-WR802N v4 |
| TP-Link | TL-WR840N v4 |
| TP-Link | TL-WR840N v6 |
| TP-Link | TL-WR840N v5 |
| TP-Link | TL-WR841N v13 |
| TP-Link | TL-WR841N v14 |
| TP-Link | TL-WR842N v5 |
| TP-Link | TL-WR849N v4.0 (BR) |
| TP-Link | TL-WR850N v2 |
| TP-Link | TL-WR850N v1 |
| TP-Link | TL-WR902AC v4 |
| TP-Link | TL-WR902AC v3 |
| UniElec | U7628-01 |
| VoCore | VoCore2 |
| WAVLINK | WL-WN531A3 (QUANTUM D4) |
| WAVLINK | WL-WN532N2 Rev. F |
| WAVLINK | WL-WN570HA2 |
| WAVLINK | WL-WN575A3B |
| WAVLINK | WL-WN576A2 |
| WAVLINK | WL-WN577A2 |
| WAVLINK | WL-WN578A2 |
| Winstars | WS-WN532A3 |
| Wodesys | WD-R1208U |
| WRTnode | WRTnode 2P |
| Xiaomi | Mi Router 4A (MIR4A) 100M |
| Xiaomi | Mi Router 4A (R4AC) 100M (International) |
| Xiaomi | Mi Router 4A (R4AC) 100M (International) V2 |
| Xiaomi | Mi Router 4C |
| Xiaomi | Mi WiFi Range Extender AC1200 RA75 |
| Xiaomi | MiWiFi 3A |
| Xiaomi | MiWifi 3C |
| Xiaomi | MiWiFi Nano |
| YunCore | CPE200 |
| YunCore | M300 |
| ZBT | WE1226 |
| ZBT | ZBT-WE2426-B |
| ZyXEL | Keenetic 4G III rev. B |
| ZyXEL | Keenetic Extra II |
| ZyXEL | Keenetic Start II rev. A |