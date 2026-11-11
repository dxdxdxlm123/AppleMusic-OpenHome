from PIL import Image, ImageDraw

def create_icon(size, out_path):
    img = Image.new('RGBA', (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    
    # Draw rounded rectangle with Apple Music pink/red gradient
    radius = int(size * 0.22)
    # Background gradient approximation
    for y in range(size):
        ratio = y / size
        # #fa2d48 to #a81d33
        r = int(250 * (1 - ratio*0.3))
        g = int(45 * (1 - ratio*0.3))
        b = int(72 * (1 - ratio*0.2))
        draw.line([(0, y), (size, y)], fill=(r, g, b, 255))
        
    # Apply rounded corners mask
    mask = Image.new('L', (size, size), 0)
    mask_draw = ImageDraw.Draw(mask)
    mask_draw.rounded_rectangle([(0, 0), (size, size)], radius=radius, fill=255)
    img.putalpha(mask)
    
    # Draw simple double eighth note (music note) in white
    draw = ImageDraw.Draw(img)
    scale = size / 200.0
    
    # Left note head
    cx1, cy1 = int(60 * scale), int(135 * scale)
    r1 = int(18 * scale)
    draw.ellipse([cx1 - r1, cy1 - int(r1*0.7), cx1 + r1, cy1 + int(r1*0.7)], fill=(255, 255, 255, 255))
    
    # Right note head
    cx2, cy2 = int(130 * scale), int(115 * scale)
    draw.ellipse([cx2 - r1, cy2 - int(r1*0.7), cx2 + r1, cy2 + int(r1*0.7)], fill=(255, 255, 255, 255))
    
    # Vertical stems
    stem_w = max(2, int(7 * scale))
    draw.rectangle([cx1 + r1 - stem_w, int(55 * scale), cx1 + r1, cy1], fill=(255, 255, 255, 255))
    draw.rectangle([cx2 + r1 - stem_w, int(35 * scale), cx2 + r1, cy2], fill=(255, 255, 255, 255))
    
    # Beam
    beam_h = max(2, int(14 * scale))
    draw.polygon([
        (cx1 + r1 - stem_w, int(55 * scale)),
        (cx2 + r1, int(35 * scale)),
        (cx2 + r1, int(35 * scale) + beam_h),
        (cx1 + r1 - stem_w, int(55 * scale) + beam_h)
    ], fill=(255, 255, 255, 255))
    
    img.save(out_path, 'PNG')
    print(f'Generated {out_path} ({size}x{size})')

create_icon(192, 'tools/icon-192.png')
create_icon(512, 'tools/icon-512.png')
