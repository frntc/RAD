# Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
import serial
import sys
import time
import math

fileaddress = sys.argv[1]
flashport = sys.argv[2]
flashbaud = int(sys.argv[3])
monitor_secs = float(sys.argv[4]) if len(sys.argv) > 4 else 15.0

ser = serial.Serial(
    port=flashport,
    baudrate=flashbaud,
    parity=serial.PARITY_NONE,
    stopbits=serial.STOPBITS_ONE,
    bytesize=serial.EIGHTBITS,
    timeout=0.2,
)
ser.reset_input_buffer()

F = open(fileaddress, "rb")
F.seek(0, 2)
size = F.tell()
F.seek(0, 0)

print("Flashing with baudrate of " + str(flashbaud) + "...")
sys.stdout.flush()

ser.write(b"R")
blocksize = math.trunc(flashbaud / 5)
offset = 0
seen_during_xfer = b""
while offset < size:
    readlen = blocksize
    if size - offset < readlen:
        readlen = size - offset
    data = F.read(readlen)
    ser.write(data)
    offset += readlen
    # drain anything the device sends back mid-transfer (errors, acks, etc.)
    pending = ser.read(ser.in_waiting or 1)
    if pending:
        seen_during_xfer += pending

print("Completed transfer. Bytes seen from device during transfer: %r" % seen_during_xfer)
sys.stdout.flush()

# give the device a moment, then drain anything else pending before "go"
time.sleep(0.3)
pending = ser.read(4096)
if pending:
    print("Bytes seen from device post-transfer, pre-go: %r" % pending)
    sys.stdout.flush()

print("Sending go...")
sys.stdout.flush()
time.sleep(1)
ser.write(b"g")

print("--- monitoring same fd for %.1fs ---" % monitor_secs)
sys.stdout.flush()
end = time.time() + monitor_secs
total = b""
while time.time() < end:
    chunk = ser.read(4096)
    if chunk:
        total += chunk
        sys.stdout.buffer.write(chunk)
        sys.stdout.flush()
print("\n--- done, %d bytes received after go ---" % len(total))
print("raw: %r" % total)

F.close()
ser.close()
