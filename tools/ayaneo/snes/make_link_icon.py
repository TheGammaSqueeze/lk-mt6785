"""make_link_icon.py - the Link icon of the Classic themes' menubar (btn_link.png): 24x24, one
handheld with waves coming out of it, drawn in each theme's palette (outline, body, shading, the
display icon's screen bands, waves, shadow) and written into the theme atlases at the same place
(the sprite table packed.json is shared, the themes only change pixels).

    python3 make_link_icon.py          writes assets/sprites/packed.png, themes/*/packed.png, and the
                                       sprite's entries in packed.json and assets/dependencies.json
"""
import math
from PIL import Image

def draw(p):
    W = H = 24
    g = [['.'] * W for _ in range(H)]
    def put(x, y, c):
        if 0 <= x < W and 0 <= y < H: g[y][x] = c
    # the handheld: 11 wide, 17 tall, rounded top corners like the display icon's frame
    x0, y0, x1, y1 = 2, 4, 12, 20
    for y in range(y0, y1 + 1):
        for x in range(x0, x1 + 1):
            edge = x in (x0, x1) or y in (y0, y1)
            put(x, y, 'k' if edge else 'w')
    put(x0, y0, '.'); put(x1, y0, '.'); put(x0 + 1, y0 + 1, 'k'); put(x1 - 1, y0 + 1, 'k')
    for x in range(x0 + 1, x1): put(x, y1 - 1, 'g')               # shaded underside
    for x in range(x1 - 1, x1): 
        for y in range(y0 + 2, y1 - 1): put(x, y, 'g')             # shaded right side
    # the screen: framed, with the display icon's blue bands
    sx0, sy0, sx1, sy1 = x0 + 2, y0 + 2, x1 - 2, y0 + 8
    for y in range(sy0, sy1 + 1):
        for x in range(sx0, sx1 + 1):
            if x in (sx0, sx1) or y in (sy0, sy1): put(x, y, 'k')
            else:
                band = ['f', 'f', 'S', 'h', 'i'][min(4, y - sy0 - 1)]
                put(x, y, band)
    put(sx0 + 1, sy0 + 1, 'L'); put(sx0 + 2, sy0 + 1, 'L')         # a glint
    # d-pad and two buttons
    for x in range(x0 + 2, x0 + 5): put(x, y0 + 12, 'k')
    for y in range(y0 + 11, y0 + 14): put(x0 + 3, y, 'k')
    put(x1 - 3, y0 + 13, 'k'); put(x1 - 2, y0 + 11, 'k')
    # the waves: two arcs out of its top right, blue edged in black
    cx, cy = 12, 7
    for r in (4, 7):
        pts = set()
        for a in range(-70, 25, 2):
            t = math.radians(a)
            pts.add((round(cx + r * math.cos(t)), round(cy + r * math.sin(t))))
        for (x, y) in pts: put(x, y, 'i')
    # outline the waves
    for y in range(H):
        for x in range(W):
            if g[y][x] == '.':
                for dx, dy in ((1,0),(-1,0),(0,1),(0,-1)):
                    xx, yy = x + dx, y + dy
                    if 0 <= xx < W and 0 <= yy < H and g[yy][xx] == 'i' and xx > x1:
                        g[y][x] = 'K'; break
    img = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    for y in range(H):
        for x in range(W):
            c = g[y][x]
            if c != '.': img.putpixel((x, y), p['k' if c == 'K' else c])
    for y in range(H - 1, 0, -1):
        for x in range(W - 1, 0, -1):
            if g[y][x] == '.' and g[y - 1][x - 1] in ('k', 'K'):
                img.putpixel((x, y), p['s'])
    return img
CLASSIC = {'k': (0,0,0,255), 'w': (255,255,255,255), 'g': (176,176,176,255), 'f': (0,42,105,255), 'S': (0,60,149,255),
           'h': (27,85,170,255), 'i': (53,115,206,255), 'L': (96,160,255,255), 's': (0,0,0,92)}
INDIGO = {'k': (41,36,74,255), 'w': (231,231,239,255), 'g': (156,154,173,255), 'f': (57,52,82,255), 'S': (90,85,140,255),
          'h': (107,105,165,255), 'i': (123,117,181,255), 'L': (189,186,214,255), 's': (24,24,41,92)}

# where it goes in every atlas (free in each: checked when this was added)
AT = (309, 809)
THEMES = {'assets/sprites/packed.png': CLASSIC, 'themes/gba-indigo/packed.png': INDIGO}

if __name__ == '__main__':
    import json, os
    here = os.path.dirname(os.path.abspath(__file__))
    for rel, pal in THEMES.items():
        path = os.path.join(here, rel)
        atlas = Image.open(path).convert('RGBA')
        icon = draw(pal)
        clear = Image.new('RGBA', (24, 24), (0, 0, 0, 0))
        atlas.paste(clear, AT)
        atlas.paste(icon, AT)
        atlas.save(path)
    jp = os.path.join(here, 'assets/sprites/packed.json')
    j = json.load(open(jp))
    j['sprites']['btn_link.png'] = {'textureFrame': [AT[0], AT[1], 24, 24], 'pivot': [12, 12],
                                    'sourceSize': [24, 24], 'sourceOffset': [0, 0]}
    # the packer takes sprites from the asset tree's resource list
    dp = os.path.join(here, 'assets/dependencies.json')
    d = json.load(open(dp))
    sid = 'ff9fb46c-ad68-46f6-9e87-7108e2080773.btn_link.png'
    d['resources'] = [r for r in d['resources'] if r['id'] != sid]
    d['resources'].append({'id': sid, 'type': 'SpriteResource',
                           'dependencies': [{'id': 'ff9fb46c-ad68-46f6-9e87-7108e2080773', 'refCount': 1}],
                           'content': json.dumps({'textureId': 'ff9fb46c-ad68-46f6-9e87-7108e2080773',
                                                  'textureFrame': [AT[0], AT[1], 24, 24], 'pivot': [12, 12]},
                                                 separators=(',', ':')),
                           'contentType': 1, 'groupId': '114'})
    with open(dp, 'w') as f:
        json.dump(d, f, separators=(',', ':'), ensure_ascii=False)
    with open(jp, 'w') as f:
        json.dump(j, f, separators=(',', ':'))   # the file's own compact form
