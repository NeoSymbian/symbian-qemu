#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# syborg-view.py -- a live window onto a Syborg guest started with `run-syborg` in LIVE=1 mode, for when no VNC viewer
# is at hand (VNC is on port 5900). It polls the screen dump (a P6 PPM, which Tk reads natively) and redraws it. Key presses
# become monitor `sendkey` commands and mouse press/drag/release become `mouse_move` + `mouse_button` (touch), sent to
# the QEMU monitor on TCP port 4444. Without a reachable monitor it is a plain viewer. Standard library only.
#
# Usage:  python3 syborg-view.py [path-to-fb.ppm] [zoom] [monitor-port]
# Keys:   letters, digits and shell punctuation (\ : . _ * ? ...) as typed; arrows -> keypad 8/2/4/6 (what the Syborg
#         keyboard table maps for navigation), Enter = select, F5 = left softkey, F6 = OK key, F7 = right softkey,
#         + - * / -> keypad operators.
import os
import socket
import sys
import time
import tkinter as tk

path = sys.argv[1] if len(sys.argv) > 1 else "fb.ppm"
zoom = int(sys.argv[2]) if len(sys.argv) > 2 else 1
port = int(sys.argv[3]) if len(sys.argv) > 3 else 4444

KEYMAP = {
    "Return": "ret", "KP_Enter": "ret", "space": "spc", "BackSpace": "backspace", "Tab": "tab",
    "Escape": "esc", "Delete": "delete",
    "Up": "kp_8", "Down": "kp_2", "Left": "kp_4", "Right": "kp_6",
    "period": "dot", "comma": "comma", "minus": "kp_subtract", "plus": "kp_add",
    "asterisk": "kp_multiply", "slash": "kp_divide", "equal": "equal",
    "KP_Add": "kp_add", "KP_Subtract": "kp_subtract", "KP_Multiply": "kp_multiply",
    "KP_Divide": "kp_divide", "KP_Decimal": "kp_decimal",
    # what a shell needs (paths, wildcards); shifted ones as QEMU's shift-<key>
    "backslash": "backslash", "colon": "shift-semicolon", "semicolon": "semicolon", "underscore": "shift-minus",
    "question": "shift-slash", "greater": "shift-dot", "less": "shift-comma", "quotedbl": "shift-apostrophe",
    "apostrophe": "apostrophe", "exclam": "shift-1", "bar": "shift-backslash",
}
for i in range(1, 13):
    KEYMAP["F%d" % i] = "f%d" % i
for i in range(10):
    KEYMAP["KP_%d" % i] = "kp_%d" % i


class Monitor:
    """Line-oriented client of the QEMU human monitor; reconnects lazily, never blocks the UI."""

    def __init__(self, port):
        self.port, self.sock, self.next_try = port, None, 0.0

    def send(self, line):
        if self.sock is None and time.time() >= self.next_try:
            try:
                self.sock = socket.create_connection(("127.0.0.1", self.port), timeout=0.5)
                self.sock.setblocking(False)
            except OSError:
                self.sock, self.next_try = None, time.time() + 3
        if self.sock is None:
            return False
        try:
            try:
                while self.sock.recv(65536):      # drain the echo so the pipe never fills
                    pass
            except BlockingIOError:
                pass
            self.sock.sendall((line + "\n").encode())
            return True
        except OSError:
            self.sock, self.next_try = None, time.time() + 1
            return False


mon = Monitor(port)
root = tk.Tk()
root.title("Symbian (Syborg) - live")
lbl = tk.Label(root, bg="black", cursor="hand2")
lbl.pack()
status = tk.Label(root, anchor="w", font=("TkFixedFont", 9))
status.pack(fill="x")
state = {"img": None, "w": 0, "h": 0, "last_move": 0.0}


def say(text):
    status.configure(text=text)


def on_key(ev):
    ks = ev.keysym
    if ks in KEYMAP:
        name = KEYMAP[ks]
    elif len(ks) == 1 and ks.isalnum():
        name = ("shift-" + ks.lower()) if ks.isupper() else ks
    else:
        say("unmapped key: %s" % ks)
        return
    say(("sendkey %s" % name) if mon.send("sendkey " + name) else "no monitor on :%d (view only)" % port)


def guest_xy(ev):
    w, h = state["w"], state["h"]
    if not w or not h:
        return None
    x = min(max(ev.x // zoom, 0), w - 1)
    y = min(max(ev.y // zoom, 0), h - 1)
    # absolute QEMU mouse coordinates span 0..0x7fff over the display
    return x, y, x * 0x7FFF // (w - 1), y * 0x7FFF // (h - 1)


def on_press(ev):
    p = guest_xy(ev)
    if p:
        ok = mon.send("mouse_move %d %d" % (p[2], p[3])) and mon.send("mouse_button 1")
        say(("touch down %d,%d" % (p[0], p[1])) if ok else "no monitor on :%d (view only)" % port)


def on_drag(ev):
    p = guest_xy(ev)
    if p and time.time() - state["last_move"] > 0.04:
        state["last_move"] = time.time()
        mon.send("mouse_move %d %d" % (p[2], p[3]))


def on_release(ev):
    p = guest_xy(ev)
    if p:
        mon.send("mouse_move %d %d" % (p[2], p[3]))
        mon.send("mouse_button 0")
        say("touch up %d,%d" % (p[0], p[1]))


def tick():
    try:
        img = tk.PhotoImage(file=path)
        state["w"], state["h"] = img.width(), img.height()
        if zoom > 1:
            img = img.zoom(zoom, zoom)
        lbl.configure(image=img)
        state["img"] = img          # hold a ref so Tk doesn't GC it
    except Exception:
        pass                        # file missing / mid-write -> keep last frame
    root.after(120, tick)


root.bind("<Key>", on_key)
lbl.bind("<ButtonPress-1>", on_press)
lbl.bind("<B1-Motion>", on_drag)
lbl.bind("<ButtonRelease-1>", on_release)
say("keys + clicks go to the guest (monitor :%d)" % port)
tick()
root.mainloop()
