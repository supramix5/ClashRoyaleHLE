// CoreGraphics bitmap contexts and the CoreText subset used by the engine's text module (nti_coretext.c): fonts from
// TTF data (stb_truetype), cascade lists for fallback, CTTypesetter line breaking, lines/runs/glyph positions, and
// CTFontDrawGlyphs into bitmap contexts. CG coordinates are y-up from the bottom-left; bitmap rows are top-first.
#define STB_TRUETYPE_IMPLEMENTATION
#include "platform/Platform.h"
#include "../../external/stb/stb_truetype.h"
#include "foundation/Foundation.h"
#include "Log.h"
#include "Memory.h"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>

using namespace ns;
using hle::arg;
using hle::ret;

namespace {
struct Face {
    std::vector<unsigned char> bytes;
    stbtt_fontinfo info{};
    std::string family;
    bool bold = false;
    bool init() {
        if (!stbtt_InitFont(&info, bytes.data(), stbtt_GetFontOffsetForIndex(bytes.data(), 0))) return false;
        int length = 0;
        const char *name = stbtt_GetFontNameString(&info, &length, STBTT_PLATFORM_ID_MICROSOFT, STBTT_MS_EID_UNICODE_BMP, STBTT_MS_LANG_ENGLISH, 1);
        for (int i = 0; name && i + 1 < length; i += 2) family.push_back(char(name[i + 1]));
        return true;
    }
};
std::mutex g_lock;
std::shared_ptr<Face> systemFace(const std::string &name) {
    static std::map<std::string, std::shared_ptr<Face>> cache;
    const bool bold = name.find("Bold") != std::string::npos;
    // Arial on Windows; the closest metric-compatible fonts on Linux (Liberation Sans, then DejaVu / Noto / FreeSans).
    const std::string file = bold ? platform::findFont({"arialbd.ttf", "LiberationSans-Bold.ttf", "DejaVuSans-Bold.ttf", "NotoSans-Bold.ttf", "FreeSansBold.ttf"})
                                  : platform::findFont({"arial.ttf", "LiberationSans-Regular.ttf", "DejaVuSans.ttf", "NotoSans-Regular.ttf", "FreeSans.ttf"});
    auto &f = cache[file];
    if (!f) {
        f = std::make_shared<Face>();
        std::ifstream in(std::filesystem::u8path(file), std::ios::binary);
        f->bytes.assign(std::istreambuf_iterator<char>(in), {});
        f->bold = bold;
        if (f->bytes.empty() || !f->init()) f.reset();
    }
    return f;
}

struct CGFontData : objc::HostObject { std::shared_ptr<Face> face; };
struct DataProviderData : objc::HostObject { std::vector<unsigned char> bytes; };
struct FontData : objc::HostObject {
    std::shared_ptr<Face> face;
    double size = 12;
    std::vector<id> cascade; // CTFonts tried for characters the face lacks
    float scale() const { return face ? stbtt_ScaleForMappingEmToPixels(&face->info, float(size)) : 1.f; }
};
struct DescriptorData : objc::HostObject {
    id font = 0; // the font it describes (for copies at other sizes)
    std::vector<id> cascade;
};
struct AttributedData : objc::HostObject { id string = 0, attributes = 0; };
struct Run {
    id font = 0;
    std::vector<std::uint16_t> glyphs;
    std::vector<double> x, advances;
    std::vector<std::int64_t> indices;
};
struct LineData : objc::HostObject {
    std::int64_t location = 0, length = 0;
    std::vector<Run> runs;
    id runObjects = 0;
    double width = 0, ascent = 0, descent = 0, leading = 0;
};
struct RunData : objc::HostObject { Run run; id attributes = 0; };
struct TypesetterData : objc::HostObject { std::u16string text; id font = 0; };
struct ContextData : objc::HostObject {
    std::uint64_t data = 0;
    int width = 0, height = 0, bytesPerRow = 0;
    double fill[4] = {0, 0, 0, 1};
};

id makeObject(const char *cls, std::unique_ptr<objc::HostObject> data) {
    const auto o = objc::allocInstance(objc::getClass(cls));
    objc::setHostObject(o, std::move(data));
    return o;
}
FontData *font(id f) { return objc::host<FontData>(f); }
id makeFont(std::shared_ptr<Face> face, double size, std::vector<id> cascade) {
    auto d = std::make_unique<FontData>();
    d->face = std::move(face);
    d->size = size;
    d->cascade = std::move(cascade);
    for (auto c : d->cascade) objc::retain(c);
    return makeObject("CTFont", std::move(d));
}
std::vector<id> cascadeFrom(id attributes) {
    std::vector<id> out;
    const auto list = dictGet(attributes, string("kCTFontCascadeListAttribute"));
    for (auto descriptor : arrayItems(list))
        if (auto *d = objc::host<DescriptorData>(descriptor); d && d->font) out.push_back(d->font);
    return out;
}
// The font (primary or a cascade entry) with a glyph for c, and the glyph.
id fontFor(id primary, char16_t c, std::uint16_t &glyph) {
    auto *f = font(primary);
    if (f && f->face) {
        glyph = std::uint16_t(stbtt_FindGlyphIndex(&f->face->info, c));
        if (glyph || c == u' ' || c == u'\n') return primary;
        for (auto alt : f->cascade)
            if (auto *a = font(alt); a && a->face)
                if (auto g = stbtt_FindGlyphIndex(&a->face->info, c)) {
                    glyph = std::uint16_t(g);
                    // The fallback at the primary size.
                    return a->size == f->size ? alt : makeFont(a->face, f->size, {});
                }
    }
    glyph = 0;
    return primary;
}
double advanceOf(id f, std::uint16_t glyph) {
    auto *d = font(f);
    if (!d || !d->face) return 0;
    int advance = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(&d->face->info, glyph, &advance, &lsb);
    return advance * d->scale();
}
void metrics(id f, double &ascent, double &descent, double &leading) {
    auto *d = font(f);
    if (!d || !d->face) { ascent = descent = leading = 0; return; }
    int a, de, l;
    stbtt_GetFontVMetrics(&d->face->info, &a, &de, &l);
    ascent = a * d->scale();
    descent = -de * d->scale();
    leading = l * d->scale();
}
// Lays out text[location, location+length) into runs split by font.
void layout(TypesetterData &ts, std::int64_t location, std::int64_t length, LineData &line) {
    line.location = location;
    line.length = length;
    double x = 0;
    Run *current = nullptr;
    std::uint16_t previousGlyph = 0;
    id previousFont = 0;
    double a, de, le;
    metrics(ts.font, a, de, le);
    line.ascent = a;
    line.descent = de;
    line.leading = le;
    for (std::int64_t i = location; i < location + length; ++i) {
        const char16_t c = ts.text[std::size_t(i)];
        if (c == u'\n' || c == u'\r' || c == 0x85 || c == 0x2028 || c == 0x2029) continue;
        std::uint16_t glyph;
        const id f = fontFor(ts.font, c, glyph);
        if (!current || current->font != f) {
            line.runs.push_back({});
            current = &line.runs.back();
            current->font = f;
            double ra, rd, rl;
            metrics(f, ra, rd, rl);
            line.ascent = std::max(line.ascent, ra);
            line.descent = std::max(line.descent, rd);
        }
        if (previousFont == f && previousGlyph)
            if (auto *fd = font(f); fd && fd->face) x += stbtt_GetGlyphKernAdvance(&fd->face->info, previousGlyph, glyph) * fd->scale();
        const double adv = advanceOf(f, glyph);
        current->glyphs.push_back(glyph);
        current->x.push_back(x);
        current->advances.push_back(adv);
        current->indices.push_back(i);
        x += adv;
        previousGlyph = glyph;
        previousFont = f;
    }
    line.width = x;
    // Trailing whitespace does not count toward the typographic width.
    for (std::int64_t i = location + length - 1; i >= location; --i) {
        const char16_t c = ts.text[std::size_t(i)];
        if (c != u' ' && c != u'\n' && c != u'\r' && c != u'\t') break;
        if (c == u' ' || c == u'\t') {
            std::uint16_t g;
            const id f = fontFor(ts.font, c, g);
            line.width -= advanceOf(f, g);
        }
    }
    std::vector<id> runs;
    for (auto &r : line.runs) {
        auto rd = std::make_unique<RunData>();
        rd->run = r;
        rd->attributes = dictionary({{string("kCTFontAttributeName"), r.font}});
        objc::retain(rd->attributes);
        runs.push_back(makeObject("CTRun", std::move(rd)));
    }
    line.runObjects = array(runs);
    objc::retain(line.runObjects);
}
void blendGlyph(ContextData &c, id f, std::uint16_t glyph, double px, double py) {
    auto *d = font(f);
    if (!d || !d->face || !c.data) return;
    const float s = d->scale();
    const double top = double(c.height) - py; // baseline in top-down rows
    const double ox = std::floor(px), oy = std::floor(top);
    const float fx = float(px - ox), fy = float(top - oy);
    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBoxSubpixel(&d->face->info, glyph, s, s, fx, fy, &x0, &y0, &x1, &y1);
    const int w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0) return;
    std::vector<unsigned char> coverage(std::size_t(w * h));
    stbtt_MakeGlyphBitmapSubpixel(&d->face->info, coverage.data(), w, h, w, s, s, fx, fy, glyph);
    auto *pixels = mem::base() + c.data;
    for (int j = 0; j < h; ++j) {
        const int row = int(oy) + y0 + j;
        if (row < 0 || row >= c.height) continue;
        for (int i = 0; i < w; ++i) {
            const int col = int(ox) + x0 + i;
            if (col < 0 || col >= c.width) continue;
            const float cov = coverage[std::size_t(j * w + i)] / 255.f;
            if (cov <= 0) continue;
            auto *p = pixels + std::size_t(row) * std::size_t(c.bytesPerRow) + std::size_t(col) * 4;
            for (int k = 0; k < 4; ++k) {
                const float src = float(c.fill[k] * c.fill[3]) * 255.f;
                const float value = (k == 3 ? float(c.fill[3]) * 255.f : src) * cov + p[k] * (1 - cov);
                p[k] = std::uint8_t(std::min(255.f, value + 0.5f));
            }
        }
    }
}
void retRect(double x, double y, double w, double h) { cpu::setD(0, x); cpu::setD(1, y); cpu::setD(2, w); cpu::setD(3, h); }
} // namespace

OBJC_CLASS(CGColorSpace, NSObject);
OBJC_CLASS(CGContext, NSObject);
OBJC_CLASS(CGDataProvider, NSObject);
OBJC_CLASS(CGFont, NSObject);
OBJC_CLASS(CTFont, NSObject);
OBJC_CLASS(CTFontDescriptor, NSObject);
OBJC_CLASS(NSAttributedString, NSObject);
OBJC_CLASS(NSMutableAttributedString, NSAttributedString);
OBJC_CLASS(CTTypesetter, NSObject);
OBJC_CLASS(CTLine, NSObject);
OBJC_CLASS(CTRun, NSObject);
OBJC_CLASS(CGColor, NSObject);
OBJC_METHOD(CTFont, "isEqual:", isEqual) {
    auto *a = font(objc::self()), *b = font(objc::marg(0));
    ret(a && b && a->face == b->face && a->size == b->size);
}
OBJC_METHOD(CTFont, "hash", hash) {
    auto *a = font(objc::self());
    ret(a ? (std::uint64_t(reinterpret_cast<std::uintptr_t>(a->face.get())) ^ std::uint64_t(a->size * 64)) : 0);
}

// ---------------- CoreGraphics ----------------
HLE(CGColorSpaceCreateDeviceRGB) { ret(objc::allocInstance(objc::getClass("CGColorSpace"))); }
HLE(CGColorSpaceCreateDeviceGray) { ret(objc::allocInstance(objc::getClass("CGColorSpace"))); }
HLE(CGColorSpaceRelease) {}
HLE(CGColorCreate) {
    // CGColorCreate(space, components[4] CGFloat)
    const auto c = objc::allocInstance(objc::getClass("CGColor"));
    return ret(c);
}
HLE(CGColorRelease) {}
HLE(CGColorGetComponents) { static std::uint64_t white = [] { auto p = mem::hleAlloc(32); for (int i = 0; i < 4; ++i) mem::write<double>(p + 8 * i, 1.0); return p; }(); ret(white); }
// CGBitmapContextCreate(data, width, height, bitsPerComponent, bytesPerRow, space, bitmapInfo)
HLE(CGBitmapContextCreate) {
    auto d = std::make_unique<ContextData>();
    d->width = int(arg(1));
    d->height = int(arg(2));
    d->bytesPerRow = int(arg(4)) ? int(arg(4)) : int(arg(1)) * 4;
    d->data = arg(0) ? arg(0) : mem::calloc(std::size_t(d->bytesPerRow) * std::size_t(d->height));
    hlog("cg", "bitmap context %dx%d (%d bpc, row %d, info %llu)", d->width, d->height, int(arg(3)), d->bytesPerRow, (unsigned long long)arg(6));
    ret(makeObject("CGContext", std::move(d)));
}
HLE(CGBitmapContextGetData) { auto *c = objc::host<ContextData>(arg(0)); ret(c ? c->data : 0); }
HLE(CGBitmapContextGetWidth) { auto *c = objc::host<ContextData>(arg(0)); ret(c ? std::uint64_t(c->width) : 0); }
HLE(CGBitmapContextGetHeight) { auto *c = objc::host<ContextData>(arg(0)); ret(c ? std::uint64_t(c->height) : 0); }
HLE(CGContextRelease) { objc::release(arg(0)); }
HLE(CGContextSetAllowsAntialiasing) {}
HLE(CGContextSetShouldAntialias) {}
HLE(CGContextSetAllowsFontSubpixelPositioning) {}
HLE(CGContextSetShouldSubpixelQuantizeFonts) {}
HLE(CGContextSetTextMatrix) {}
HLE(CGContextSetTextPosition) {}
HLE(CGContextSetFillColor) {
    if (auto *c = objc::host<ContextData>(arg(0)))
        for (int i = 0; i < 4; ++i) c->fill[i] = mem::read<double>(arg(1) + 8 * std::uint64_t(i));
}
HLE(CGContextSetRGBFillColor) {
    if (auto *c = objc::host<ContextData>(arg(0))) for (int i = 0; i < 4; ++i) c->fill[i] = cpu::d(i);
}
HLE(CGContextSetGrayFillColor) {
    if (auto *c = objc::host<ContextData>(arg(0))) { c->fill[0] = c->fill[1] = c->fill[2] = cpu::d(0); c->fill[3] = cpu::d(1); }
}
HLE(CGContextClearRect) {
    auto *c = objc::host<ContextData>(arg(0));
    if (!c) return;
    const int x0 = std::max(0, int(cpu::d(0))), x1 = std::min(c->width, int(cpu::d(0) + cpu::d(2)));
    const int yTop = std::max(0, int(c->height - (cpu::d(1) + cpu::d(3)))), yBottom = std::min(c->height, int(c->height - cpu::d(1)));
    for (int y = yTop; y < yBottom; ++y)
        if (x1 > x0) std::memset(mem::base() + c->data + std::size_t(y) * std::size_t(c->bytesPerRow) + std::size_t(x0) * 4, 0, std::size_t(x1 - x0) * 4);
}
HLE(CGContextFillRect) {
    auto *c = objc::host<ContextData>(arg(0));
    if (!c) return;
    const int x0 = std::max(0, int(cpu::d(0))), x1 = std::min(c->width, int(cpu::d(0) + cpu::d(2)));
    const int yTop = std::max(0, int(c->height - (cpu::d(1) + cpu::d(3)))), yBottom = std::min(c->height, int(c->height - cpu::d(1)));
    for (int y = yTop; y < yBottom; ++y)
        for (int x = x0; x < x1; ++x) {
            auto *p = mem::base() + c->data + std::size_t(y) * std::size_t(c->bytesPerRow) + std::size_t(x) * 4;
            for (int k = 0; k < 4; ++k) p[k] = std::uint8_t((k == 3 ? c->fill[3] : c->fill[k] * c->fill[3]) * 255 + 0.5);
        }
}
HLE(CGContextSaveGState) {}
HLE(CGContextRestoreGState) {}
HLE(CGContextTranslateCTM) {}
HLE(CGContextScaleCTM) {}
HLE(CGDataProviderCreateWithCFData) {
    auto d = std::make_unique<DataProviderData>();
    const auto bytes = dataBytes(arg(0));
    d->bytes.assign(mem::base() + bytes, mem::base() + bytes + dataLength(arg(0)));
    ret(makeObject("CGDataProvider", std::move(d)));
}
HLE(CGFontCreateWithDataProvider) {
    auto *p = objc::host<DataProviderData>(arg(0));
    if (!p) return ret(0);
    auto face = std::make_shared<Face>();
    face->bytes = p->bytes;
    if (!face->init()) { hlog("ct", "CGFontCreateWithDataProvider: not a TrueType font"); return ret(0); }
    hlog("ct", "loaded font \"%s\" (%zu bytes)", face->family.c_str(), face->bytes.size());
    auto d = std::make_unique<CGFontData>();
    d->face = face;
    ret(makeObject("CGFont", std::move(d)));
}

// ---------------- CoreText fonts ----------------
HLE(CTFontCreateWithGraphicsFont) {
    // (graphicsFont, size d0, matrix, descriptor): the descriptor's cascade list provides fallback fonts.
    auto *g = objc::host<CGFontData>(arg(0));
    if (!g) return ret(0);
    auto *desc = objc::host<DescriptorData>(arg(2));
    ret(makeFont(g->face, cpu::d(0), desc ? desc->cascade : std::vector<id>{}));
}
HLE(CTFontCreateWithName) {
    const auto name = text(arg(0));
    auto face = systemFace(name);
    if (!face) return ret(0);
    ret(makeFont(face, cpu::d(0) > 0 ? cpu::d(0) : 12.0, {}));
}
HLE(CTFontCopyFontDescriptor) {
    auto d = std::make_unique<DescriptorData>();
    d->font = arg(0);
    objc::retain(arg(0));
    if (auto *f = font(arg(0))) d->cascade = f->cascade;
    ret(makeObject("CTFontDescriptor", std::move(d)));
}
HLE(CTFontDescriptorCreateWithAttributes) {
    auto d = std::make_unique<DescriptorData>();
    d->cascade = cascadeFrom(arg(0));
    ret(makeObject("CTFontDescriptor", std::move(d)));
}
HLE(CTFontDescriptorCreateCopyWithAttributes) {
    auto *src = objc::host<DescriptorData>(arg(0));
    auto d = std::make_unique<DescriptorData>();
    if (src) { d->font = src->font; d->cascade = src->cascade; }
    const auto extra = cascadeFrom(arg(1));
    if (!extra.empty()) d->cascade = extra;
    ret(makeObject("CTFontDescriptor", std::move(d)));
}
HLE(CTFontCreateWithFontDescriptor) {
    auto *desc = objc::host<DescriptorData>(arg(0));
    if (!desc || !desc->font) return ret(0);
    auto *f = font(desc->font);
    ret(makeFont(f->face, cpu::d(0) > 0 ? cpu::d(0) : f->size, desc->cascade));
}
HLE(CTFontGetAscent) { double a, d, l; metrics(arg(0), a, d, l); hle::retDouble(a); }
HLE(CTFontGetDescent) { double a, d, l; metrics(arg(0), a, d, l); hle::retDouble(d); }
HLE(CTFontGetLeading) { double a, d, l; metrics(arg(0), a, d, l); hle::retDouble(l); }
HLE(CTFontGetUnderlineThickness) { auto *f = font(arg(0)); hle::retDouble(f ? f->size / 14.0 : 1.0); }
HLE(CTFontGetSize) { auto *f = font(arg(0)); hle::retDouble(f ? f->size : 0); }
HLE(CTFontGetSymbolicTraits) { auto *f = font(arg(0)); ret(f && f->face && f->face->bold ? 2 : 0); }
HLE(CTFontCopyFamilyName) { auto *f = font(arg(0)); ret(string(f && f->face ? f->face->family : "")); }
HLE(CTFontCopyPostScriptName) { auto *f = font(arg(0)); ret(string(f && f->face ? f->face->family : "")); }
// CTFontGetBoundingRectsForGlyphs(font, orientation, glyphs, rects, count) -> union rect (d0-d3)
HLE(CTFontGetBoundingRectsForGlyphs) {
    auto *f = font(arg(0));
    double ux0 = 1e9, uy0 = 1e9, ux1 = -1e9, uy1 = -1e9;
    for (std::uint64_t i = 0; i < arg(4); ++i) {
        const auto glyph = mem::read<std::uint16_t>(arg(2) + 2 * i);
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        if (f && f->face) stbtt_GetGlyphBox(&f->face->info, glyph, &x0, &y0, &x1, &y1);
        const double s = f ? f->scale() : 1;
        const double rect[4] = {x0 * s, y0 * s, (x1 - x0) * s, (y1 - y0) * s};
        if (arg(3)) for (int k = 0; k < 4; ++k) mem::write<double>(arg(3) + 32 * i + 8 * std::uint64_t(k), rect[k]);
        ux0 = std::min(ux0, rect[0]); uy0 = std::min(uy0, rect[1]);
        ux1 = std::max(ux1, rect[0] + rect[2]); uy1 = std::max(uy1, rect[1] + rect[3]);
    }
    if (ux1 < ux0) retRect(0, 0, 0, 0);
    else retRect(ux0, uy0, ux1 - ux0, uy1 - uy0);
}
// CTFontGetAdvancesForGlyphs(font, orientation, glyphs, advances (CGSize), count) -> total
HLE(CTFontGetAdvancesForGlyphs) {
    double total = 0;
    for (std::uint64_t i = 0; i < arg(4); ++i) {
        const double a = advanceOf(arg(0), mem::read<std::uint16_t>(arg(2) + 2 * i));
        if (arg(3)) { mem::write<double>(arg(3) + 16 * i, a); mem::write<double>(arg(3) + 16 * i + 8, 0.0); }
        total += a;
    }
    hle::retDouble(total);
}
HLE(CTFontGetGlyphsForCharacters) {
    auto *f = font(arg(0));
    bool all = true;
    for (std::uint64_t i = 0; i < arg(3); ++i) {
        const auto g = f && f->face ? stbtt_FindGlyphIndex(&f->face->info, mem::read<std::uint16_t>(arg(1) + 2 * i)) : 0;
        mem::write<std::uint16_t>(arg(2) + 2 * i, std::uint16_t(g));
        all = all && g;
    }
    ret(all);
}
// CTFontDrawGlyphs(font, glyphs, positions (CGPoint), count, context)
HLE(CTFontDrawGlyphs) {
    auto *c = objc::host<ContextData>(arg(4));
    if (!c) return;
    for (std::uint64_t i = 0; i < arg(3); ++i)
        blendGlyph(*c, arg(0), mem::read<std::uint16_t>(arg(1) + 2 * i), mem::read<double>(arg(2) + 16 * i), mem::read<double>(arg(2) + 16 * i + 8));
}

// ---------------- Attributed strings, typesetters, lines, runs ----------------
HLE(CFAttributedStringCreate) {
    auto d = std::make_unique<AttributedData>();
    d->string = arg(1);
    d->attributes = arg(2);
    objc::retain(arg(1));
    objc::retain(arg(2));
    ret(makeObject("NSAttributedString", std::move(d)));
}
OBJC_METHOD(NSAttributedString, "initWithString:attributes:", initWithStringAttributes) {
    auto d = std::make_unique<AttributedData>();
    d->string = objc::marg(0);
    d->attributes = objc::marg(1);
    objc::retain(objc::marg(0));
    objc::retain(objc::marg(1));
    objc::setHostObject(objc::self(), std::move(d));
    ret(objc::self());
}
OBJC_METHOD(NSAttributedString, "initWithString:", initWithString) {
    auto d = std::make_unique<AttributedData>();
    d->string = objc::marg(0);
    objc::retain(objc::marg(0));
    objc::setHostObject(objc::self(), std::move(d));
    ret(objc::self());
}
OBJC_METHOD(NSAttributedString, "string", string_) { auto *d = objc::host<AttributedData>(objc::self()); ret(d ? d->string : 0); }
OBJC_METHOD(NSAttributedString, "length", length) { auto *d = objc::host<AttributedData>(objc::self()); ret(d ? text16(d->string).size() : 0); }
HLE(CTTypesetterCreateWithAttributedString) {
    auto *a = objc::host<AttributedData>(arg(0));
    if (!a) return ret(0);
    auto d = std::make_unique<TypesetterData>();
    d->text = text16(a->string);
    if (std::getenv("HLE_TEXTLOG")) {
        std::string dump;
        for (char16_t c : d->text) {
            char buf[16];
            if (c >= 32 && c < 127) dump += char(c);
            else { std::snprintf(buf, sizeof buf, "<%X>", unsigned(c)); dump += buf; }
        }
        hlog("text", "typesetter: %s", dump.c_str());
    }
    d->font = dictGet(a->attributes, string("kCTFontAttributeName"));
    if (!d->font) d->font = dictGet(a->attributes, string("NSFontAttributeName"));
    objc::retain(d->font);
    ret(makeObject("CTTypesetter", std::move(d)));
}
// CTTypesetterSuggestLineBreak(typesetter, start, width) -> characters in the line (including the break character).
HLE(CTTypesetterSuggestLineBreak) {
    auto *ts = objc::host<TypesetterData>(arg(0));
    const auto start = std::int64_t(arg(1));
    const double width = cpu::d(0);
    if (!ts || start >= std::int64_t(ts->text.size())) return ret(0);
    double x = 0;
    std::int64_t lastBreak = -1;
    for (std::int64_t i = start; i < std::int64_t(ts->text.size()); ++i) {
        const char16_t c = ts->text[std::size_t(i)];
        // Hard breaks as in CoreText: LF, CR, CR LF (one break), NEL, LINE/PARAGRAPH SEPARATOR.
        if (c == u'\r' && i + 1 < std::int64_t(ts->text.size()) && ts->text[std::size_t(i + 1)] == u'\n') return ret(std::uint64_t(i - start + 2));
        if (c == u'\n' || c == u'\r' || c == 0x85 || c == 0x2028 || c == 0x2029) return ret(std::uint64_t(i - start + 1));
        std::uint16_t g;
        const id f = fontFor(ts->font, c, g);
        const double adv = advanceOf(f, g);
        if (c == u' ' || c == u'\t' || c == u'-' || (c >= 0x3000 && c < 0xA000)) lastBreak = i;
        if (x + adv > width && i > start && c != u' ') {
            if (lastBreak >= start && lastBreak < i) return ret(std::uint64_t(lastBreak - start + 1));
            return ret(std::uint64_t(i - start));
        }
        x += adv;
    }
    ret(std::uint64_t(std::int64_t(ts->text.size()) - start));
}
HLE(CTTypesetterCreateLine) {
    auto *ts = objc::host<TypesetterData>(arg(0));
    if (!ts) return ret(0);
    auto line = std::make_unique<LineData>();
    std::int64_t location = std::int64_t(arg(1)), length = std::int64_t(arg(2));
    if (length == 0) length = std::int64_t(ts->text.size()) - location;
    layout(*ts, location, length, *line);
    ret(makeObject("CTLine", std::move(line)));
}
HLE(CTLineCreateWithAttributedString) {
    auto *a = objc::host<AttributedData>(arg(0));
    if (!a) return ret(0);
    TypesetterData ts;
    ts.text = text16(a->string);
    ts.font = dictGet(a->attributes, string("kCTFontAttributeName"));
    auto line = std::make_unique<LineData>();
    layout(ts, 0, std::int64_t(ts.text.size()), *line);
    ret(makeObject("CTLine", std::move(line)));
}
HLE(CTLineGetStringRange) {
    auto *l = objc::host<LineData>(arg(0));
    cpu::setX(0, l ? std::uint64_t(l->location) : 0);
    cpu::setX(1, l ? std::uint64_t(l->length) : 0);
}
HLE(CTLineGetGlyphRuns) { auto *l = objc::host<LineData>(arg(0)); ret(l ? l->runObjects : array({})); }
HLE(CTLineGetGlyphCount) {
    auto *l = objc::host<LineData>(arg(0));
    std::uint64_t n = 0;
    if (l) for (auto &r : l->runs) n += r.glyphs.size();
    ret(n);
}
// CTLineGetTypographicBounds(line, ascent*, descent*, leading*) -> width
HLE(CTLineGetTypographicBounds) {
    auto *l = objc::host<LineData>(arg(0));
    if (!l) return hle::retDouble(0);
    if (arg(1)) mem::write<double>(arg(1), l->ascent);
    if (arg(2)) mem::write<double>(arg(2), l->descent);
    if (arg(3)) mem::write<double>(arg(3), l->leading);
    hle::retDouble(l->width);
}
HLE(CTLineGetOffsetForStringIndex) {
    auto *l = objc::host<LineData>(arg(0));
    const auto index = std::int64_t(arg(1));
    double offset = 0;
    if (l) {
        offset = l->width;
        for (auto &r : l->runs)
            for (std::size_t i = 0; i < r.indices.size(); ++i)
                if (r.indices[i] >= index) { offset = std::min(offset, r.x[i]); }
    }
    if (arg(2)) mem::write<double>(arg(2), offset);
    hle::retDouble(offset);
}
HLE(CTLineGetStringIndexForPosition) {
    auto *l = objc::host<LineData>(arg(0));
    const double x = cpu::d(0);
    std::int64_t index = l ? l->location + l->length : 0;
    if (l)
        for (auto &r : l->runs)
            for (std::size_t i = 0; i < r.indices.size(); ++i)
                if (r.x[i] + r.advances[i] / 2 > x) { index = std::min(index, r.indices[i]); }
    ret(std::uint64_t(index));
}
HLE(CTLineGetPenOffsetForFlush) {
    auto *l = objc::host<LineData>(arg(0));
    const double flush = cpu::d(0), width = cpu::d(1);
    hle::retDouble(l ? std::max(0.0, (width - l->width) * flush) : 0);
}
HLE(CTLineDraw) {}
HLE(CTLineCreateTruncatedLine) { objc::retain(arg(0)); ret(arg(0)); }
static Run *run(id r) { auto *d = objc::host<RunData>(r); return d ? &d->run : nullptr; }
HLE(CTRunGetGlyphCount) { auto *r = run(arg(0)); ret(r ? r->glyphs.size() : 0); }
HLE(CTRunGetAttributes) { auto *d = objc::host<RunData>(arg(0)); ret(d ? d->attributes : 0); }
HLE(CTRunGetStatus) { ret(0); }
HLE(CTRunGetStringRange) {
    auto *r = run(arg(0));
    cpu::setX(0, r && !r->indices.empty() ? std::uint64_t(r->indices.front()) : 0);
    cpu::setX(1, r ? r->indices.size() : 0);
}
// Range arguments: (run, CFRange{location x1, length x2}, buffer x3). A zero length means "to the end".
static void runRange(Run *r, std::uint64_t &begin, std::uint64_t &end) {
    begin = arg(1);
    end = arg(2) ? begin + arg(2) : (r ? r->glyphs.size() : 0);
    if (r) end = std::min<std::uint64_t>(end, r->glyphs.size());
}
HLE(CTRunGetGlyphs) {
    auto *r = run(arg(0));
    std::uint64_t b, e;
    runRange(r, b, e);
    for (auto i = b; r && i < e; ++i) mem::write<std::uint16_t>(arg(3) + 2 * (i - b), r->glyphs[std::size_t(i)]);
}
HLE(CTRunGetPositions) {
    auto *r = run(arg(0));
    std::uint64_t b, e;
    runRange(r, b, e);
    for (auto i = b; r && i < e; ++i) {
        mem::write<double>(arg(3) + 16 * (i - b), r->x[std::size_t(i)]);
        mem::write<double>(arg(3) + 16 * (i - b) + 8, 0.0);
    }
}
HLE(CTRunGetAdvances) {
    auto *r = run(arg(0));
    std::uint64_t b, e;
    runRange(r, b, e);
    for (auto i = b; r && i < e; ++i) {
        mem::write<double>(arg(3) + 16 * (i - b), r->advances[std::size_t(i)]);
        mem::write<double>(arg(3) + 16 * (i - b) + 8, 0.0);
    }
}
HLE(CTRunGetStringIndices) {
    auto *r = run(arg(0));
    std::uint64_t b, e;
    runRange(r, b, e);
    for (auto i = b; r && i < e; ++i) mem::w64(arg(3) + 8 * (i - b), std::uint64_t(r->indices[std::size_t(i)]));
}
HLE(CTRunGetTypographicBounds) {
    auto *r = run(arg(0));
    double width = 0;
    if (r) for (auto a : r->advances) width += a;
    double a = 0, d = 0, l = 0;
    if (r) metrics(r->font, a, d, l);
    if (arg(3)) mem::write<double>(arg(3), a);
    if (arg(4)) mem::write<double>(arg(4), d);
    if (arg(5)) mem::write<double>(arg(5), l);
    hle::retDouble(width);
}
HLE(CTParagraphStyleCreate) { ret(objc::allocInstance(objc::getClass("NSObject"))); }

namespace uikit {
// Renders one line of text (Arial) into an RGBA image (white text, transparent background), for host overlays.
bool renderTextLine(const std::u16string &text, int pixelHeight, std::vector<std::uint8_t> &rgba, int &width, int &height) {
    auto face = systemFace("ArialMT");
    if (!face) return false;
    const float scale = stbtt_ScaleForPixelHeight(&face->info, float(pixelHeight));
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&face->info, &ascent, &descent, &gap);
    const int baseline = int(ascent * scale);
    height = int((ascent - descent) * scale) + 2;
    float x = 0;
    for (auto c : text) { int adv, lsb; stbtt_GetCodepointHMetrics(&face->info, c, &adv, &lsb); x += adv * scale; }
    width = std::max(1, int(x) + 2);
    rgba.assign(std::size_t(width) * std::size_t(height) * 4, 0);
    float pen = 0;
    for (auto c : text) {
        int adv, lsb, x0, y0, x1, y1;
        stbtt_GetCodepointHMetrics(&face->info, c, &adv, &lsb);
        stbtt_GetCodepointBitmapBox(&face->info, c, scale, scale, &x0, &y0, &x1, &y1);
        const int w = x1 - x0, h = y1 - y0;
        if (w > 0 && h > 0) {
            std::vector<unsigned char> glyph(std::size_t(w * h));
            stbtt_MakeCodepointBitmap(&face->info, glyph.data(), w, h, w, scale, scale, c);
            for (int j = 0; j < h; ++j)
                for (int i = 0; i < w; ++i) {
                    const int px = int(pen) + x0 + i, py = baseline + y0 + j;
                    if (px < 0 || px >= width || py < 0 || py >= height) continue;
                    auto *p = &rgba[(std::size_t(py) * std::size_t(width) + std::size_t(px)) * 4];
                    p[0] = p[1] = p[2] = 255;
                    p[3] = std::max(p[3], glyph[std::size_t(j * w + i)]);
                }
        }
        pen += adv * scale;
    }
    return true;
}
} // namespace uikit
