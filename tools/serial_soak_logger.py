"""Long-running serial soak logger for the Mira Stage 2 verification.

Writes host-timestamped serial lines to a device-monitor-style log in logs/.
Auto-reconnects if the port drops (USB hiccup); exits only at DURATION_S or
if the port stays gone for RECONNECT_GIVE_UP_S.
"""
import sys, time
import serial

PORT = "COM5"
BAUD = 115200
DURATION_S = 72 * 3600          # 72-hour cap
RECONNECT_GIVE_UP_S = 6 * 3600  # stop if the board is gone this long
OUT = sys.argv[1]

end = time.time() + DURATION_S
ser = None
last_ok = time.time()

with open(OUT, "a", encoding="utf-8", errors="replace") as f:
    f.write(f"--- soak logger started {time.strftime('%Y-%m-%d %H:%M:%S')} ---\n")
    f.flush()
    while time.time() < end:
        if ser is None:
            try:
                ser = serial.Serial(PORT, BAUD, timeout=1)
                f.write(f"--- port opened {time.strftime('%H:%M:%S')} ---\n")
                f.flush()
                last_ok = time.time()
            except serial.SerialException:
                if time.time() - last_ok > RECONNECT_GIVE_UP_S:
                    f.write("--- giving up: port gone > 6 h ---\n")
                    break
                time.sleep(5)
                continue
        try:
            line = ser.readline()
            if line:
                f.write(f"[{time.strftime('%m-%d %H:%M:%S')}] {line.decode(errors='replace')}")
                f.flush()
                last_ok = time.time()
        except serial.SerialException:
            f.write(f"--- port lost {time.strftime('%H:%M:%S')}, retrying ---\n")
            f.flush()
            try:
                ser.close()
            except Exception:
                pass
            ser = None
    f.write(f"--- soak logger stopped {time.strftime('%Y-%m-%d %H:%M:%S')} ---\n")
