#!/usr/bin/env python3
"""A fake Discord for testing rt_discord.c on Linux: listens on $XDG_RUNTIME_DIR/discord-ipc-0
(or the path given), answers HANDSHAKE with READY and every command with its echo, and prints
each activity the game sends. docs/DISCORD.md"""
import json, os, socket, struct, sys
path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.environ.get('XDG_RUNTIME_DIR') or '/tmp', 'discord-ipc-0')
if os.path.exists(path): os.unlink(path)
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); s.bind(path); s.listen(1)
print('fake Discord on', path, flush=True)
def recv(c, n):
    b = b''
    while len(b) < n:
        x = c.recv(n - len(b))
        if not x: raise EOFError
        b += x
    return b
def send(c, op, obj):
    d = json.dumps(obj).encode(); c.sendall(struct.pack('<II', op, len(d)) + d)
while True:
    c, _ = s.accept(); print('game connected', flush=True)
    try:
        while True:
            op, n = struct.unpack('<II', recv(c, 8)); msg = json.loads(recv(c, n))
            if op == 0:
                print('HANDSHAKE', msg, flush=True)
                send(c, 1, {'cmd': 'DISPATCH', 'evt': 'READY', 'data': {'v': 1, 'user': {'username': 'tester'}}})
            else:
                print('ACTIVITY', json.dumps(msg['args']['activity']), flush=True)
                send(c, 1, {'cmd': msg['cmd'], 'evt': None, 'nonce': msg['nonce'], 'data': msg['args']['activity']})
    except EOFError:
        print('game disconnected', flush=True)
