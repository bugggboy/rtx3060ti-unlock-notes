"""Create a UEFI firmware boot entry (Boot#### + BootNext) pointing at
F:\\EFI\\BOOT\\BOOTX64.EFI, bypassing Windows Boot Manager entirely.
Reversible: delete the variable or clear BootNext.
"""
import ctypes
import ctypes.wintypes as wt
import struct
import sys

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
adv = ctypes.WinDLL("advapi32", use_last_error=True)

GUID_EFI_GLOBAL = "{8be4df61-93ca-11d2-aa0d-00e098032b8c}"

TOKEN_ADJUST_PRIVILEGES = 0x0020
TOKEN_QUERY = 0x0008
SE_PRIVILEGE_ENABLED = 0x0002


class LUID(ctypes.Structure):
    _fields_ = [("LowPart", wt.DWORD), ("HighPart", wt.LONG)]


class LUID_AND_ATTRIBUTES(ctypes.Structure):
    _fields_ = [("Luid", LUID), ("Attributes", wt.DWORD)]


class TOKEN_PRIVILEGES(ctypes.Structure):
    _fields_ = [("PrivilegeCount", wt.DWORD), ("Privileges", LUID_AND_ATTRIBUTES * 1)]


def enable_privilege():
    k32.GetCurrentProcess.restype = wt.HANDLE
    adv.OpenProcessToken.argtypes = [wt.HANDLE, wt.DWORD,
                                     ctypes.POINTER(wt.HANDLE)]
    adv.OpenProcessToken.restype = wt.BOOL
    adv.LookupPrivilegeValueW.argtypes = [wt.LPCWSTR, wt.LPCWSTR,
                                          ctypes.POINTER(LUID)]
    adv.LookupPrivilegeValueW.restype = wt.BOOL
    adv.AdjustTokenPrivileges.argtypes = [wt.HANDLE, wt.BOOL,
                                          ctypes.POINTER(TOKEN_PRIVILEGES),
                                          wt.DWORD, ctypes.c_void_p,
                                          ctypes.c_void_p]
    adv.AdjustTokenPrivileges.restype = wt.BOOL
    h = wt.HANDLE()
    if not adv.OpenProcessToken(k32.GetCurrentProcess(),
                                TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY,
                                ctypes.byref(h)):
        return False, ctypes.get_last_error()
    luid = LUID()
    if not adv.LookupPrivilegeValueW(None, "SeSystemEnvironmentPrivilege",
                                     ctypes.byref(luid)):
        return False, ctypes.get_last_error()
    tp = TOKEN_PRIVILEGES(1, (LUID_AND_ATTRIBUTES * 1)(
        LUID_AND_ATTRIBUTES(luid, SE_PRIVILEGE_ENABLED)))
    adv.AdjustTokenPrivileges(h, False, ctypes.byref(tp), 0, None, None)
    err = ctypes.get_last_error()
    return err == 0, err


k32.GetFirmwareEnvironmentVariableExW.restype = wt.BOOL
k32.SetFirmwareEnvironmentVariableExW.restype = wt.BOOL
k32.GetFirmwareEnvironmentVariableExW.argtypes = [
    wt.LPCWSTR, wt.LPCWSTR, ctypes.c_void_p, wt.DWORD, ctypes.POINTER(wt.DWORD)]
k32.SetFirmwareEnvironmentVariableExW.argtypes = [
    wt.LPCWSTR, wt.LPCWSTR, ctypes.c_void_p, wt.DWORD, wt.DWORD]


def fw_get(name, size=4096):
    buf = ctypes.create_string_buffer(size)
    attr = wt.DWORD(0)
    ok = k32.GetFirmwareEnvironmentVariableExW(name, GUID_EFI_GLOBAL, buf, size,
                                               ctypes.byref(attr))
    if not ok:
        return None, None
    return buf.raw[:size], attr.value


def fw_set(name, data, attributes=0x7):
    buf = ctypes.create_string_buffer(bytes(data), len(data))
    return k32.SetFirmwareEnvironmentVariableExW(name, GUID_EFI_GLOBAL, buf,
                                                 len(data), attributes)


def main():
    ok, err = enable_privilege()
    print("privilege SeSystemEnvironmentPrivilege:", ok, "err", err)
    if not ok:
        sys.exit(1)

    cur, _ = fw_get("BootCurrent", 2)
    print("BootCurrent:", None if cur is None else struct.unpack('<H', cur)[0])

    # find a free Boot#### slot
    used = {}
    for i in range(0x10):
        name = "Boot%04X" % i
        d, a = fw_get(name, 2048)
        if d is not None:
            used[i] = d
    print("existing Boot#### entries:", sorted("%04X" % i for i in used))
    free = next((i for i in range(0x10, 0x100) if i not in used), None)
    if free is None:
        print("no free slot")
        sys.exit(1)
    print("chosen slot: Boot%04X" % free)

    # --- device path: HD(partition 4) + FilePath(\EFI\BOOT\BOOTX64.EFI) + End
    part_start = 1996216320
    part_size = 2000408575 - 1996216320 + 1
    part_guid = bytes([0xb3, 0xc2, 0xd0, 0x1a, 0xaa, 0x37, 0xa2, 0x41,
                       0xb8, 0xc3, 0x1c, 0x92, 0x40, 0xdc, 0xa1, 0x3e])
    hd = (struct.pack('<BBH', 4, 1, 42) +
          struct.pack('<I', 4) +
          struct.pack('<Q', part_start) +
          struct.pack('<Q', part_size) +
          part_guid +
          bytes([2]) +   # MBR type = GPT
          bytes([2]))    # signature type = GUID
    filepath = sys.argv[1] if len(sys.argv) > 1 else "\\EFI\\BOOT\\BOOTX64.EFI"
    fp_utf16 = filepath.encode('utf-16-le') + b'\x00\x00'
    fp = struct.pack('<BBH', 4, 4, 4 + len(fp_utf16)) + fp_utf16
    end = struct.pack('<BBH', 0x7f, 0xff, 4)
    dp = hd + fp + end

    desc = "GU Probe (read-only)".encode('utf-16-le') + b'\x00\x00'
    load_option = struct.pack('<IH', 1, len(dp)) + desc + dp
    print("device path bytes:", len(dp), " load option bytes:", len(load_option))

    if not fw_set("Boot%04X" % free, load_option, 0x7):
        print("set Boot%04X failed, err=%d" % (free, ctypes.get_last_error()))
        sys.exit(1)
    print("Boot%04X written" % free)

    if not fw_set("BootNext", struct.pack('<H', free), 0x7):
        print("set BootNext failed, err=%d" % ctypes.get_last_error())
        sys.exit(1)
    print("BootNext = %04X (one-time firmware boot)" % free)

    d, a = fw_get("BootNext", 2)
    print("verify BootNext:", None if d is None else struct.unpack('<H', d)[0])
    d, a = fw_get("Boot%04X" % free, 2048)
    print("verify Boot%04X stored bytes:" % free, None if d is None else len(d))


if __name__ == "__main__":
    main()
