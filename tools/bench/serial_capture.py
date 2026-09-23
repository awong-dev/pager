import serial, time, re, sys, glob, os
# r2: like serial_capture.py, but flushes to disk on every read instead of
# buffering everything in memory until exit (that buffering is suspected to
# be why an earlier capture produced a 0-byte log file when interrupted).
# usage: r2_serial.py <outfile> <total_s> <cmd_at_s>:<cmd> ... ; survives the
# port vanishing and being renamed.
outfile = sys.argv[1]
total = float(sys.argv[2])
cmds = sorted([(float(a.split(':', 1)[0]), a.split(':', 1)[1]) for a in sys.argv[3:]])
t0 = time.time(); i = 0; s = None; gaps = 0; absent = 0.0
# 'wb', not 'ab': on 23 Sep a re-used filename appended a new boot after an
# old one and the seam was read as an unexplained reset and a failed check.
# One capture, one file. Port drops within a run reopen the port, not the file.
f = open(outfile, 'wb', buffering=0)
ansi = re.compile(rb'\x1b\[[0-9;]*[A-Za-z]')
while time.time() - t0 < total:
    now = time.time()
    if s is None:
        ports = glob.glob('/dev/cu.usbmodem*')
        if ports:
            try:
                s = serial.Serial(ports[0], 115200, timeout=0.2)
            except Exception:
                s = None
        if s is None:
            absent += time.time() - now + 0.2
            time.sleep(0.2)
            continue
    try:
        chunk = s.read(8192)
    except Exception:
        try:
            s.close()
        except Exception:
            pass
        s = None; gaps += 1; continue
    if chunk:
        f.write(ansi.sub(b'', chunk))
    if i < len(cmds) and time.time() - t0 >= cmds[i][0]:
        try:
            s.write((cmds[i][1] + "\r\n").encode()); i += 1
        except Exception:
            pass
s and s.close()
f.close()
sys.stderr.write(f"port lost {gaps} time(s), absent about {absent:.0f} s of {total:.0f} s; commands sent {i}/{len(cmds)}\n")
