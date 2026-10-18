import zipfile
import re
import struct

apk_path = r"D:\Workspaces\vban-receptor\app\build\outputs\apk\debug\app-debug.apk"

print("Inspecting APK:", apk_path)
with zipfile.ZipFile(apk_path, 'r') as z:
    infolist = z.infolist()
    print(f"Total entries in APK: {len(infolist)}")
    dex_files = [f.filename for f in infolist if f.filename.endswith('.dex')]
    print(f"DEX files: {dex_files}")
    
    so_files = [f.filename for f in infolist if f.filename.endswith('.so')]
    print(f"SO files: {so_files}")

    # Inspect class names inside all DEX files
    # A standard DEX header starts with 'dex\n035\0' or 'dex\n037\0' etc.
    # class_defs table is at class_defs_off, count is class_defs_size
    # string_ids table is at string_ids_off, count is string_ids_size
    # type_ids table is at type_ids_off, count is type_ids_size
    found_target_classes = {}
    target_patterns = [
        "VbanPlaybackService",
        "PowerNetworkLockManager",
        "BitPerfectController",
        "AudioDeviceManager",
        "StereoVuMeter",
        "BufferOccupancyGauge",
        "DeviceSelector",
        "DashboardScreen",
        "MainActivity",
        "NativeVbanEngine"
    ]

    for dex_name in dex_files:
        dex_data = z.read(dex_name)
        magic = dex_data[:8]
        print(f"\n{dex_name}: magic={magic}")
        if not magic.startswith(b'dex\n'):
            continue
            
        string_ids_size, string_ids_off = struct.unpack_from('<II', dex_data, 56)
        type_ids_size, type_ids_off = struct.unpack_from('<II', dex_data, 64)
        proto_ids_size, proto_ids_off = struct.unpack_from('<II', dex_data, 72)
        field_ids_size, field_ids_off = struct.unpack_from('<II', dex_data, 80)
        method_ids_size, method_ids_off = struct.unpack_from('<II', dex_data, 88)
        class_defs_size, class_defs_off = struct.unpack_from('<II', dex_data, 96)
        
        print(f"  string_ids: {string_ids_size}, type_ids: {type_ids_size}, class_defs: {class_defs_size}")
        
        # Read string offsets
        string_offsets = []
        for i in range(string_ids_size):
            off = struct.unpack_from('<I', dex_data, string_ids_off + i * 4)[0]
            string_offsets.append(off)
            
        # Helper to read dex string
        def get_string(idx):
            if idx < 0 or idx >= len(string_offsets):
                return ""
            off = string_offsets[idx]
            # uleb128 utf16_size
            # read until null byte
            p = off
            # skip uleb128
            while dex_data[p] & 0x80:
                p += 1
            p += 1
            end = dex_data.find(b'\0', p)
            return dex_data[p:end].decode('utf-8', errors='replace')

        # Read type_ids -> string_idx
        type_strings = []
        for i in range(type_ids_size):
            s_idx = struct.unpack_from('<I', dex_data, type_ids_off + i * 4)[0]
            type_strings.append(get_string(s_idx))

        # Read class_defs -> class_idx (into type_ids)
        classes_in_dex = []
        for i in range(class_defs_size):
            class_idx = struct.unpack_from('<I', dex_data, class_defs_off + i * 32)[0]
            cname = type_strings[class_idx]
            classes_in_dex.append(cname)
            
        print(f"  Total classes parsed: {len(classes_in_dex)}")
        
        # Check targets
        for pat in target_patterns:
            matches = [c for c in classes_in_dex if pat in c]
            if matches:
                found_target_classes.setdefault(pat, []).extend(matches)

    print("\nTarget Class Verification Results:")
    for pat in target_patterns:
        matches = found_target_classes.get(pat, [])
        print(f"  [{'PASS' if matches else 'FAIL'}] {pat}: {len(matches)} matches")
        for m in matches[:5]:
            print(f"      {m}")
        if len(matches) > 5:
            print(f"      ... and {len(matches)-5} more")

