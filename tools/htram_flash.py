"""Flashes htram-fw onto a Honeywell HTRAM-V1-W from start to finish.

    python tools/htram_flash.py                  # asks for the COM port
    python tools/htram_flash.py COM19
    python tools/htram_flash.py COM19 --keep-settings   # update, keep Wi-Fi and pairing

Needs: pip install esptool   (brings pyserial)
Wiring: adapter RX -> module pin 35 (TXD0), adapter TX -> pin 34 (RXD0), GND,
and IO0 (pin 25) either to the C3 bridge's GPIO5 or to GND by hand.

What it does, and why it is careful:

1. Downloads the latest release image and checks it against SHA256SUMS.
2. Puts the ESP32 into download mode: the C3 bridge holds IO0 low by itself,
   any other adapter needs IO0 grounded. Then you power-cycle the monitor.
3. Backs up the stock flash (4 MB, ~6 min), verified by esptool's MD5.
4. Writes the image.
5. Releases IO0, has you power-cycle once more and checks the boot log.
6. Prints this unit's setup network, address, HomeKit code and OTA password.

The trap it is built around: download mode only exists from a power-up with
IO0 low, and on the HTRAM the main board (GD32) controls the ESP32's power
and cuts it after a few minutes in download mode. The chip then answers
nothing ("No serial data received"), which looks like a wiring fault but is
not. So before every step the script checks that the chip answers, and if it
does not, asks for a power-cycle and waits for the boot ROM to say it is in
download mode again. Speed is fixed at 115200 and esptool never toggles
DTR/RTS: the C3 bridge runs at a fixed baud and has no reset lines.
"""
import argparse, hashlib, io, json, os, re, subprocess, sys, tempfile, time, urllib.request, zipfile

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    sys.exit("Missing pyserial. Run:  pip install esptool")

REPO = "YoYoZ/htram-fw"
BAUD = 115200
FLASH_SIZE = 0x400000


def say(msg=""):
    print(msg, flush=True)


def step(msg):
    say(f"\n== {msg}")


# --------------------------------------------------------------------------
# esptool, called as a subprocess so any installed version works
# --------------------------------------------------------------------------
def esptool_major():
    try:
        out = subprocess.run([sys.executable, "-m", "esptool", "version"],
                             capture_output=True, text=True, timeout=60).stdout
    except Exception:
        out = ""
    m = re.search(r"(\d+)\.\d+", out)
    if not m:
        sys.exit("esptool is not installed for this Python. Run:  pip install esptool")
    return int(m.group(1))


V5 = esptool_major() >= 5


def cmd(name):  # esptool 5 spells commands and options with hyphens
    return name.replace("_", "-") if V5 else name


def esptool(port, *args, quiet=False):
    argv = [sys.executable, "-m", "esptool", "--chip", "esp32", "--port", port,
            "--baud", str(BAUD), "--before", cmd("no_reset"), "--after", cmd("no_reset"),
            "--connect-attempts", "3", *args]
    p = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)
    out = []
    for line in p.stdout:
        out.append(line)
        if not quiet and not re.match(r"\s*(Writing at|Read \d+ bytes at|\d+ \()", line):
            print("   " + line.rstrip(), flush=True)
    p.wait()
    return p.returncode, "".join(out)


def chip_answers(port):
    """MAC address if a chip in download mode answers, else None."""
    rc, out = esptool(port, cmd("read_mac"), quiet=True)
    if "Wrong --chip" in out or "This chip is" in out:
        sys.exit("The chip that answered is not an ESP32, so this is not an HTRAM:\n" + out)
    if rc != 0:
        return None
    if "ESP32" not in out or "ESP32-S" in out or "ESP32-C" in out:
        sys.exit("The chip that answered is not a classic ESP32, so this is not an HTRAM:\n" + out)
    m = re.search(r"MAC:\s*([0-9a-f:]{17})", out, re.I)
    return m.group(1).lower() if m else None


# --------------------------------------------------------------------------
# C3 bridge and boot mode
# --------------------------------------------------------------------------
def open_port(port):
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, BAUD, 0.3
    s.dtr = s.rts = False  # a C3's USB serial resets on some DTR/RTS states
    try:
        s.open()
    except serial.SerialException as e:
        if "denied" in str(e).lower() or "busy" in str(e).lower():
            sys.exit(f"{port} is in use by another program (a serial monitor, esptool, another "
                     "Claude session...). Close it and run again.")
        sys.exit(f"Cannot open {port}: {e}")
    return s


def bridge(port, word):
    """Sends a control word to the C3 bridge. True if it is one."""
    with open_port(port) as s:
        s.reset_input_buffer()
        s.write(f"<<HTRAM-{word}>>".encode())
        s.flush()
        time.sleep(0.6)
        reply = s.read(s.in_waiting or 1).decode("ascii", "replace")
    return "[bridge]" in reply


def hold_io0(port, have_bridge):
    if have_bridge:
        bridge(port, "BOOT")
    else:
        say("   Connect IO0 (module pin 25) to GND and keep it there.")


def release_io0(port, have_bridge):
    if have_bridge:
        bridge(port, "RUN")
    else:
        say("   Disconnect IO0 from GND now.")
        input("   Press Enter when done. ")


def wait_for_download_mode(port, why):
    """Has the user power-cycle the monitor and waits for the boot ROM."""
    say(f"   {why}")
    say("   >>> Power-cycle the monitor now: off, then hold its button ~2 s. <<<")
    with open_port(port) as s:
        buf, end = b"", time.time() + 300
        while time.time() < end:
            buf += s.read(4096)
            if b"waiting for download" in buf:
                say("   Download mode confirmed.")
                return
            if b"SPI_FAST_FLASH_BOOT" in buf:
                sys.exit("   It booted normally: IO0 was not low. Check the IO0 wire and run again.")
    sys.exit("   Nothing heard in 5 minutes. Check TX/RX (try swapping them) and GND, then run again.")


def ensure_download_mode(port, why):
    """Returns the MAC once the chip answers, power-cycling as often as needed."""
    mac = chip_answers(port)
    while not mac:
        wait_for_download_mode(port, why)
        mac = chip_answers(port)
        if not mac:
            why = "The ROM said it was in download mode but did not answer; once more."
    return mac


# --------------------------------------------------------------------------
# Release image
# --------------------------------------------------------------------------
def fetch(url):
    req = urllib.request.Request(url, headers={"User-Agent": "htram-flash"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()


def latest_release(keep_settings):
    rel = json.loads(fetch(f"https://api.github.com/repos/{REPO}/releases/latest"))
    assets = {a["name"]: a["browser_download_url"] for a in rel["assets"]}
    suffix = "-parts.zip" if keep_settings else "-full.bin"
    name = next((n for n in assets if n.startswith("htram-fw-") and n.endswith(suffix)), None)
    if not name or "SHA256SUMS" not in assets:
        sys.exit(f"Release {rel['tag_name']} has no {suffix} image.")
    data = fetch(assets[name])
    sums = dict(reversed(l.split(None, 1)) for l in fetch(assets["SHA256SUMS"]).decode().splitlines() if l.strip())
    want = sums.get(name) or sums.get("*" + name)
    if hashlib.sha256(data).hexdigest() != want:
        sys.exit(f"{name}: checksum mismatch, refusing to flash.")
    return rel["tag_name"], name, data


def local_image(path):
    """A -full.bin or -parts.zip given on the command line."""
    name = os.path.basename(path)
    if not (name.endswith("-full.bin") or name.endswith("-parts.zip")):
        sys.exit("--image takes a release's htram-fw-X.Y.Z-full.bin or -parts.zip.")
    return "local file", name, open(path, "rb").read()


def write_args(name, data, workdir):
    """esptool arguments that write the image."""
    if name.endswith("-full.bin"):
        path = os.path.join(workdir, name)
        open(path, "wb").write(data)
        return ["0x0", path]
    z = zipfile.ZipFile(io.BytesIO(data))
    out = []
    for off, part in (("0x1000", "bootloader.bin"), ("0x8000", "partitions.bin"),
                      ("0xe000", "boot_app0.bin"), ("0x10000", "firmware.bin")):
        member = next(m for m in z.namelist() if m.endswith("/" + part))
        path = os.path.join(workdir, part)
        open(path, "wb").write(z.read(member))
        out += [off, path]
    return out


# --------------------------------------------------------------------------
# Per-unit values, derived from the MAC exactly as the firmware does
# --------------------------------------------------------------------------
def unit_values(mac):
    M = (1 << 64) - 1
    b = bytes.fromhex(mac.replace(":", ""))
    efuse = sum(x << (8 * i) for i, x in enumerate(b))  # ESP.getEfuseMac()
    h = efuse ^ 0x9E3779B97F4A7C15
    while True:
        h ^= h >> 33
        h = (h * 0xFF51AFD7ED558CCD) & M
        h ^= h >> 33
        pin = h % 1000000
        if pin:
            break
    code = f"{pin:06d}00"
    o = (efuse * 0x9E3779B97F4A7C15) & M
    o ^= o >> 29
    return {"code": f"{code[:3]}-{code[3:5]}-{code[5:]}", "pin": f"{pin:06d}",
            "ota": f"htram-{o & 0xffffffff:08x}", "host": f"htram-{b[4]:02x}{b[5]:02x}"}


# --------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description="Flash htram-fw onto an HTRAM-V1-W.")
    ap.add_argument("port", nargs="?", help="serial port, e.g. COM19 or /dev/ttyUSB0")
    ap.add_argument("--keep-settings", action="store_true",
                    help="update only: keep Wi-Fi and Apple Home pairing (default erases them, for a stock unit)")
    ap.add_argument("--no-backup", action="store_true", help="skip backing up the stock flash")
    ap.add_argument("--out", default=".", help="where to save the backup (default: current folder)")
    ap.add_argument("--image", help="flash this -full.bin or -parts.zip instead of the latest release")
    a = ap.parse_args()

    port = a.port
    if not port:
        ports = [p.device for p in serial.tools.list_ports.comports()]
        say("Serial ports: " + (", ".join(ports) or "none found"))
        port = input("COM port of your adapter: ").strip()
        if not port:
            sys.exit("No port given.")

    if a.image:
        tag, name, image = local_image(a.image)
        if name.endswith("-full.bin") == a.keep_settings:
            sys.exit("--keep-settings needs a -parts.zip; a -full.bin erases settings.")
        step(f"Image: {name}, {len(image)} bytes")
    else:
        step("Fetching the latest release")
        tag, name, image = latest_release(a.keep_settings)
        say(f"   {tag}: {name}, {len(image)} bytes, checksum OK")

    step("Download mode")
    have_bridge = bridge(port, "STATUS")
    say("   C3 bridge found: it holds IO0 low for you." if have_bridge else
        "   Plain adapter (no C3 bridge).")
    hold_io0(port, have_bridge)
    mac = ensure_download_mode(port, "Starting the ESP32 in download mode.")
    u = unit_values(mac)
    say(f"   ESP32 answers, MAC {mac}")

    if not a.no_backup:
        step("Backing up the stock flash (about 6 minutes)")
        path = os.path.join(a.out, f"htram-backup-{mac.replace(':', '')}.bin")
        if os.path.exists(path) and os.path.getsize(path) == FLASH_SIZE:
            say(f"   {path} already exists, keeping it.")
        else:
            rc, out = esptool(port, cmd("read_flash"), "0", hex(FLASH_SIZE), path)
            if rc != 0 or os.path.getsize(path) != FLASH_SIZE:
                sys.exit("   Backup failed; nothing was written. Run again.")
            say(f"   Saved {path} (MD5 checked by esptool). Keep it: it is the only way back.")

    step(f"Writing {name}")
    # The GD32 may have cut the ESP32's power during the backup.
    mac2 = ensure_download_mode(port, "The ESP32 stopped answering after the backup (the main board cut "
                                      "its power); one more power-cycle.")
    if mac2 != mac:
        sys.exit(f"   A different chip answered ({mac2}, was {mac}). Stopping.")
    with tempfile.TemporaryDirectory() as tmp:
        rc, out = esptool(port, cmd("write_flash"), "-z", *write_args(name, image, tmp))
    if rc != 0 or "Hash of data verified" not in out:
        sys.exit("   Writing failed. The unit is safe to retry: run the script again "
                 "(download mode lives in mask ROM, it cannot be lost).")
    say("   Written and verified.")

    step("First boot")
    release_io0(port, have_bridge)
    say("   >>> Power-cycle the monitor once more. <<<")
    with open_port(port) as s:
        buf, end = b"", time.time() + 120
        while time.time() < end and b"SPI_FAST_FLASH_BOOT" not in buf:
            buf += s.read(4096)
        if b"waiting for download" in buf:
            sys.exit("   It came up in download mode again: IO0 is still low.")
        if b"SPI_FAST_FLASH_BOOT" not in buf:
            say("   No boot log seen (fine if your adapter misses it); check for the Wi-Fi network below.")
        else:
            say("   It boots from flash.")

    step("Done")
    if a.keep_settings:
        say(f"   It rejoins your Wi-Fi as http://{u['host']}.local and keeps its Apple Home pairing.")
    else:
        say("   On your phone, join the Wi-Fi network  HTRAM-Setup  (password  htram-setup);")
        say("   the setup page opens by itself (or http://192.168.4.1). Pick your network there.")
        say(f"   Afterwards:        http://{u['host']}.local")
    say(f"   Apple Home code:   {u['code']}   (the screen can show {u['pin']})")
    say(f"   Name in Home app:  HTRAM {u['host'][-4:]}")
    say(f"   OTA password:      {u['ota']}   (until you set an admin password)")
    say("   You can unsolder the wires: later updates go over Wi-Fi.")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit("\nStopped. Nothing is half-written unless 'Writing' had started; "
                 "if so, just run the script again.")
