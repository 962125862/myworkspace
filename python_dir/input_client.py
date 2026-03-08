import socket
import struct
import time

ML_CTRL_MAGIC = 0x4D4C4354
ML_CTRL_VERSION = 1

ML_CTRL_CMD_MOUSE_ABS = 1
ML_CTRL_CMD_MOUSE_REL = 2
ML_CTRL_CMD_MOUSE_BUTTON = 3
ML_CTRL_CMD_MOUSE_CLICK = 4

BUTTON_ACTION_PRESS = 0x07
BUTTON_ACTION_RELEASE = 0x08

BUTTON_LEFT = 0x01
BUTTON_MIDDLE = 0x02
BUTTON_RIGHT = 0x03
BUTTON_X1 = 0x04
BUTTON_X2 = 0x05
ML_CTRL_CMD_MOUSE_SCROLL = 5
ML_CTRL_CMD_MOUSE_HSCROLL = 6

_CMD_STRUCT = struct.Struct("<IHHiiiiQ")


class WorkerInputClient:
    def __init__(self, host="127.0.0.1", port=50001):
        self.addr = (host, port)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.seq = 0

    def _send(self, cmd_type, a=0, b=0, c=0, d=0):
        self.seq += 1
        pkt = _CMD_STRUCT.pack(
            ML_CTRL_MAGIC,
            ML_CTRL_VERSION,
            cmd_type,
            int(a),
            int(b),
            int(c),
            int(d),
            int(self.seq),
        )
        self.sock.sendto(pkt, self.addr)

    def mouse_abs(self, x, y, ref_w=0, ref_h=0):
        # x/y 建议直接用当前流帧坐标系
        # ref_w/ref_h=0 表示让 worker 用它自己的流尺寸
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

if __name__ == "__main__":
    c = WorkerInputClient(port=50001)

    c.scroll(1)     # 上
    time.sleep(1)
    c.scroll(-1)    # 下

