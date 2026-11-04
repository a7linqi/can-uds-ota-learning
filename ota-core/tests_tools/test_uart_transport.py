"""Wire-format compatibility for the PC UART bench backend."""
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from pc.transport import (Frame, CanUartDecoder, SerialCanTransport,
                          can_uart_crc16, can_uart_encode)


class FakeSerial:
    def __init__(self):
        self.rx = bytearray()
        self.tx = bytearray()
        self.closed = False

    def write(self, data):
        self.tx.extend(data)
        return len(data)

    def flush(self):
        pass

    def read(self, size):
        if not self.rx:
            return b""
        count = min(size, len(self.rx))
        data = bytes(self.rx[:count])
        del self.rx[:count]
        return data

    def close(self):
        self.closed = True


class UartWireTests(unittest.TestCase):
    def test_crc_vector(self):
        self.assertEqual(can_uart_crc16(b"123456789"), 0x29B1)

    def test_standard_frame_matches_c_vector(self):
        frame = Frame(0x7E8, bytes(range(8)))
        self.assertEqual(
            can_uart_encode(frame).hex(),
            "a55a0108000007e800010203040506078d70",
        )
        decoder = CanUartDecoder()
        decoded = None
        for byte in can_uart_encode(frame):
            decoded = decoder.feed(byte) or decoded
        self.assertEqual(decoded, frame)

    def test_extended_frame_roundtrip(self):
        frame = Frame(0x18DAF110, b"\x01\x02\x03", extended=True)
        raw = can_uart_encode(frame)
        self.assertEqual(raw[2], 0x81)
        decoder = CanUartDecoder()
        decoded = None
        for byte in raw:
            decoded = decoder.feed(byte) or decoded
        self.assertEqual(decoded, frame)

    def test_noise_then_frame(self):
        frame = Frame(0x7E0, b"\x02\x10\x03")
        raw = bytes([0x00, 0xFF, 0xA5, 0x11]) + can_uart_encode(frame)
        decoder = CanUartDecoder()
        decoded = None
        for byte in raw:
            decoded = decoder.feed(byte) or decoded
        self.assertEqual(decoded, frame)

    def test_corrupt_crc_rejected_then_recovers(self):
        frame = Frame(0x7E0, b"\x02\x10\x03")
        bad = bytearray(can_uart_encode(frame))
        bad[-1] ^= 1
        decoder = CanUartDecoder()
        for byte in bad:
            self.assertIsNone(decoder.feed(byte))
        decoded = None
        for byte in can_uart_encode(frame):
            decoded = decoder.feed(byte) or decoded
        self.assertEqual(decoded, frame)

    def test_serial_transport_send_and_recv(self):
        fake = FakeSerial()
        transport = SerialCanTransport("fake", serial_instance=fake)
        frame = Frame(0x7E0, b"\x02\x10\x03")
        transport.send(frame)
        self.assertEqual(bytes(fake.tx), can_uart_encode(frame))
        fake.rx.extend(can_uart_encode(Frame(0x7E8, b"\x06\x50\x03\x00\x32\x01\xF4")))
        received = transport.recv(0.05)
        self.assertEqual(received, Frame(0x7E8, b"\x06\x50\x03\x00\x32\x01\xF4"))
        transport.close()
        self.assertTrue(fake.closed)


if __name__ == "__main__":
    unittest.main()
