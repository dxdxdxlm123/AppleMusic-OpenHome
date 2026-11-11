with open('tools/web_remote.html', 'rb') as f:
    data = f.read()

with open('mod/src/remote/WebRemoteHtml.h', 'w', encoding='utf-8') as f:
    f.write('#pragma once\n#include <string_view>\n\nnamespace ammod::remote {\n\n')
    f.write('inline const unsigned char kWebRemoteHtmlBytes[] = {\n')
    for i in range(0, len(data), 16):
        chunk = data[i:i+16]
        hex_str = ', '.join(f'0x{b:02x}' for b in chunk)
        f.write('    ' + hex_str + ',\n')
    f.write('};\n\n')
    f.write('inline std::string_view GetWebRemoteHtml() noexcept {\n')
    f.write(f'    return std::string_view(reinterpret_cast<const char*>(kWebRemoteHtmlBytes), {len(data)});\n')
    f.write('}\n\n} // namespace ammod::remote\n')

print(f'Generated WebRemoteHtml.h with {len(data)} bytes successfully!')
