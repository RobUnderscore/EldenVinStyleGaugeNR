"""
Cross-platform game memory reader for Elden Ring Nightreign.
Works on both Windows (ReadProcessMemory) and Linux (reading /proc/PID/mem).
"""

import struct
import os
import sys
import re
from pathlib import Path


class MemoryReader:
    """Read memory from the Nightreign process."""

    def __init__(self):
        self.pid = None
        self.process_handle = None  # Windows only
        self.mem_file = None  # Linux only
        self.is_windows = sys.platform == "win32"
        self.world_chr_man_ptr = 0  # Address of the WorldChrMan pointer

    def attach(self) -> bool:
        """Find and attach to the nightreign.exe process."""
        self.pid = self._find_process()
        if not self.pid:
            return False

        if self.is_windows:
            return self._attach_windows()
        else:
            return self._attach_linux()

    def detach(self):
        if self.is_windows and self.process_handle:
            import ctypes
            ctypes.windll.kernel32.CloseHandle(self.process_handle)
            self.process_handle = None
        if self.mem_file:
            self.mem_file.close()
            self.mem_file = None

    def read_u64(self, addr: int) -> int:
        data = self._read_bytes(addr, 8)
        if data is None or len(data) < 8:
            return 0
        return struct.unpack("<Q", data)[0]

    def read_i32(self, addr: int) -> int:
        data = self._read_bytes(addr, 4)
        if data is None or len(data) < 4:
            return 0
        return struct.unpack("<i", data)[0]

    def read_f32(self, addr: int) -> float:
        data = self._read_bytes(addr, 4)
        if data is None or len(data) < 4:
            return 0.0
        return struct.unpack("<f", data)[0]

    def read_ptr(self, addr: int) -> int:
        return self.read_u64(addr)

    def find_world_chr_man(self) -> bool:
        """Find WorldChrMan via AOB scan of the game module."""
        # Pattern: 48 8B 05 ?? ?? ?? ?? 0F 28 F1 48 85 C0
        pattern = bytes([0x48, 0x8B, 0x05])
        suffix = bytes([0x0F, 0x28, 0xF1, 0x48, 0x85, 0xC0])

        base, size = self._get_module_info()
        if base == 0:
            return False

        # Read the module in chunks and scan for the pattern
        chunk_size = 0x100000  # 1MB chunks
        for offset in range(0, size, chunk_size):
            read_size = min(chunk_size + 16, size - offset)  # overlap for boundary matches
            data = self._read_bytes(base + offset, read_size)
            if data is None:
                continue

            # Search for pattern
            pos = 0
            while True:
                idx = data.find(pattern, pos)
                if idx == -1:
                    break
                # Check suffix at idx+7
                if idx + 13 <= len(data) and data[idx + 7:idx + 13] == suffix:
                    # Found it! Resolve RIP-relative address
                    rip_offset = struct.unpack("<i", data[idx + 3:idx + 7])[0]
                    abs_addr = (base + offset + idx) + 7 + rip_offset
                    self.world_chr_man_ptr = abs_addr
                    return True
                pos = idx + 1

        return False

    def get_world_chr_man(self) -> int:
        if self.world_chr_man_ptr == 0:
            return 0
        return self.read_ptr(self.world_chr_man_ptr)

    def get_chr_ins(self) -> int:
        wcm = self.get_world_chr_man()
        if wcm == 0:
            return 0
        return self.read_ptr(wcm + 0x174e8)

    def get_sp_effect_ctrl(self) -> int:
        chr_ins = self.get_chr_ins()
        if chr_ins == 0:
            return 0
        return self.read_ptr(chr_ins + 0x190)

    def get_sp_effect_list_head(self) -> int:
        ctrl = self.get_sp_effect_ctrl()
        if ctrl == 0:
            return 0
        return self.read_ptr(ctrl + 0x08)

    # --- Platform-specific implementations ---

    def _find_process(self) -> int:
        """Find nightreign.exe PID."""
        if self.is_windows:
            return self._find_process_windows()
        else:
            return self._find_process_linux()

    def _find_process_windows(self) -> int:
        import ctypes
        from ctypes import wintypes

        PROCESS_QUERY_INFORMATION = 0x0400
        PROCESS_VM_READ = 0x0010
        MAX_PATH = 260

        kernel32 = ctypes.windll.kernel32
        psapi = ctypes.windll.psapi

        pids = (ctypes.c_ulong * 4096)()
        bytes_returned = ctypes.c_ulong()
        psapi.EnumProcesses(pids, ctypes.sizeof(pids), ctypes.byref(bytes_returned))

        for i in range(bytes_returned.value // ctypes.sizeof(ctypes.c_ulong)):
            pid = pids[i]
            if pid == 0:
                continue
            handle = kernel32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
            if handle:
                name = (ctypes.c_char * MAX_PATH)()
                psapi.GetModuleBaseNameA(handle, None, name, MAX_PATH)
                if name.value.lower() == b"nightreign.exe":
                    kernel32.CloseHandle(handle)
                    return pid
                kernel32.CloseHandle(handle)
        return 0

    def _find_process_linux(self) -> int:
        for entry in os.listdir("/proc"):
            if not entry.isdigit():
                continue
            try:
                cmdline = Path(f"/proc/{entry}/cmdline").read_bytes()
                if b"nightreign.exe" in cmdline.lower():
                    return int(entry)
            except (PermissionError, FileNotFoundError, ProcessLookupError):
                continue
        return 0

    def _attach_windows(self) -> bool:
        import ctypes
        PROCESS_VM_READ = 0x0010
        PROCESS_QUERY_INFORMATION = 0x0400
        self.process_handle = ctypes.windll.kernel32.OpenProcess(
            PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, False, self.pid
        )
        return self.process_handle is not None and self.process_handle != 0

    def _attach_linux(self) -> bool:
        try:
            self.mem_file = open(f"/proc/{self.pid}/mem", "rb")
            return True
        except (PermissionError, FileNotFoundError):
            return False

    def _read_bytes(self, addr: int, size: int) -> bytes:
        if addr == 0:
            return None
        try:
            if self.is_windows:
                return self._read_bytes_windows(addr, size)
            else:
                return self._read_bytes_linux(addr, size)
        except Exception:
            return None

    def _read_bytes_windows(self, addr: int, size: int) -> bytes:
        import ctypes
        buf = (ctypes.c_char * size)()
        bytes_read = ctypes.c_size_t()
        result = ctypes.windll.kernel32.ReadProcessMemory(
            self.process_handle, ctypes.c_void_p(addr),
            buf, size, ctypes.byref(bytes_read)
        )
        if result == 0:
            return None
        return bytes(buf[:bytes_read.value])

    def _read_bytes_linux(self, addr: int, size: int) -> bytes:
        try:
            self.mem_file.seek(addr)
            return self.mem_file.read(size)
        except (OSError, ValueError):
            return None

    def _get_module_info(self) -> tuple:
        """Get base address and size of nightreign.exe module."""
        if self.is_windows:
            return self._get_module_info_windows()
        else:
            return self._get_module_info_linux()

    def _get_module_info_windows(self) -> tuple:
        import ctypes
        from ctypes import wintypes

        psapi = ctypes.windll.psapi
        kernel32 = ctypes.windll.kernel32

        hModules = (ctypes.c_void_p * 1024)()
        cbNeeded = ctypes.c_ulong()
        psapi.EnumProcessModulesEx(
            self.process_handle, hModules, ctypes.sizeof(hModules),
            ctypes.byref(cbNeeded), 0x03
        )

        for i in range(cbNeeded.value // ctypes.sizeof(ctypes.c_void_p)):
            name = (ctypes.c_char * 260)()
            psapi.GetModuleBaseNameA(self.process_handle, hModules[i], name, 260)
            if name.value.lower() == b"nightreign.exe":
                class MODULEINFO(ctypes.Structure):
                    _fields_ = [
                        ("lpBaseOfDll", ctypes.c_void_p),
                        ("SizeOfImage", ctypes.c_ulong),
                        ("EntryPoint", ctypes.c_void_p),
                    ]
                info = MODULEINFO()
                psapi.GetModuleInformation(
                    self.process_handle, hModules[i],
                    ctypes.byref(info), ctypes.sizeof(info)
                )
                return (info.lpBaseOfDll, info.SizeOfImage)
        return (0, 0)

    def _get_module_info_linux(self) -> tuple:
        """Parse /proc/PID/maps to find nightreign.exe base and size."""
        try:
            maps = Path(f"/proc/{self.pid}/maps").read_text()
        except (PermissionError, FileNotFoundError):
            return (0, 0)

        base = 0
        end = 0
        for line in maps.splitlines():
            if "nightreign.exe" in line.lower() or "nightreign" in line.lower():
                parts = line.split()
                addr_range = parts[0].split("-")
                start = int(addr_range[0], 16)
                stop = int(addr_range[1], 16)
                if base == 0 or start < base:
                    base = start
                if stop > end:
                    end = stop

        if base == 0:
            return (0, 0)
        return (base, end - base)
