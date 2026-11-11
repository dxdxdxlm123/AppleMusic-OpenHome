import os
import re

with open('tools/web_remote.html', 'rb') as f:
    html_data = f.read()

icon_data = b''
if os.path.exists('tools/icon-192.png'):
    with open('tools/icon-192.png', 'rb') as f:
        icon_data = f.read()
elif os.path.exists('mod/src/remote/WebRemoteHtml.h'):
    # Extract existing kAppIconBytes
    with open('mod/src/remote/WebRemoteHtml.h', 'r', encoding='utf-8') as f:
        content = f.read()
        m = re.search(r'kAppIconBytes\[\]\s*=\s*\{([^}]+)\}', content)
        if m:
            hexes = re.findall(r'0x([0-9a-fA-F]{2})', m.group(1))
            icon_data = bytes(int(h, 16) for h in hexes)

with open('mod/src/remote/WebRemoteHtml.h', 'w', encoding='utf-8') as f:
    f.write('#pragma once\n#include <string_view>\n\nnamespace ammod::remote {\n\n')
    
    # HTML
    f.write('inline const unsigned char kWebRemoteHtmlBytes[] = {\n')
    for i in range(0, len(html_data), 16):
        chunk = html_data[i:i+16]
        hex_str = ', '.join(f'0x{b:02x}' for b in chunk)
        f.write('    ' + hex_str + ',\n')
    f.write('};\n\n')
    f.write('inline std::string_view GetWebRemoteHtml() noexcept {\n')
    f.write(f'    return std::string_view(reinterpret_cast<const char*>(kWebRemoteHtmlBytes), {len(html_data)});\n')
    f.write('}\n\n')
    
    # Icon
    f.write('inline const unsigned char kAppIconBytes[] = {\n')
    for i in range(0, len(icon_data), 16):
        chunk = icon_data[i:i+16]
        hex_str = ', '.join(f'0x{b:02x}' for b in chunk)
        f.write('    ' + hex_str + ',\n')
    f.write('};\n\n')
    f.write('inline std::string_view GetAppIconPng() noexcept {\n')
    f.write(f'    return std::string_view(reinterpret_cast<const char*>(kAppIconBytes), {len(icon_data)});\n')
    f.write('}\n\n} // namespace ammod::remote\n')

print(f'Generated WebRemoteHtml.h with HTML ({len(html_data)} bytes) and Icon ({len(icon_data)} bytes) successfully!')
