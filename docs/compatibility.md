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

The current list of routers with OpenWrt support is available in the official [Table of Hardware](https://openwrt.org/toh/start). Use the CPU filter to for precise search.

| Brand | Model |
| :--- | :--- |
| **TP-Link** | Archer C20 v4 / v5 |
| **TP-Link** | Archer C50 v3 / v4 |
| **TP-Link** | TL-WR841N / TL-WR841ND (v13 и выше) |
| **TP-Link** | TL-WR842N (v3 / v5) |
| **TP-Link** | TL-WR840N (v4 / v5 / v6.20) |
| **TP-Link** | TL-WR940N (v5 / v6) |
| **Xiaomi** | Mi Router 4A (100M Edition) |
| **Xiaomi** | Mi Router 4C |
| **Xiaomi** | Mi WiFi Router 3C |
| **GL.iNet** | GL-MT300N-V2 (Mango) |
| **D-Link** | DIR-615 (ревизии T1, X1, Z1) |
| **D-Link** | DIR-806U / DIR-822 |
| **Mercusys** | MW305R (v2 и выше) |
| **Mercusys** | AC12 |
| **Totolink** | N300RT / N302R |
| **Tenda** | AC6 (v1 / v2) |
| **HooToo** | HT-TM05 |
| **Onion** | Omega2 / Omega2+ |
