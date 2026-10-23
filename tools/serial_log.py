"""Log the C3 sniffer's USB output to a file as it arrives, reopening the port if it drops."""
import os, serial, sys, time

port, seconds, out = sys.argv[1], float(sys.argv[2]), sys.argv[3]
end = time.time() + seconds
with open(out, "ab") as f:
    f.write(f"\n==== capture started {time.strftime('%Y-%m-%d %H:%M:%S')} ====\n".encode())
    f.flush()
    s = None
    while time.time() < end:
        try:
            if s is None:
                s = serial.Serial()
                s.port, s.baudrate, s.timeout, s.dtr, s.rts = port, 115200, 0.5, False, False
                s.open()
            cmd = out + ".cmd"  # lines dropped here are sent to the C3
            if os.path.exists(cmd):
                lines = open(cmd, "rb").read()
                os.remove(cmd)
                s.write(lines.replace(b"\r\n", b"\n").rstrip(b"\n") + b"\n")
                f.write(b"\n>>> " + lines.strip() + b"\n")
            d = s.read(4096)
            if d:
                f.write(d)
                f.flush()
        except (serial.SerialException, OSError):
            s = None
            time.sleep(1)
print("capture finished")
