"""
STM32 固件升级工具 - 协议层
协议: 0xAA TYPE LEN_H LEN_L SEQ_H SEQ_L PAYLOAD CRC16_H CRC16_L 0x55
"""

PKT_HEAD = 0xAA
PKT_TAIL = 0x55

# 帧类型
PKT_TRIGGER = 0xF0
PKT_READY   = 0x01
PKT_INFO    = 0x02
PKT_DATA    = 0x03
PKT_ACK     = 0x04
PKT_NAK     = 0x05
PKT_COMPLETE= 0x06
PKT_OK      = 0x07
PKT_STATUS  = 0x08

# 错误码
ERR_CRC    = 0x01
ERR_SEQ    = 0x02
ERR_FLASH  = 0x03
ERR_SIZE   = 0x04
ERR_UNKNOWN= 0x05

DATA_PAYLOAD_MAX = 256
PKT_MIN_LEN = 9  # HEAD + TYPE + LEN(2) + SEQ(2) + CRC(2) + TAIL


def crc16(data: bytes) -> int:
    """CRC16-CCITT (多项式 0x8408 反转)"""
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0x8408
            else:
                crc >>= 1
    return crc


def build_frame(type_: int, seq: int = 0, payload: bytes = b'') -> bytes:
    """构建一帧"""
    assert len(payload) <= DATA_PAYLOAD_MAX, f"Payload too long: {len(payload)}"

    header = bytes([
        PKT_HEAD,
        type_,
        (len(payload) >> 8) & 0xFF,
        len(payload) & 0xFF,
        (seq >> 8) & 0xFF,
        seq & 0xFF,
    ])

    crc_val = crc16(header[1:] + payload)
    frame = header + payload + bytes([(crc_val >> 8) & 0xFF, crc_val & 0xFF, PKT_TAIL])
    return frame


class FrameParser:
    """帧解析器（状态机）"""

    def __init__(self):
        self.reset()

    def reset(self):
        self.state = 'WAIT_HEAD'
        self.type_ = 0
        self.len_ = 0
        self.seq_ = 0
        self.payload = b''
        self.crc_recv = 0
        self.buf = bytearray()
        self.expected_len = 0

    def feed(self, byte: int) -> dict or None:
        """喂一个字节，返回完整帧 dict 或 None"""
        if self.state == 'WAIT_HEAD':
            if byte == PKT_HEAD:
                self.state = 'WAIT_TYPE'
                self.buf = bytearray()

        elif self.state == 'WAIT_TYPE':
            self.type_ = byte
            self.buf.append(byte)
            self.state = 'WAIT_LEN_H'

        elif self.state == 'WAIT_LEN_H':
            self.len_ = byte << 8
            self.buf.append(byte)
            self.state = 'WAIT_LEN_L'

        elif self.state == 'WAIT_LEN_L':
            self.len_ |= byte
            self.buf.append(byte)
            if self.len_ > DATA_PAYLOAD_MAX:
                self.state = 'WAIT_HEAD'  # 非法长度
            else:
                self.state = 'WAIT_SEQ_H'

        elif self.state == 'WAIT_SEQ_H':
            self.seq_ = byte << 8
            self.buf.append(byte)
            self.state = 'WAIT_SEQ_L'

        elif self.state == 'WAIT_SEQ_L':
            self.seq_ |= byte
            self.buf.append(byte)
            if self.len_ == 0:
                self.state = 'WAIT_CRC_H'
            else:
                self.state = 'WAIT_PAYLOAD'
                self.payload = bytearray()
                self.expected_len = self.len_

        elif self.state == 'WAIT_PAYLOAD':
            self.payload.append(byte)
            self.buf.append(byte)
            if len(self.payload) >= self.expected_len:
                self.state = 'WAIT_CRC_H'

        elif self.state == 'WAIT_CRC_H':
            self.crc_recv = byte << 8
            self.state = 'WAIT_CRC_L'

        elif self.state == 'WAIT_CRC_L':
            self.crc_recv |= byte
            self.state = 'WAIT_TAIL'

        elif self.state == 'WAIT_TAIL':
            ok = (byte == PKT_TAIL)
            data_for_crc = bytes(self.buf)
            crc_calc = crc16(data_for_crc)
            crc_ok = (crc_calc == self.crc_recv)

            frame = {
                'type': self.type_,
                'len': self.len_,
                'seq': self.seq_,
                'payload': bytes(self.payload) if self.len_ > 0 else b'',
                'crc_ok': crc_ok,
                'crc_recv': self.crc_recv,
                'crc_calc': crc_calc,
            }
            self.reset()
            return frame

        return None
