import socket
import struct
import time

ML_CTRL_MAGIC = 0x4D4C4354
ML_CTRL_VERSION = 1

ML_CTRL_CMD_MOUSE_ABS = 1
ML_CTRL_CMD_MOUSE_REL = 2
ML_CTRL_CMD_MOUSE_BUTTON = 3
ML_CTRL_CMD_MOUSE_CLICK = 4
ML_CTRL_CMD_MOUSE_SCROLL = 5
ML_CTRL_CMD_MOUSE_HSCROLL = 6
ML_CTRL_CMD_KEYBOARD = 7
ML_CTRL_CMD_KEY_PRESS = 8
ML_CTRL_CMD_TEXT = 9

BUTTON_ACTION_PRESS = 0x07
BUTTON_ACTION_RELEASE = 0x08

BUTTON_LEFT = 0x01
BUTTON_MIDDLE = 0x02
BUTTON_RIGHT = 0x03
BUTTON_X1 = 0x04
BUTTON_X2 = 0x05

KEY_ACTION_DOWN = 0x03
KEY_ACTION_UP = 0x04

MODIFIER_SHIFT = 0x01
MODIFIER_CTRL = 0x02
MODIFIER_ALT = 0x04
MODIFIER_META = 0x08

# 常用 Windows Virtual-Key（通常这里就是按这个用）
VK_BACK = 0x08
VK_TAB = 0x09
VK_RETURN = 0x0D
VK_SHIFT = 0x10
VK_CONTROL = 0x11
VK_MENU = 0x12      # Alt
VK_PAUSE = 0x13
VK_CAPITAL = 0x14   # Caps Lock
VK_ESCAPE = 0x1B
VK_SPACE = 0x20
VK_PRIOR = 0x21     # Page Up
VK_NEXT = 0x22      # Page Down
VK_END = 0x23
VK_HOME = 0x24
VK_LEFT = 0x25
VK_UP = 0x26
VK_RIGHT = 0x27
VK_DOWN = 0x28
VK_SNAPSHOT = 0x2C  # Print Screen
VK_INSERT = 0x2D
VK_DELETE = 0x2E

VK_0 = 0x30
VK_1 = 0x31
VK_2 = 0x32
VK_3 = 0x33
VK_4 = 0x34
VK_5 = 0x35
VK_6 = 0x36
VK_7 = 0x37
VK_8 = 0x38
VK_9 = 0x39

VK_A = 0x41
VK_B = 0x42
VK_C = 0x43
VK_D = 0x44
VK_E = 0x45
VK_F = 0x46
VK_G = 0x47
VK_H = 0x48
VK_I = 0x49
VK_J = 0x4A
VK_K = 0x4B
VK_L = 0x4C
VK_M = 0x4D
VK_N = 0x4E
VK_O = 0x4F
VK_P = 0x50
VK_Q = 0x51
VK_R = 0x52
VK_S = 0x53
VK_T = 0x54
VK_U = 0x55
VK_V = 0x56
VK_W = 0x57
VK_X = 0x58
VK_Y = 0x59
VK_Z = 0x5A

VK_F1 = 0x70
VK_F2 = 0x71
VK_F3 = 0x72
VK_F4 = 0x73
VK_F5 = 0x74
VK_F6 = 0x75
VK_F7 = 0x76
VK_F8 = 0x77
VK_F9 = 0x78
VK_F10 = 0x79
VK_F11 = 0x7A
VK_F12 = 0x7B

_CMD_STRUCT = struct.Struct("<IHHiiiiQ")


class WorkerInputClient:
    def __init__(self, host="192.168.11.31", port=50001):
        self.addr = (host, port)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.seq = 0

    def _send(self, cmd_type, a=0, b=0, c=0, d=0, payload=b""):
        self.seq += 1
        hdr = _CMD_STRUCT.pack(
            ML_CTRL_MAGIC,
            ML_CTRL_VERSION,
            int(cmd_type),
            int(a),
            int(b),
            int(c),
            int(d),
            int(self.seq),
        )
        self.sock.sendto(hdr + payload, self.addr)

    # ---------- mouse ----------
    def mouse_abs(self, x, y, ref_w=0, ref_h=0):
        # 建议 x/y 直接用视频流坐标系
        self._send(ML_CTRL_CMD_MOUSE_ABS, x, y, ref_w, ref_h)

    def mouse_rel(self, dx, dy):
        self._send(ML_CTRL_CMD_MOUSE_REL, dx, dy)

    def button(self, button=BUTTON_LEFT, pressed=True):
        action = BUTTON_ACTION_PRESS if pressed else BUTTON_ACTION_RELEASE
        self._send(ML_CTRL_CMD_MOUSE_BUTTON, action, button)

    def click(self, button=BUTTON_LEFT):
        self._send(ML_CTRL_CMD_MOUSE_CLICK, button)

    def scroll(self, clicks):
        self._send(ML_CTRL_CMD_MOUSE_SCROLL, clicks)

    def hscroll(self, clicks):
        self._send(ML_CTRL_CMD_MOUSE_HSCROLL, clicks)

    # ---------- keyboard ----------
    def key(self, key_code, action, modifiers=0):
        self._send(ML_CTRL_CMD_KEYBOARD, key_code, action, modifiers)

    def key_down(self, key_code, modifiers=0):
        self.key(key_code, KEY_ACTION_DOWN, modifiers)

    def key_up(self, key_code, modifiers=0):
        self.key(key_code, KEY_ACTION_UP, modifiers)

    def key_press(self, key_code, modifiers=0):
        self._send(ML_CTRL_CMD_KEY_PRESS, key_code, modifiers)

    def hotkey(self, key_code, modifiers):
        # 例如 Ctrl+C, Alt+Tab
        self.key_press(key_code, modifiers)

    # ---------- text ----------
    def text(self, s: str):
        payload = s.encode("utf-8")
        self._send(ML_CTRL_CMD_TEXT, len(payload), payload=payload)


if __name__ == "__main__":
    time.sleep(1)
    c = WorkerInputClient(port=50001)

    # 鼠标
    c.mouse_abs(640, 360)
    # c.click(BUTTON_LEFT)
    c.scroll(-1)

    # 键盘快捷键：Ctrl+L

    # 普通文本输入
    c.text("hello world????")
    time.sleep(1)
    c.hotkey(VK_W, MODIFIER_CTRL)


