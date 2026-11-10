#!/usr/bin/env python3
"""Copy a sysrr log, flipping one byte of a recorded syscall output.

usage: tamper_log.py IN OUT GEN NTH_READ
Flips the first byte of the D_OUT data of the NTH_READ-th read() (0-based)
of generation GEN.  Replay will inject the wrong byte; the target then
computes something else and the divergence check must catch it.
"""
import struct, sys

src, dst, gen, nth = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
buf = bytearray(open(src, "rb").read())
hdr_size = struct.unpack_from("<I", buf, 12)[0]

reads, data = [], {}
off = hdr_size
while off + 16 <= len(buf):
    t, fl, ln, seq = struct.unpack_from("<HHIQ", buf, off)
    if t == 2:                                   # REC_DATA: tid arg kind idx off len
        kind = buf[off + 16 + 5]
        if kind == 1:                            # D_OUT
            data.setdefault(seq, off + 16 + 16)  # where its bytes start
    elif t == 1:                                 # REC_SYSCALL
        g = struct.unpack_from("<I", buf, off + 16 + 32)[0]
        nr = struct.unpack_from("<q", buf, off + 16 + 40)[0]
        if g == gen and nr == 0 and not fl & 1:
            reads.append(seq)
    off += ln
seq = sorted(reads)[nth]
pos = data[seq]
print("read #%d (seq %d): byte 0x%02x -> 0x%02x" % (nth, seq, buf[pos], buf[pos] ^ 0xff))
buf[pos] ^= 0xff
open(dst, "wb").write(buf)
