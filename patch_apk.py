#!/usr/bin/env python3
"""Inject ComfortLayer into a Quest game APK.
usage: python3 patch_apk.py game.apk game_comfort.apk [proxy/out/libopenxr_loader.so]
Needs zipalign, apksigner and keytool (Android build-tools + JDK) on PATH."""
import os, subprocess, sys, tempfile, zipfile

LIB = "lib/arm64-v8a/libopenxr_loader.so"
REAL = "lib/arm64-v8a/libopenxr_loader_real.so"

def run(*a):
    subprocess.check_call(a)

def main():
    src, out = sys.argv[1], sys.argv[2]
    proxy = sys.argv[3] if len(sys.argv) > 3 else os.path.join(os.path.dirname(__file__), "proxy/out/libopenxr_loader.so")
    proxy_data = open(proxy, "rb").read()
    tmp = tempfile.mkdtemp()
    stage = os.path.join(tmp, "stage.apk")
    found = False
    with zipfile.ZipFile(src) as zin, zipfile.ZipFile(stage, "w") as zout:
        for item in zin.infolist():
            n = item.filename
            if n.startswith("META-INF/") and n.rsplit(".", 1)[-1].upper() in ("SF", "RSA", "DSA", "EC", "MF"):
                continue
            data = zin.read(n)
            if n == LIB:
                found = True
                zi = zipfile.ZipInfo(REAL, item.date_time); zi.compress_type = zipfile.ZIP_STORED
                zout.writestr(zi, data)
                zi = zipfile.ZipInfo(LIB, item.date_time); zi.compress_type = zipfile.ZIP_STORED
                zout.writestr(zi, proxy_data)
            else:
                zout.writestr(item, data)
    if not found:
        sys.exit("No bundled OpenXR loader in this APK (old Oculus VrApi game?). Not supported.")
    aligned = os.path.join(tmp, "aligned.apk")
    run("zipalign", "-f", "-p", "4", stage, aligned)
    ks = os.path.expanduser("~/.comfortquest.keystore")
    if not os.path.exists(ks):
        run("keytool", "-genkeypair", "-keystore", ks, "-storepass", "android", "-keypass", "android",
            "-alias", "comfort", "-keyalg", "RSA", "-keysize", "2048", "-validity", "10000", "-dname", "CN=ComfortQuest")
    run("apksigner", "sign", "--ks", ks, "--ks-pass", "pass:android", "--out", out, aligned)
    print("Done:", out)
    print("Install: adb uninstall <package>  (signature differs, this wipes local game data)")
    print("         adb install", out)

main()
