"""Select the HTRAM ESP32's boot mode through the C3 bridge.

    python htram_reset.py COM19 boot     -> next power-on enters download mode
    python htram_reset.py COM19 run      -> next power-on runs the firmware
    python htram_reset.py COM19 status   -> print what the bridge is doing

Needs C3 pin 5 wired to the module's IO0. The setting latches, so power-cycle
the monitor afterwards (unless C3 pin 6 is also wired to the module's EN, in
which case it resets by itself).
"""
import sys, time
import serial

port, mode = sys.argv[1], (sys.argv[2] if len(sys.argv) > 2 else "boot")
magic = {"boot": b"<<HTRAM-BOOT>>", "run": b"<<HTRAM-RUN>>", "status": b"<<HTRAM-STATUS>>"}[mode]

s = serial.Serial()
s.port, s.baudrate, s.timeout = port, 115200, 0.5
s.dtr = s.rts = False
s.open()
s.reset_input_buffer()
s.write(magic)
s.flush()
time.sleep(0.6)
reply = s.read(s.in_waiting or 1)
s.close()
print(f"sent {magic.decode()}")
for line in reply.decode("ascii", "replace").splitlines():
    if "[bridge]" in line:
        print(line.strip())
