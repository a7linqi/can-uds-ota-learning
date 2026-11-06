import sys
sys.path.insert(0, "tools")
from pc.transport import SimTransport
from pc.isotp_client import IsoTpClient
from pc.uds_client import UdsClient, NegativeResponse, demo_key


def hx(data):
    return " ".join(f"{b:02X}" for b in data)


def be32(v):
    return int(v).to_bytes(4, "big")


transport = SimTransport(compiler="gcc")
client = UdsClient(IsoTpClient(transport))
seeds = []


def show(label, req):
    try:
        r = client.request(req)
        print(f"{label:<26} {hx(req):<28} -> {hx(r)}")
        return r
    except NegativeResponse as e:
        print(f"{label:<26} {hx(req):<28} -> 7F {e.sid:02X} {e.nrc:02X}")
        return None


print("== 1. 进编程会话 ==")
show("10 02", b"\x10\x02")

print()
print("== 2. 第一次要种子 ==")
r = show("27 01 (第1次)", b"\x27\x01")
seed1 = int.from_bytes(r[2:6], "big")
seeds.append(seed1)
key1 = demo_key(seed1)
print(f"   种子 = {seed1:08X}, 正确 key = {key1:08X}")

print()
print("== 3. 用错误 key ==")
show("27 02 错误key", b"\x27\x02" + be32(key1 ^ 0xFFFFFFFF))

print()
print("== 4. 不重新要种子，直接再送 key ==")
show("27 02 再送一次", b"\x27\x02" + be32(key1))

print()
print("== 5. 重新要种子（应该是新值）==")
r = show("27 01 (第2次)", b"\x27\x01")
seed2 = int.from_bytes(r[2:6], "big")
seeds.append(seed2)
print(f"   种子 = {seed2:08X}, 和第一次相同? {seed1 == seed2}")
print(f"   用旧种子算的 key = {key1:08X}")
print(f"   用新种子算的 key = {demo_key(seed2):08X}")

print()
print("== 6. 用旧种子的 key 去配新种子 ==")
show("27 02 旧种子的key", b"\x27\x02" + be32(key1))

print()
print("== 7. 重新要种子，用正确 key ==")
r = show("27 01 (第3次)", b"\x27\x01")
seed3 = int.from_bytes(r[2:6], "big")
print(f"   种子 = {seed3:08X}")
show("27 02 正确key", b"\x27\x02" + be32(demo_key(seed3)))

print()
print("== 8. 已解锁后再要种子 ==")
show("27 01 (已解锁)", b"\x27\x01")

print()
print("== 9. 切会话后解锁是否还在 ==")
show("10 02 重新进会话", b"\x10\x02")
show("27 01 切会话后", b"\x27\x01")

transport.close()
