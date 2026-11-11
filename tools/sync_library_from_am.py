import os
import zlib
import struct
import sqlite3
import json
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
from cryptography.hazmat.backends import default_backend

def sync_library_from_apple_music():
    dest_dir = r"C:\Users\Public\AMExclusive"
    os.makedirs(dest_dir, exist_ok=True)
    albums_path = os.path.join(dest_dir, "am_library_albums.json")
    songs_path = os.path.join(dest_dir, "am_library_songs.json")

    # 1. Read artwork.sqlite
    art_db = r'C:\Users\admin\Music\Apple Music\Apple Music Library.musiclibrary\artwork.sqlite'
    item_to_art = {}
    art_sources = {}

    if os.path.exists(art_db):
        try:
            conn = sqlite3.connect(f"file:{art_db}?mode=ro", uri=True)
            cur = conn.cursor()
            cur.execute("SELECT artwork_id, location FROM artwork_source WHERE location_type = 4")
            for aid, loc in cur.fetchall():
                art_sources[aid] = f"https://is1-ssl.mzstatic.com/image/thumb/{loc}/600x600bb.jpg"
            cur.execute("SELECT item_id, artwork_id FROM item_to_artwork")
            for item_id, aid in cur.fetchall():
                if aid in art_sources:
                    item_to_art[item_id] = art_sources[aid]
            conn.close()
        except Exception as e:
            print("Error reading artwork.sqlite:", e)

    # 2. Read and decrypt Library.musicdb
    db_path = r'C:\Users\admin\Music\Apple Music\Apple Music Library.musiclibrary\Library.musicdb'
    if not os.path.exists(db_path):
        print("Library.musicdb not found")
        return False

    with open(db_path, 'rb') as f:
        data = f.read()

    env = int.from_bytes(data[4:8], 'little')
    maxC = int.from_bytes(data[84:88], 'little')
    pay = len(data) - env
    cryptSize = maxC if (0 < maxC < pay) else (pay // 16) * 16

    key = b'BHUILuilfghuila3'
    cipher = Cipher(algorithms.AES(key), modes.ECB(), backend=default_backend())
    dec = zlib.decompress(cipher.decryptor().update(data[env:env+cryptSize]) + cipher.decryptor().finalize() + data[env+cryptSize:])

    def parse_bomas(b):
        fields = {}
        pos = 0
        while pos < len(b) - 8:
            bidx = b.find(b'boma', pos)
            if bidx == -1 or bidx + 28 > len(b):
                break
            try:
                total_len = struct.unpack('<I', b[bidx+8:bidx+12])[0]
                sub = struct.unpack('<I', b[bidx+12:bidx+16])[0]
                slen = struct.unpack('<I', b[bidx+24:bidx+28])[0]
                if 0 < slen < 2000 and bidx + 36 + slen <= len(b):
                    val = b[bidx+36:bidx+36+slen].decode('utf-16le', errors='ignore').rstrip('\x00')
                    fields[sub] = val
                pos = bidx + max(total_len, 28)
            except Exception:
                pos = bidx + 4
        return fields

    # Extract all tracks
    pos = 0
    tracks = []
    album_art_map = {}

    while pos < len(dec) - 8:
        idx = dec.find(b'itma', pos)
        if idx == -1:
            break
        sec_len = struct.unpack('<I', dec[idx+8:idx+12])[0]
        chunk = dec[idx:idx+sec_len]
        item_id = struct.unpack('<q', chunk[16:24])[0]
        fields = parse_bomas(chunk)
        
        title = fields.get(0x0002)
        album = fields.get(0x0003)
        artist = fields.get(0x0004) or fields.get(0x001B)
        genre = fields.get(0x0005)
        
        art = item_to_art.get(item_id, '')
        if album and art and album not in album_art_map:
            album_art_map[album] = art
            
        if title:
            tracks.append({
                'id': f'lib.song.{len(tracks)+1}',
                'name': title,
                'album': album or '',
                'artist': artist or '',
                'genre': genre or '',
                'artworkUrl': art
            })
        pos = idx + max(sec_len, 4)

    # Backfill track artworks from album
    for t in tracks:
        if not t['artworkUrl'] and t['album'] in album_art_map:
            t['artworkUrl'] = album_art_map[t['album']]

    # Extract all albums
    pos = 0
    albums = []
    while pos < len(dec) - 8:
        idx = dec.find(b'iama', pos)
        if idx == -1:
            break
        sec_len = struct.unpack('<I', dec[idx+8:idx+12])[0]
        chunk = dec[idx:idx+sec_len]
        fields = parse_bomas(chunk)
        
        title = fields.get(0x012C) or fields.get(0x0003)
        artist = fields.get(0x012D) or fields.get(0x0004) or fields.get(0x001B)
        if title:
            art = album_art_map.get(title, '')
            albums.append({
                'id': f'lib.alb.{len(albums)+1}',
                'name': title,
                'artist': artist or '',
                'artworkUrl': art
            })
        pos = idx + max(sec_len, 4)

    # Write out the authentic JSON files
    with open(albums_path, 'w', encoding='utf-8') as f:
        json.dump(albums, f, ensure_ascii=False, indent=2)

    with open(songs_path, 'w', encoding='utf-8') as f:
        json.dump(tracks, f, ensure_ascii=False, indent=2)

    print(f"Successfully synced authentic library from Library.musicdb: {len(albums)} albums, {len(tracks)} songs.")
    return True

if __name__ == '__main__':
    sync_library_from_apple_music()
