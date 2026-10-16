#!/usr/bin/env python3
"""Create the minimal Wii NAND contents the game expects (same defaults Dolphin uses):
   nand/shared2/sys/SYSCONF                          console settings
   nand/title/00000001/00000002/data/setting.txt     encrypted region/video settings
Usage: python tools/make_nand.py [--lang N] [--widescreen]"""
import os, struct, sys
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIG, SMALL, BYTE, SHORT, LONG, LONGLONG, BOOL = 1, 2, 3, 4, 5, 6, 7

def bt_dinf():
    # conf_pads: u8 num_registered; pad registered[10]; pad active[4]; pad balance_board; u8 unk[0x45]
    # pad = bdaddr (6, reversed) + name (64). Four emulated remotes (runtime/rt_wiimote.c), the
    # addresses Dolphin uses; registered and active in order, so remote i is WPAD channel i.
    d = bytearray(0x461)
    d[0] = 4
    for i in range(4):
        pad = bytes([i, 0x00, 0x79, 0x19, 0x02, 0x11]) + b'Nintendo RVL-CNT-01'.ljust(0x40, b'\0')
        d[1 + i * 0x46:1 + (i + 1) * 0x46] = pad                  # registered[i]
        d[1 + (10 + i) * 0x46:1 + (11 + i) * 0x46] = pad          # active[i]
    return bytes(d)

def sysconf(lang=1, widescreen=False):
    nick = bytearray(22)
    for i, ch in enumerate('NSMBW'): nick[2 * i + 1] = ord(ch)
    nick[21] = 5
    sadr = bytearray(0x1008); sadr[0] = 0x31        # country: USA
    pc = bytearray(0x4A); pc[1] = 0x04; pc[2] = 0x14
    entries = [
        (BIG, 'BT.DINF', bt_dinf()), (BIG, 'BT.CDIF', bytes(0x205)),
        (LONG, 'BT.SENS', bytes([0, 0, 0, 3])), (BYTE, 'BT.BAR', b'\x01'), (BYTE, 'BT.SPKV', b'\x58'), (BYTE, 'BT.MOT', b'\x01'),
        (SMALL, 'IPL.NIK', bytes(nick)), (BYTE, 'IPL.LNG', bytes([lang])), (BIG, 'IPL.SADR', bytes(sadr)),
        (SMALL, 'IPL.PC', bytes(pc)), (LONG, 'IPL.CB', bytes(4)), (BYTE, 'IPL.AR', bytes([1 if widescreen else 0])),
        (BYTE, 'IPL.SSV', b'\x01'), (BOOL, 'IPL.CD', b'\x00'), (BOOL, 'IPL.CD2', b'\x00'), (BOOL, 'IPL.EULA', b'\x01'),
        (BYTE, 'IPL.UPT', b'\x02'), (BYTE, 'IPL.PGS', b'\x00'), (BYTE, 'IPL.E60', b'\x01'), (BYTE, 'IPL.DH', b'\x00'),
        (LONG, 'IPL.INC', bytes([0, 0, 0, 8])), (LONG, 'IPL.FRC', bytes([0, 0, 0, 0x28])), (SMALL, 'IPL.IDL', b'\x00\x01'),
        (LONG, 'NET.WCFG', bytes([0, 0, 0, 1])), (LONG, 'NET.CTPC', bytes(4)), (BYTE, 'WWW.RST', b'\x00'),
        (BOOL, 'MPLS.MOVIE', b'\x01'),
    ]
    body = bytearray(); offs = []
    start = 4 + 2 + 2 * (len(entries) + 1)
    for t, name, data in entries:
        offs.append(start + len(body))
        body.append((t << 5) | (len(name) - 1)); body += name.encode()
        if t == BIG: body += struct.pack('>H', len(data) - 1)
        elif t == SMALL: body.append(len(data) - 1)
        body += data
    offs.append(start + len(body))
    buf = bytearray(b'SCv0') + struct.pack('>H', len(entries)) + b''.join(struct.pack('>H', o) for o in offs) + body
    buf += bytes(0x4000 - len(buf)); buf[-4:] = b'SCed'
    return bytes(buf)

def setting_txt():
    text = ('AREA=USA\r\nMODEL=RVL-001(USA)\r\nDVD=0\r\nMPCH=0x7FFE\r\nCODE=LU\r\n'
            'SERNO=123456789\r\nVIDEO=NTSC\r\nGAME=US\r\n').encode()
    out = bytearray(0x100); key = 0x73B5DBFA
    for i, b in enumerate(text):
        out[i] = b ^ (key & 0xFF); key = ((key >> 31) | (key << 1)) & 0xFFFFFFFF
    return bytes(out)

NAND = os.path.join(ROOT, 'nand')
if '--out' in sys.argv: NAND = os.path.abspath(sys.argv[sys.argv.index('--out') + 1])

def write(rel, data):
    p = os.path.join(NAND, rel); os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, 'wb') as f: f.write(data)
    print('wrote', p)

if __name__ == '__main__':
    lang = int(sys.argv[sys.argv.index('--lang') + 1]) if '--lang' in sys.argv else 1
    write('shared2/sys/SYSCONF', sysconf(lang, '--widescreen' in sys.argv))
    write('title/00000001/00000002/data/setting.txt', setting_txt())
