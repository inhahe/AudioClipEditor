# Builds the app's icons from the Noto Emoji "studio microphone" (noto-emoji_u1f399.svg,
# Apache 2.0, see THIRD-PARTY-NOTICES.md):
#   src/app.ico      the program: the microphone
#   src/project.ico  .acep project files: the microphone on a page, so a project
#                    doesn't look like the program in Explorer
# Each size is rendered from the vector artwork rather than shrunk from a large
# bitmap. Run after changing the artwork:  python art/make_icon.py  [preview-dir]
# Needs Pillow and skia-python (pip install pillow skia-python).
import io, os, struct, sys
import skia
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(os.path.dirname(HERE), 'src')
SVG = os.path.join(HERE, 'noto-emoji_u1f399.svg')
# What Windows asks an .ico for: title bar and small icons (16-24 at 100-150%),
# taskbar (24-48), Explorer's views up to extra large (256).
SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]

def to_pil(surface):
    arr = surface.makeImageSnapshot().toarray(colorType=skia.kRGBA_8888_ColorType,
                                               alphaType=skia.kUnpremul_AlphaType)
    return Image.fromarray(arr)   # RGBA, from its shape

def mic(px):
    """The artwork rendered px x px."""
    dom = skia.SVGDOM.MakeFromStream(skia.Stream.MakeFromFile(SVG))
    dom.setContainerSize(skia.Size(128, 128))   # its viewBox
    surface = skia.Surface(px, px)
    with surface as canvas:
        canvas.clear(skia.ColorTRANSPARENT)
        canvas.scale(px / 128, px / 128)
        dom.render(canvas)
    return to_pil(surface)

def page(s):
    """A document page with its top right corner folded over, filling an s x s icon."""
    L, R = round(s * 0.12), round(s * 0.88)
    T, B = round(s * 0.02), round(s * 0.98)
    f = max(3, round(s * 0.26))
    surface = skia.Surface(s, s)
    with surface as canvas:
        canvas.clear(skia.ColorTRANSPARENT)
        body = skia.Path()
        body.moveTo(L, T); body.lineTo(R - f, T); body.lineTo(R, T + f); body.lineTo(R, B)
        body.lineTo(L, B); body.close()
        fill = skia.Paint(AntiAlias=True)
        fill.setShader(skia.GradientShader.MakeLinear(
            points=[(0, T), (0, B)], colors=[skia.Color(255, 255, 255), skia.Color(236, 238, 243)]))
        canvas.drawPath(body, fill)
        fold = skia.Path()
        fold.moveTo(R - f, T); fold.lineTo(R - f, T + f); fold.lineTo(R, T + f); fold.close()
        canvas.drawPath(fold, skia.Paint(AntiAlias=True, Color=skia.Color(206, 209, 218)))
        # The outline on pixel centres, so it stays one crisp pixel at small sizes.
        edge = skia.Paint(AntiAlias=True, Style=skia.Paint.kStroke_Style, StrokeWidth=1.0,
                          Color=skia.Color(128, 131, 142))
        outline = skia.Path()
        outline.moveTo(L + 0.5, T + 0.5); outline.lineTo(R - f + 0.5, T + 0.5)
        outline.lineTo(R - 0.5, T + f - 0.5); outline.lineTo(R - 0.5, B - 0.5)
        outline.lineTo(L + 0.5, B - 0.5); outline.close()
        canvas.drawPath(outline, edge)
        foldEdge = skia.Path()
        foldEdge.moveTo(R - f + 0.5, T + 0.5); foldEdge.lineTo(R - f + 0.5, T + f - 0.5)
        foldEdge.lineTo(R - 0.5, T + f - 0.5)
        canvas.drawPath(foldEdge, edge)
    return to_pil(surface), (L, T, R, B)

def project(s):
    img, (L, T, R, B) = page(s)
    # Larger on the small sizes, where the page's edges take a bigger share.
    m = round(s * (0.66 if s >= 32 else 0.76))
    art = mic(m)
    x = (L + R - m) // 2
    y = B - m - max(1, round(s * 0.04))
    img.alpha_composite(art, (x, y))
    return img

def write_ico(path, images):
    """An .ico of `images` (size -> RGBA image): 32-bit bitmaps below 256, which every
    loader reads, and PNG at 256, as Windows expects there."""
    blobs = []
    for s in sorted(images):
        im = images[s]
        if s >= 256:
            buf = io.BytesIO(); im.save(buf, 'PNG'); blobs.append((s, buf.getvalue()))
            continue
        px = im.tobytes('raw', 'BGRA')
        rows = [px[y * s * 4:(y + 1) * s * 4] for y in range(s)]
        xor = b''.join(reversed(rows))   # bottom-up
        mask_row = ((s + 31) // 32) * 4
        alpha = im.getchannel('A').tobytes()
        mask = bytearray()
        for y in reversed(range(s)):
            row = bytearray(mask_row)
            for x in range(s):
                if alpha[y * s + x] == 0:
                    row[x // 8] |= 0x80 >> (x % 8)
            mask += row
        header = struct.pack('<IiiHHIIiiII', 40, s, s * 2, 1, 32, 0, len(xor) + len(mask), 0, 0, 0, 0)
        blobs.append((s, header + xor + bytes(mask)))
    out = struct.pack('<HHH', 0, 1, len(blobs))
    offset = 6 + 16 * len(blobs)
    entries = b''
    for s, data in blobs:
        dim = 0 if s >= 256 else s
        entries += struct.pack('<BBBBHHII', dim, dim, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    with open(path, 'wb') as f:
        f.write(out + entries + b''.join(data for _, data in blobs))
    print('wrote', path, os.path.getsize(path), 'bytes')

app = {s: mic(s) for s in SIZES}
doc = {s: project(s) for s in SIZES}
write_ico(os.path.join(SRC, 'app.ico'), app)
write_ico(os.path.join(SRC, 'project.ico'), doc)

if len(sys.argv) > 1:   # a preview of both, every size, on dark and light
    pv = sys.argv[1]
    os.makedirs(pv, exist_ok=True)
    W = sum(SIZES) + 12 * len(SIZES) + 24
    sheet = Image.new('RGBA', (W, 2 * (256 + 24) * 2), (255, 255, 255, 255))
    y = 0
    for icons in (app, doc):
        for bg in ((28, 28, 32, 255), (255, 255, 255, 255)):
            strip = Image.new('RGBA', (W, 256 + 24), bg)
            x = 12
            for s in SIZES:
                strip.alpha_composite(icons[s], (x, 12 + 256 - s))
                x += s + 12
            sheet.alpha_composite(strip, (0, y))
            y += 256 + 24
    sheet.save(os.path.join(pv, 'icons.png'))
    zoom = Image.new('RGBA', (2 * (16 + 24 + 32) * 4 + 80, 2 * 32 * 4 + 60), (245, 245, 247, 255))
    xs = 10
    for row, bg in enumerate(((28, 28, 32, 255), (255, 255, 255, 255))):
        x = xs
        for icons in (app, doc):
            for s in (16, 24, 32):
                tile = Image.new('RGBA', (s, s), bg); tile.alpha_composite(icons[s])
                zoom.alpha_composite(tile.resize((s * 4, s * 4), Image.NEAREST),
                                     (x, 20 + row * (32 * 4 + 20) + (32 - s) * 4))
                x += s * 4 + 10
            x += 20
    zoom.save(os.path.join(pv, 'small-enlarged.png'))
    print('previews in', pv)
