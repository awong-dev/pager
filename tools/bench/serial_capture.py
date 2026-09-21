import serial, time, re, sys, glob
# usage: ser4.py <total_s> <cmd_at_s>:<cmd> ... ; survives the port vanishing and being renamed.
total=float(sys.argv[1]); cmds=sorted([(float(a.split(':',1)[0]), a.split(':',1)[1]) for a in sys.argv[2:]])
buf=b''; t0=time.time(); i=0; s=None; gaps=0; absent=0.0; last=time.time()
while time.time()-t0 < total:
    now=time.time()
    if s is None:
        ports=glob.glob('/dev/cu.usbmodem*')
        if ports:
            try: s=serial.Serial(ports[0],115200,timeout=0.2)
            except Exception: s=None
        if s is None:
            absent+=time.time()-now+0.2; time.sleep(0.2); continue
    try: buf+=s.read(8192)
    except Exception:
        try: s.close()
        except Exception: pass
        s=None; gaps+=1; continue
    if i<len(cmds) and time.time()-t0>=cmds[i][0]:
        try: s.write((cmds[i][1]+"\r\n").encode()); i+=1
        except Exception: pass
s and s.close()
sys.stderr.write(f"port lost {gaps} time(s), absent about {absent:.0f} s of {total:.0f} s; commands sent {i}/{len(cmds)}\n")
sys.stdout.write(re.sub(rb'\x1b\[[0-9;]*[A-Za-z]',b'',buf).decode('utf-8','replace'))
