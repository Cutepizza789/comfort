# ComfortQuest

One-time setup
1. Build the proxy:  `export ANDROID_NDK_HOME=...; proxy/build.sh`
2. Build the settings app in Android Studio (opens this folder).
3. `python3 comfort.py doctor` shows what is missing (adb, zipalign, apksigner, keytool).

Use
- `python3 comfort.py connect`  tries, in order: devices adb already sees, USB (+ `--wireless` switches to wifi),
  a given/saved ip:port, ip:5555, optional pairing (`--pair IP:PAIRPORT --code 123456`), mDNS discovery,
  and a port scan of the headset IP. It prints why each step failed and remembers the last good address.
- `python3 comfort.py install-app`  installs the settings app.
- `python3 comfort.py patch com.vendor.game`  connects, pulls the game (including split APKs) from the headset,
  swaps in the proxy, signs, backs up /sdcard/Android/data/<pkg>, reinstalls and restores the data.
- Then open Comfort Quest once and launch the game. Logs: `adb logcat -s ComfortLayer`.

Why 5555 fails: it only exists after `adb tcpip 5555` over USB and disappears on reboot. Wireless debugging uses a random
port and needs one-time pairing.
(`patch_apk.py` is the old manual patcher for a game APK you already have on the PC.)
