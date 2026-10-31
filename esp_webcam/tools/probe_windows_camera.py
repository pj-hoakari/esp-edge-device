"""Read USB hub descriptors for 349c:3307. No capture or driver changes.

Windows only. IOCTL definitions/structures: Microsoft usbioctl.h documentation.
Run from a terminal permitted to read USB hubs. Current PC speed is not proof
of Full Speed compatibility on ESP32-S3. Output contains no captured images.
"""
import ctypes as c
from ctypes import wintypes as w
import json
import struct
import sys
import uuid

if sys.platform != "win32":
    raise SystemExit("Windows only")

class GUID(c.Structure):
    _fields_ = [("Data1", w.DWORD), ("Data2", w.WORD), ("Data3", w.WORD), ("Data4", c.c_ubyte * 8)]

class INTERFACE(c.Structure):
    _fields_ = [("cbSize", w.DWORD), ("guid", GUID), ("flags", w.DWORD), ("reserved", c.c_void_p)]

setup = c.WinDLL("setupapi", use_last_error=True)
kernel = c.WinDLL("kernel32", use_last_error=True)
setup.SetupDiGetClassDevsW.argtypes = [c.POINTER(GUID), w.LPCWSTR, w.HWND, w.DWORD]
setup.SetupDiGetClassDevsW.restype = c.c_void_p
setup.SetupDiEnumDeviceInterfaces.argtypes = [c.c_void_p, c.c_void_p, c.POINTER(GUID), w.DWORD, c.POINTER(INTERFACE)]
setup.SetupDiGetDeviceInterfaceDetailW.argtypes = [c.c_void_p, c.POINTER(INTERFACE), c.c_void_p, w.DWORD, c.POINTER(w.DWORD), c.c_void_p]
setup.SetupDiDestroyDeviceInfoList.argtypes = [c.c_void_p]
kernel.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, c.c_void_p, w.DWORD, w.DWORD, c.c_void_p]
kernel.CreateFileW.restype = w.HANDLE
kernel.DeviceIoControl.argtypes = [w.HANDLE, w.DWORD, c.c_void_p, w.DWORD, c.c_void_p, w.DWORD, c.POINTER(w.DWORD), c.c_void_p]
kernel.CloseHandle.argtypes = [w.HANDLE]
INVALID = c.c_void_p(-1).value

def ioctl(handle, code, data, size):
    buf = c.create_string_buffer(size)
    c.memmove(buf, data, len(data))
    got = w.DWORD()
    if not kernel.DeviceIoControl(handle, code, buf, len(data), buf, size, c.byref(got), None):
        return None
    return bytes(buf.raw[:got.value])

def describe(config):
    result, video, coding = [], False, None
    pos = 0
    while pos + 2 <= len(config):
        n, kind = config[pos:pos+2]
        if n < 2 or pos + n > len(config):
            raise ValueError("Malformed USB descriptor")
        d = config[pos:pos+n]
        if kind == 4 and n >= 9:
            video = d[5] == 14 and d[6] == 2
            coding = None
        if video and kind == 0x24 and n >= 3:
            sub = d[2]
            if sub == 6:
                coding = "MJPEG"
            elif sub == 4:
                coding = "uncompressed"
            if sub in (5, 7) and n >= 26:
                width, height = struct.unpack_from("<HH", d, 5)
                default = struct.unpack_from("<I", d, 21)[0]
                intervals = [struct.unpack_from("<I", d, j)[0] for j in range(26, n-3, 4)]
                result.append({"format": coding, "width": width, "height": height,
                               "default_fps": round(1e7/default, 3) if default else None,
                               "interval_type": d[25], "intervals_100ns": intervals,
                               "max_frame_bytes": struct.unpack_from("<I", d, 17)[0]})
        pos += n
    return result

hub_guid = GUID.from_buffer_copy(uuid.UUID("f18a0e88-c30c-11d0-8815-00a0c906bed8").bytes_le)
devices = setup.SetupDiGetClassDevsW(c.byref(hub_guid), None, None, 0x12)
if devices == INVALID:
    raise c.WinError(c.get_last_error())
found = []
try:
    index = 0
    while True:
        interface = INTERFACE(); interface.cbSize = c.sizeof(INTERFACE)
        if not setup.SetupDiEnumDeviceInterfaces(devices, None, c.byref(hub_guid), index, c.byref(interface)):
            if c.get_last_error() != 259:
                raise c.WinError(c.get_last_error())
            break
        index += 1
        needed = w.DWORD()
        setup.SetupDiGetDeviceInterfaceDetailW(devices, c.byref(interface), None, 0, c.byref(needed), None)
        detail = c.create_string_buffer(needed.value)
        struct.pack_into("<I", detail, 0, 8 if c.sizeof(c.c_void_p) == 8 else 6)
        if not setup.SetupDiGetDeviceInterfaceDetailW(devices, c.byref(interface), detail, needed.value, None, None):
            continue
        path = c.wstring_at(c.addressof(detail) + 4)
        hub = kernel.CreateFileW(path, 0x40000000, 3, None, 3, 0, None)
        if hub == INVALID:
            continue
        try:
            info = ioctl(hub, 0x220408, bytes(76), 76)
            if not info or len(info) < 7:
                continue
            for port in range(1, info[6] + 1):
                conn = ioctl(hub, 0x220448, struct.pack("<I", port) + bytes(4092), 4096)
                if not conn or len(conn) < 36 or struct.unpack_from("<HH", conn, 12) != (0x349c, 0x3307):
                    continue
                entry = {"vid_pid": "349c:3307", "pc_speed": {0:"low",1:"full",2:"high"}.get(conn[23], "unknown")}
                for kind, label in [(2,"current_configuration"),(7,"other_speed_configuration")]:
                    req = struct.pack("<IBBHHH", port, 0x80, 6, kind << 8, 0, 65535)
                    desc = ioctl(hub, 0x220410, req, 65547)
                    if desc and len(desc) >= 21:
                        entry[label] = describe(desc[12:])
                        entry[label + '_total_bytes'] = struct.unpack_from('<H', desc, 14)[0]
                        endpoints = []
                        pos = 12
                        while pos + 2 <= len(desc):
                            length, desc_type = desc[pos:pos+2]
                            if length < 2 or pos + length > len(desc):
                                break
                            if desc_type == 5 and length >= 7:
                                endpoints.append({'address': hex(desc[pos+2]),
                                                  'transfer_type': desc[pos+3] & 3,
                                                  'max_packet_bytes': struct.unpack_from('<H', desc, pos+4)[0] & 0x7ff})
                            pos += length
                        entry[label + '_endpoints'] = endpoints
                    else:
                        entry[label] = None
                found.append(entry)
        finally:
            kernel.CloseHandle(hub)
finally:
    setup.SetupDiDestroyDeviceInfoList(devices)
print(json.dumps(found, ensure_ascii=False, indent=2))
if not found:
    raise SystemExit("Camera not found or USB hub access unavailable")
