import os
import zlib
import struct
import json
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.backends import default_backend

db_path = r'C:\Users\admin\Music\Apple Music\Apple Music Library.musiclibrary\Library.musicdb'
with open(db_path, 'rb') as f:
    data = f.read()

envelopeLength = int.from_bytes(data[4:8], 'little')
maxCryptSize = int.from_bytes(data[84:88], 'little')
payloadLength = len(data) - envelopeLength
cryptSize = maxCryptSize if (0 < maxCryptSize < payloadLength) else (payloadLength // 16) * 16

key = b'BHUILuilfghuila3'
cipher = Cipher(algorithms.AES(key), modes.ECB(), backend=default_backend())
decryptor = cipher.decryptor()
decompressed = zlib.decompress(decryptor.update(data[envelopeLength:envelopeLength+cryptSize]) + decryptor.finalize() + data[envelopeLength+cryptSize:])

def parse_boma_strings(data_bytes):
    fields = {}
    pos = 0
    while pos < len(data_bytes) - 8:
        bidx = data_bytes.find(b'boma', pos)
        if bidx == -1:
            break
        if bidx + 28 > len(data_bytes):
            break
        try:
            total_len = struct.unpack('<I', data_bytes[bidx+8:bidx+12])[0]
            sub_type = struct.unpack('<I', data_bytes[bidx+12:bidx+16])[0]
            strlen = struct.unpack('<I', data_bytes[bidx+24:bidx+28])[0]
            if 0 < strlen < 2000 and bidx + 36 + strlen <= len(data_bytes):
                val = data_bytes[bidx+36:bidx+36+strlen].decode('utf-16le', errors='ignore').rstrip('\x00')
                fields[sub_type] = val
            pos = bidx + max(total_len, 28)
        except Exception:
            pos = bidx + 4
    return fields

# Find all iama (albums)
pos = 0
albums = []
while pos < len(decompressed) - 8:
    idx = decompressed.find(b'iama', pos)
    if idx == -1:
        break
    sec_len = struct.unpack('<I', decompressed[idx+8:idx+12])[0]
    sec_data = decompressed[idx:idx+sec_len]
    fields = parse_boma_strings(sec_data)
    
    title = fields.get(0x012C) or fields.get(0x0003)
    artist = fields.get(0x012D) or fields.get(0x0004) or fields.get(0x001B)
    if title:
        albums.append({
            'name': title,
            'artist': artist or '',
            'raw_fields': {hex(k): v for k, v in fields.items()}
        })
    pos = idx + max(sec_len, 4)

print(f"=== Extracted {len(albums)} Albums from Library.musicdb ===")
for i, a in enumerate(albums):
    print(f"[{i+1}] {a['name']} - {a['artist']}")
