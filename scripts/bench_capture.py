#!/usr/bin/env python3
# Reliable USB-C serial-console capture for the carsh bench loop.
#
#   python3 scripts/bench_capture.py [logfile]      # default: bench-<pid>.log
#
# Why this exists (gotcha, learned the hard way 2026-06-29): a plain
# `cat /dev/ttyACM0` or `stty -F /dev/ttyACM0 ...` BLOCKS on open() — the Circle
# CDC-ACM gadget doesn't assert carrier (DCD), so a normal open waits forever and
# the capture silently hangs. The fix is to open NON-BLOCKING with CLOCAL set
# (ignore modem-control lines). This script does that, streams to a log, and
# RE-ATTACHES across Pi power-cycles (the port disappears/reappears on reboot), so
# you start it once and leave it running through many flash/boot iterations.
import os, sys, select, termios, time

PORT = "/dev/ttyACM0"
LOG = sys.argv[1] if len(sys.argv) > 1 else "bench-%d.log" % os.getpid()
out = open(LOG, "ab", buffering=0)  # unbuffered binary append

def note(msg):
    out.write(("[capture] " + time.strftime("%H:%M:%S") + " " + msg + "\n").encode())

note("started -> " + LOG)
while True:
    try:
        fd = os.open(PORT, os.O_RDONLY | os.O_NONBLOCK | os.O_NOCTTY)
    except OSError:
        time.sleep(0.5)
        continue
    try:
        a = termios.tcgetattr(fd)            # [iflag,oflag,cflag,lflag,isp,osp,cc]
        a[0] = 0                              # raw input
        a[1] &= ~termios.OPOST                # raw output
        a[2] |= (termios.CLOCAL | termios.CREAD)   # <-- ignore carrier; the key bit
        a[3] &= ~(termios.ICANON | termios.ECHO | termios.ECHOE |
                  termios.ECHOK | termios.ISIG)
        a[4] = termios.B115200
        a[5] = termios.B115200
        termios.tcsetattr(fd, termios.TCSANOW, a)
    except Exception as e:
        note("termios warn: %r" % e)
    note("ATTACHED " + PORT)
    while True:
        try:
            r, _, _ = select.select([fd], [], [], 1.0)
        except OSError:
            break
        if not r:
            continue
        try:
            data = os.read(fd, 4096)
        except OSError:
            break
        if not data:
            break
        out.write(data)
    try:
        os.close(fd)
    except OSError:
        pass
    note("DETACHED " + PORT + " (reboot / power-cycle?)")
    time.sleep(0.5)
