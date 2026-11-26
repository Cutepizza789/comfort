#!/usr/bin/env python3
"""ComfortQuest helper: connect to a Quest over ADB (many methods), patch a game, reinstall it.

  python3 comfort.py connect [--ip 192.168.1.50[:port]] [--pair IP:PAIRPORT --code 123456] [--wireless]
  python3 comfort.py patch com.vendor.game      (connects first, then pulls/patches/signs/reinstalls)
  python3 comfort.py install-app [app-debug.apk]
  python3 comfort.py doctor

Needs: Python 3.8+, adb, and for `patch`: zipalign + apksigner (Android build-tools) + keytool (JDK).
"""
import argparse, json, os, re, shutil, socket, subprocess, sys, tempfile, time, zipfile
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
STATE = os.path.expanduser("~/.comfortquest_adb.json")
LIB = "lib/arm64-v8a/libopenxr_loader.so"
REAL = "lib/arm64-v8a/libopenxr_loader_real.so"
SERIAL = None


def log(m=""):
    print(m, flush=True)


def die(m):
    log("ERROR: " + m)
    sys.exit(1)


def find_tool(names, build_tools=False):
    for n in names:
        p = shutil.which(n)
        if p:
            return p
    roots = [os.environ.get("ANDROID_HOME"), os.environ.get("ANDROID_SDK_ROOT"), os.path.expanduser("~/Android/Sdk"),
             os.path.expanduser("~/Library/Android/sdk"), os.path.join(os.environ.get("LOCALAPPDATA", ""), "Android", "Sdk")]
    for r in filter(None, roots):
        if build_tools:
            bt = os.path.join(r, "build-tools")
            if os.path.isdir(bt):
                key = lambda v: [int(x) for x in re.findall(r"\d+", v)]
                for v in sorted(os.listdir(bt), key=key, reverse=True):
                    for n in names:
                        p = os.path.join(bt, v, n)
                        if os.path.exists(p):
                            return p
        else:
            for n in names:
                p = os.path.join(r, "platform-tools", n)
                if os.path.exists(p):
                    return p
    return None


ADB = os.environ.get("ADB") or find_tool(["adb", "adb.exe"])


def adb(*args, serial=True, timeout=120, env=None):
    cmd = [ADB]
    if serial and SERIAL:
        cmd += ["-s", SERIAL]
    cmd += list(args)
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=env)
        return p.returncode, ((p.stdout or "") + (p.stderr or "")).strip()
    except subprocess.TimeoutExpired:
        return 124, "timeout"


def load_state():
    try:
        return json.load(open(STATE))
    except Exception:
        return {}


def save_state(**kw):
    s = load_state(); s.update(kw)
    try:
        json.dump(s, open(STATE, "w"))
    except Exception:
        pass


# ---------------------------------------------------------------- connection methods
def devices():
    _, out = adb("devices", "-l", serial=False, timeout=30)
    res = []
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 2 and p[1] in ("device", "unauthorized", "offline", "authorizing", "connecting"):
            res.append((p[0], p[1]))
    return res


def is_ready(serial):
    rc, out = adb("-s", serial, "shell", "echo", "ok", serial=False, timeout=20)
    return rc == 0 and "ok" in out


def try_connect(addr):
    rc, out = adb("connect", addr, serial=False, timeout=20)
    low = out.lower()
    ok = "connected to" in low and "failed" not in low and "cannot" not in low
    if ok and is_ready(addr):
        return addr
    if "authenticate" in low or "unauthorized" in low:
        log("    -> headset has not authorized this PC yet (put the headset on, tap 'Always allow')")
    return None


def usb_ip(serial):
    _, out = adb("-s", serial, "shell", "ip", "route", serial=False, timeout=20)
    for line in out.splitlines():
        m = re.search(r"wlan\d.*src (\d+\.\d+\.\d+\.\d+)", line) or re.search(r"src (\d+\.\d+\.\d+\.\d+).*wlan\d", line)
        if m:
            return m.group(1)
    return None


def mdns():
    found = []
    for extra in ({}, {"ADB_MDNS_OPENSCREEN": "1"}):
        _, out = adb("mdns", "services", serial=False, env=dict(os.environ, **extra), timeout=25)
        for line in out.splitlines():
            m = re.search(r"(_adb[\w-]*\._tcp)\.?\s+([\d.]+):(\d+)", line)
            if m:
                found.append((m.group(1), m.group(2), int(m.group(3))))
        if found:
            break
    return found


def scan(ip, lo=30000, hi=50000, timeout=0.4):
    def probe(p):
        s = socket.socket(); s.settimeout(timeout)
        try:
            s.connect((ip, p)); return p
        except OSError:
            return None
        finally:
            s.close()
    ports = [5555] + list(range(lo, hi + 1))
    with ThreadPoolExecutor(300) as ex:
        return [p for p in ex.map(probe, ports) if p]


def pair(hostport, code):
    rc, out = adb("pair", hostport, code, serial=False, timeout=40)
    log("    " + out.replace("\n", " | "))
    return "success" in out.lower()


def connect_flow(a):
    global SERIAL
    if not ADB:
        die("adb not found. Install platform-tools or set the ADB environment variable to its path.")
    adb("start-server", serial=False)
    st = load_state()
    ip = (a.ip or os.environ.get("QUEST_IP") or st.get("ip") or "").strip()
    port = a.port
    if ":" in ip:
        ip, p = ip.rsplit(":", 1); port = port or int(p)

    def done(serial, how):
        global SERIAL
        SERIAL = serial
        h = serial.split(":")[0] if ":" in serial else None
        if h and re.match(r"\d+\.\d+\.\d+\.\d+$", h):
            save_state(ip=h, port=int(serial.split(":")[1]))
        log("CONNECTED via %s: %s" % (how, serial))
        return serial

    # 1: whatever adb already sees
    log("[1] devices adb already sees")
    devs = devices()
    for s, state in devs:
        log("    %s  %s" % (s, state))
    usb = [s for s, stt in devs if stt == "device" and ":" not in s and not s.startswith("adb-")]
    if a.wireless and usb:
        log("[1b] USB found, switching the headset to wireless (adb tcpip 5555)")
        wip = ip if (ip and not ip.startswith("127.")) else usb_ip(usb[0])
        if wip:
            adb("-s", usb[0], "tcpip", "5555", serial=False, timeout=30)
            time.sleep(2.5)
            s = try_connect("%s:5555" % wip)
            if s:
                return done(s, "USB -> tcpip 5555")
        log("    could not determine headset IP over USB; using USB instead")
    for s, stt in devs:
        if stt == "device" and is_ready(s):
            return done(s, "existing connection")
    if any(stt == "unauthorized" for _, stt in devs):
        log("    A device is 'unauthorized': put the headset on and accept 'Allow USB debugging' (tick Always allow).")
        adb("reconnect", serial=False, timeout=20)

    def run_methods():
        # 2: address given on the command line / saved
        if ip and port:
            log("[2] %s:%s (given/saved address)" % (ip, port))
            s = try_connect("%s:%s" % (ip, port))
            if s:
                return done(s, "given address")
        # 3: classic tcpip port
        if ip:
            log("[3] %s:5555 (adb tcpip port, only works if you ran 'adb tcpip 5555' over USB since last reboot)" % ip)
            s = try_connect("%s:5555" % ip)
            if s:
                return done(s, "port 5555")
        # 4: mDNS (wireless debugging announces itself)
        log("[4] mDNS discovery")
        svc = mdns()
        for kind, h, p in svc:
            log("    found %s at %s:%s" % (kind, h, p))
        for kind, h, p in svc:
            if "connect" in kind:
                s = try_connect("%s:%s" % (h, p))
                if s:
                    return done(s, "mDNS")
        if any("pairing" in k for k, _, _ in svc):
            log("    a PAIRING service is visible: finish pairing (see below), then re-run.")
        # 5: port scan of the headset's IP
        if ip:
            log("[5] scanning %s for open ports 30000-50000 (+5555), about 20-30 s" % ip)
            opens = scan(ip)
            log("    open ports: %s" % (opens or "none"))
            for p in opens:
                s = try_connect("%s:%d" % (ip, p))
                if s:
                    return done(s, "port scan")
        return None

    if a.pair and a.code:
        log("[0] pairing with %s" % a.pair)
        if not pair(a.pair, a.code):
            log("    pairing failed (code is only valid while the pairing dialog is open on the headset)")
    r = run_methods()
    if r:
        return r

    if sys.stdin.isatty():
        log("\nNothing worked yet. Enter details to try pairing + the methods again (blank = skip).")
        if not ip:
            ip = input("Headset IP (Settings > Wi-Fi > your network): ").strip()
        hp = input("Pairing IP:PORT shown on the 'Pair device with pairing code' dialog: ").strip()
        if hp:
            code = input("Pairing code: ").strip()
            pair(hp, code)
        r = run_methods()
        if r:
            return r

    log("""
Could not connect. What this means:
 - Port 5555 only exists after 'adb tcpip 5555' is run once over USB, and it is gone again after a reboot.
 - Wireless debugging uses a RANDOM port and needs one-time pairing ('adb pair ip:pairport code'); the pairing port
   and code only exist while the pairing dialog is open on the headset.
 - Developer mode must be on, and the headset must be awake (wireless debugging stops when it sleeps).
 - Quickest fix: plug in USB once and run:  python3 comfort.py connect --wireless
 - Or:  python3 comfort.py connect --ip <headset ip> --pair <ip:pairport> --code <code>""")
    sys.exit(2)


# ---------------------------------------------------------------- patching
def patch_zip(src, dst, proxy):
    found = False
    with zipfile.ZipFile(src) as zin, zipfile.ZipFile(dst, "w") as zout:
        for item in zin.infolist():
            n = item.filename
            if n.startswith("META-INF/") and n.rsplit(".", 1)[-1].upper() in ("SF", "RSA", "DSA", "EC", "MF"):
                continue
            if n == LIB:
                found = True
                for name, data in ((REAL, zin.read(n)), (LIB, proxy)):
                    zi = zipfile.ZipInfo(name, item.date_time); zi.compress_type = zipfile.ZIP_STORED
                    zout.writestr(zi, data)
                continue
            zi = zipfile.ZipInfo(n, item.date_time)
            zi.compress_type = item.compress_type; zi.external_attr = item.external_attr
            with zin.open(item) as fi, zout.open(zi, "w", force_zip64=item.file_size > 2**31 - 1) as fo:
                shutil.copyfileobj(fi, fo, 1 << 20)
    return found


def has_loader(apk):
    with zipfile.ZipFile(apk) as z:
        return LIB in z.namelist()


def run(*cmd):
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        die("%s failed:\n%s%s" % (cmd[0], p.stdout, p.stderr))


def patch_cmd(a):
    connect_flow(a)
    proxy_path = a.proxy or os.path.join(HERE, "proxy", "out", "libopenxr_loader.so")
    if not os.path.exists(proxy_path):
        die("Proxy library missing: %s\nBuild it first:  export ANDROID_NDK_HOME=...; proxy/build.sh" % proxy_path)
    proxy = open(proxy_path, "rb").read()
    zipalign = find_tool(["zipalign", "zipalign.exe"], True)
    apksigner = find_tool(["apksigner", "apksigner.bat"], True)
    keytool = find_tool(["keytool", "keytool.exe"]) or (os.path.join(os.environ["JAVA_HOME"], "bin", "keytool") if os.environ.get("JAVA_HOME") else None)
    if not (zipalign and apksigner and keytool):
        die("Need zipalign + apksigner (Android SDK build-tools) and keytool (JDK) on PATH or in ANDROID_HOME.")

    pkg = a.package
    rc, out = adb("shell", "pm", "path", pkg)
    paths = [l.split(":", 1)[1].strip() for l in out.splitlines() if l.startswith("package:")]
    if not paths:
        _, lst = adb("shell", "pm", "list", "packages", "-3")
        die("Package %s not installed. Third-party packages:\n%s" % (pkg, lst))
    work = tempfile.mkdtemp(prefix="comfortquest_")
    log("Pulling %d APK file(s) from the headset..." % len(paths))
    local = []
    for i, p in enumerate(paths):
        dst = os.path.join(work, "in_%d_%s" % (i, os.path.basename(p)))
        rc, out = adb("pull", p, dst, timeout=3600)
        if rc != 0:
            die("pull failed: " + out)
        local.append(dst)
    target = next((l for l in local if has_loader(l)), None)
    if not target:
        die("No bundled OpenXR loader (lib/arm64-v8a/libopenxr_loader.so) in any APK of %s. "
            "This game probably uses the old Oculus VrApi and can't be supported." % pkg)
    ks = os.path.expanduser("~/.comfortquest.keystore")
    if not os.path.exists(ks):
        run(keytool, "-genkeypair", "-keystore", ks, "-storepass", "android", "-keypass", "android", "-alias", "comfort",
            "-keyalg", "RSA", "-keysize", "2048", "-validity", "10000", "-dname", "CN=ComfortQuest")
    outs = []
    for l in local:
        base = os.path.join(work, "out_" + os.path.basename(l))
        staged = base + ".stage"
        if l == target:
            log("Patching %s ..." % os.path.basename(l))
            patch_zip(l, staged, proxy)
        else:
            shutil.copy(l, staged)
        aligned = base + ".aligned"
        run(zipalign, "-f", "-p", "4", staged, aligned)
        run(apksigner, "sign", "--ks", ks, "--ks-pass", "pass:android", "--out", base, aligned)
        outs.append(base)
    log("Signed %d file(s)." % len(outs))

    data_dir = "/sdcard/Android/data/%s" % pkg
    backup = os.path.join(work, "data_backup")
    if not a.no_backup:
        log("Backing up %s (so saves survive the reinstall)..." % data_dir)
        rc, out = adb("pull", data_dir, backup, timeout=3600)
        log("    " + ("done" if rc == 0 else "nothing to back up / failed: " + out[:200]))
    log("Uninstalling original (required: the signature changes)...")
    rc, out = adb("uninstall", pkg)
    log("    " + out)
    log("Installing patched build...")
    if len(outs) == 1:
        rc, out = adb("install", "-r", "-g", outs[0], timeout=3600)
    else:
        rc, out = adb("install-multiple", "-r", "-g", *outs, timeout=3600)
    log("    " + out)
    if "Success" not in out:
        die("Install failed. Patched files are in %s" % work)
    if os.path.isdir(backup):
        log("Restoring app data...")
        adb("shell", "mkdir", "-p", data_dir)
        src = os.path.join(backup, os.path.basename(data_dir))
        adb("push", src + os.sep + ".", data_dir + "/", timeout=3600)
    log("\nDone. Open Comfort Quest once (it keeps sending settings), then launch the game. Debug: adb logcat -s ComfortLayer")


def install_app(a):
    connect_flow(a)
    apk = a.apk or os.path.join(HERE, "app", "build", "outputs", "apk", "debug", "app-debug.apk")
    if not os.path.exists(apk):
        die("APK not found: %s (build the app in Android Studio first)" % apk)
    rc, out = adb("install", "-r", apk, timeout=600)
    log(out)


def doctor(a):
    log("adb: %s" % ADB)
    if ADB:
        log(adb("version", serial=False)[1])
        log(adb("devices", "-l", serial=False)[1])
    for n, names, bt in (("zipalign", ["zipalign", "zipalign.exe"], True), ("apksigner", ["apksigner", "apksigner.bat"], True),
                         ("keytool", ["keytool", "keytool.exe"], False)):
        log("%s: %s" % (n, find_tool(names, bt) if bt else shutil.which(names[0])))
    log("proxy lib: %s" % os.path.exists(os.path.join(HERE, "proxy", "out", "libopenxr_loader.so")))


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)

    def common(p):
        p.add_argument("--ip"); p.add_argument("--port", type=int)
        p.add_argument("--pair", help="pairing IP:PORT from the headset pairing dialog"); p.add_argument("--code")
        p.add_argument("--wireless", action="store_true", help="if USB is attached, switch to wireless (adb tcpip 5555)")
    c = sub.add_parser("connect"); common(c)
    p = sub.add_parser("patch"); common(p); p.add_argument("package"); p.add_argument("--proxy"); p.add_argument("--no-backup", action="store_true")
    i = sub.add_parser("install-app"); common(i); i.add_argument("apk", nargs="?")
    sub.add_parser("doctor")
    a = ap.parse_args()
    {"connect": connect_flow, "patch": patch_cmd, "install-app": install_app, "doctor": doctor}[a.cmd](a)


if __name__ == "__main__":
    main()
