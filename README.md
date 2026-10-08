# steamlink-ps5

A native Steam Remote Play ("Steam Link") client for a jailbroken PS5, built as a
homebrew title. The goal is the TV Steam Link experience: the console finds the PC
running Steam, pairs with a PIN, and streams Big Picture Mode with DualSense
controllers, hardware video decoding and low latency.

**Status: planning.** Read [PLAN.md](PLAN.md) for the research, the chosen
architecture and the milestones. Nothing builds yet.

## Planned workflow

```bash
git clone --recursive <this repo>
make                          # dist/<TITLE_ID>/ (eboot.bin, sce_sys/, sce_module/)
PS5_HOST=192.168.x.x make deploy   # upload over FTP (ftpsrv, port 2121)
```

## Licence

GPL-3.0-or-later for this repository's own code. Protocol code comes from
[ihslib](https://github.com/mariotaku/ihslib) (LGPL-3.0); PS5 platform code is
adapted from [ProsperoLight](https://github.com/blackbearreloaded/ProsperoLight)
and [ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate)
(GPL-3.0-or-later). No Sony SDK, firmware, keys or Valve code are included.
Unofficial: not affiliated with Valve or Sony.
