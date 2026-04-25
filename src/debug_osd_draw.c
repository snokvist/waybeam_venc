#include "debug_osd_draw.h"
#include "debug_osd.h"

#include <stdlib.h>
#include <string.h>

/* ── Palette ──────────────────────────────────────────────────────────
 * I4 fits exactly 16 entries; we use 9.  Indices must match DEBUG_OSD_*
 * in include/debug_osd.h.  Alpha values match the 4-bit ARGB4444 codes
 * the legacy implementation used (0x4 → 68, 0xA → 170, 0xF → 255) so
 * visual output is unchanged. */
static const OsdPaletteEntry g_palette[OSD_PALETTE_SIZE] = {
	[DEBUG_OSD_TRANSPARENT]       = {   0,   0,   0,   0 },
	[DEBUG_OSD_WHITE]             = { 255, 255, 255, 255 },
	[DEBUG_OSD_RED]               = { 255, 255,   0,   0 },
	[DEBUG_OSD_GREEN]             = { 255,   0, 255,   0 },
	[DEBUG_OSD_BLUE]              = { 255,   0,   0, 255 },
	[DEBUG_OSD_YELLOW]            = { 255, 255, 255,   0 },
	[DEBUG_OSD_CYAN]              = { 255,   0, 255, 255 },
	[DEBUG_OSD_SEMITRANS_GREEN]   = {  68,   0, 170,   0 },
	[DEBUG_OSD_SEMITRANS_BLACK]   = { 170,   0,   0,   0 },
};

const OsdPaletteEntry *osd_palette(void)
{
	return g_palette;
}

/* ── 8x8 bitmap font (CP437 printable ASCII, public domain) ────────
 * Each glyph: 8 rows, 5px wide, bit 4 = leftmost pixel. */
static const uint8_t g_font8x8[128][8] = {
	[0x20] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* space */
	[0x21] = {0x04,0x04,0x04,0x04,0x04,0x00,0x04,0x00}, /* ! */
	[0x22] = {0x0A,0x0A,0x0A,0x00,0x00,0x00,0x00,0x00}, /* " */
	[0x23] = {0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A,0x00}, /* # */
	[0x24] = {0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04,0x00}, /* $ */
	[0x25] = {0x18,0x19,0x02,0x04,0x08,0x13,0x03,0x00}, /* % */
	[0x26] = {0x08,0x14,0x14,0x08,0x15,0x12,0x0D,0x00}, /* & */
	[0x27] = {0x04,0x04,0x08,0x00,0x00,0x00,0x00,0x00}, /* ' */
	[0x28] = {0x02,0x04,0x08,0x08,0x08,0x04,0x02,0x00}, /* ( */
	[0x29] = {0x08,0x04,0x02,0x02,0x02,0x04,0x08,0x00}, /* ) */
	[0x2A] = {0x00,0x04,0x15,0x0E,0x15,0x04,0x00,0x00}, /* * */
	[0x2B] = {0x00,0x04,0x04,0x1F,0x04,0x04,0x00,0x00}, /* + */
	[0x2C] = {0x00,0x00,0x00,0x00,0x00,0x04,0x04,0x08}, /* , */
	[0x2D] = {0x00,0x00,0x00,0x1F,0x00,0x00,0x00,0x00}, /* - */
	[0x2E] = {0x00,0x00,0x00,0x00,0x00,0x00,0x04,0x00}, /* . */
	[0x2F] = {0x00,0x01,0x02,0x04,0x08,0x10,0x00,0x00}, /* / */
	[0x30] = {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E,0x00}, /* 0 */
	[0x31] = {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E,0x00}, /* 1 */
	[0x32] = {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F,0x00}, /* 2 */
	[0x33] = {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E,0x00}, /* 3 */
	[0x34] = {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02,0x00}, /* 4 */
	[0x35] = {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E,0x00}, /* 5 */
	[0x36] = {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E,0x00}, /* 6 */
	[0x37] = {0x1F,0x01,0x02,0x04,0x08,0x08,0x08,0x00}, /* 7 */
	[0x38] = {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E,0x00}, /* 8 */
	[0x39] = {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C,0x00}, /* 9 */
	[0x3A] = {0x00,0x00,0x04,0x00,0x00,0x04,0x00,0x00}, /* : */
	[0x3B] = {0x00,0x00,0x04,0x00,0x00,0x04,0x04,0x08}, /* ; */
	[0x3C] = {0x02,0x04,0x08,0x10,0x08,0x04,0x02,0x00}, /* < */
	[0x3D] = {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00,0x00}, /* = */
	[0x3E] = {0x08,0x04,0x02,0x01,0x02,0x04,0x08,0x00}, /* > */
	[0x3F] = {0x0E,0x11,0x01,0x02,0x04,0x00,0x04,0x00}, /* ? */
	[0x40] = {0x0E,0x11,0x17,0x15,0x17,0x10,0x0E,0x00}, /* @ */
	[0x41] = {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11,0x00}, /* A */
	[0x42] = {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E,0x00}, /* B */
	[0x43] = {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E,0x00}, /* C */
	[0x44] = {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C,0x00}, /* D */
	[0x45] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F,0x00}, /* E */
	[0x46] = {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10,0x00}, /* F */
	[0x47] = {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F,0x00}, /* G */
	[0x48] = {0x11,0x11,0x11,0x1F,0x11,0x11,0x11,0x00}, /* H */
	[0x49] = {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E,0x00}, /* I */
	[0x4A] = {0x07,0x02,0x02,0x02,0x02,0x12,0x0C,0x00}, /* J */
	[0x4B] = {0x11,0x12,0x14,0x18,0x14,0x12,0x11,0x00}, /* K */
	[0x4C] = {0x10,0x10,0x10,0x10,0x10,0x10,0x1F,0x00}, /* L */
	[0x4D] = {0x11,0x1B,0x15,0x15,0x11,0x11,0x11,0x00}, /* M */
	[0x4E] = {0x11,0x19,0x15,0x13,0x11,0x11,0x11,0x00}, /* N */
	[0x4F] = {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E,0x00}, /* O */
	[0x50] = {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10,0x00}, /* P */
	[0x51] = {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D,0x00}, /* Q */
	[0x52] = {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11,0x00}, /* R */
	[0x53] = {0x0E,0x11,0x10,0x0E,0x01,0x11,0x0E,0x00}, /* S */
	[0x54] = {0x1F,0x04,0x04,0x04,0x04,0x04,0x04,0x00}, /* T */
	[0x55] = {0x11,0x11,0x11,0x11,0x11,0x11,0x0E,0x00}, /* U */
	[0x56] = {0x11,0x11,0x11,0x11,0x0A,0x0A,0x04,0x00}, /* V */
	[0x57] = {0x11,0x11,0x11,0x15,0x15,0x1B,0x11,0x00}, /* W */
	[0x58] = {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11,0x00}, /* X */
	[0x59] = {0x11,0x11,0x0A,0x04,0x04,0x04,0x04,0x00}, /* Y */
	[0x5A] = {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F,0x00}, /* Z */
	[0x5B] = {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E,0x00}, /* [ */
	[0x5C] = {0x00,0x10,0x08,0x04,0x02,0x01,0x00,0x00}, /* \ */
	[0x5D] = {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E,0x00}, /* ] */
	[0x5E] = {0x04,0x0A,0x11,0x00,0x00,0x00,0x00,0x00}, /* ^ */
	[0x5F] = {0x00,0x00,0x00,0x00,0x00,0x00,0x1F,0x00}, /* _ */
	[0x60] = {0x08,0x04,0x02,0x00,0x00,0x00,0x00,0x00}, /* ` */
	[0x61] = {0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F,0x00}, /* a */
	[0x62] = {0x10,0x10,0x1E,0x11,0x11,0x11,0x1E,0x00}, /* b */
	[0x63] = {0x00,0x00,0x0E,0x11,0x10,0x11,0x0E,0x00}, /* c */
	[0x64] = {0x01,0x01,0x0F,0x11,0x11,0x11,0x0F,0x00}, /* d */
	[0x65] = {0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E,0x00}, /* e */
	[0x66] = {0x06,0x08,0x1C,0x08,0x08,0x08,0x08,0x00}, /* f */
	[0x67] = {0x00,0x00,0x0F,0x11,0x0F,0x01,0x0E,0x00}, /* g */
	[0x68] = {0x10,0x10,0x1E,0x11,0x11,0x11,0x11,0x00}, /* h */
	[0x69] = {0x04,0x00,0x0C,0x04,0x04,0x04,0x0E,0x00}, /* i */
	[0x6A] = {0x02,0x00,0x06,0x02,0x02,0x12,0x0C,0x00}, /* j */
	[0x6B] = {0x10,0x10,0x12,0x14,0x18,0x14,0x12,0x00}, /* k */
	[0x6C] = {0x0C,0x04,0x04,0x04,0x04,0x04,0x0E,0x00}, /* l */
	[0x6D] = {0x00,0x00,0x1A,0x15,0x15,0x11,0x11,0x00}, /* m */
	[0x6E] = {0x00,0x00,0x1E,0x11,0x11,0x11,0x11,0x00}, /* n */
	[0x6F] = {0x00,0x00,0x0E,0x11,0x11,0x11,0x0E,0x00}, /* o */
	[0x70] = {0x00,0x00,0x1E,0x11,0x1E,0x10,0x10,0x00}, /* p */
	[0x71] = {0x00,0x00,0x0F,0x11,0x0F,0x01,0x01,0x00}, /* q */
	[0x72] = {0x00,0x00,0x16,0x19,0x10,0x10,0x10,0x00}, /* r */
	[0x73] = {0x00,0x00,0x0F,0x10,0x0E,0x01,0x1E,0x00}, /* s */
	[0x74] = {0x08,0x08,0x1C,0x08,0x08,0x09,0x06,0x00}, /* t */
	[0x75] = {0x00,0x00,0x11,0x11,0x11,0x13,0x0D,0x00}, /* u */
	[0x76] = {0x00,0x00,0x11,0x11,0x11,0x0A,0x04,0x00}, /* v */
	[0x77] = {0x00,0x00,0x11,0x11,0x15,0x15,0x0A,0x00}, /* w */
	[0x78] = {0x00,0x00,0x11,0x0A,0x04,0x0A,0x11,0x00}, /* x */
	[0x79] = {0x00,0x00,0x11,0x11,0x0F,0x01,0x0E,0x00}, /* y */
	[0x7A] = {0x00,0x00,0x1F,0x02,0x04,0x08,0x1F,0x00}, /* z */
	[0x7B] = {0x02,0x04,0x04,0x08,0x04,0x04,0x02,0x00}, /* { */
	[0x7C] = {0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x00}, /* | */
	[0x7D] = {0x08,0x04,0x04,0x02,0x04,0x04,0x08,0x00}, /* } */
	[0x7E] = {0x00,0x00,0x08,0x15,0x02,0x00,0x00,0x00}, /* ~ */
};

/* ── Dirty rect (multi-rect list) ────────────────────────────────────── */

void osd_dirty_reset(OsdDirty *d, uint32_t w, uint32_t h)
{
	d->x0 = (uint16_t)w;
	d->y0 = (uint16_t)h;
	d->x1 = 0;
	d->y1 = 0;
	d->count = 0;
}

int osd_dirty_empty(const OsdDirty *d)
{
	return d->x1 < d->x0 || d->y1 < d->y0;
}

/* Update the union bbox to cover (x,y), assuming (x,y) is already clamped. */
static inline void union_bbox(OsdDirty *d, int x, int y)
{
	if ((uint16_t)x < d->x0) d->x0 = (uint16_t)x;
	if ((uint16_t)y < d->y0) d->y0 = (uint16_t)y;
	if ((uint16_t)x > d->x1) d->x1 = (uint16_t)x;
	if ((uint16_t)y > d->y1) d->y1 = (uint16_t)y;
}

/* Should rects A and B coalesce?  Merge only when the resulting bounding
 * box area is at most 1.25× the sum of the originals — keeps row-adjacent
 * char rects merging into a string strip but rejects orthogonal outline
 * strips (top + left) which would otherwise merge into the full rect bbox
 * and defeat the entire optimization. */
static inline int rects_touch(uint16_t ax0, uint16_t ay0, uint16_t ax1,
	uint16_t ay1, int bx0, int by0, int bx1, int by1)
{
	/* Disjoint with a one-pixel gap: never coalesce — they're truly
	 * separate regions and merging would just enlarge the cleared area. */
	if (bx0 > (int)ax1 + 1 || bx1 < (int)ax0 - 1 ||
	    by0 > (int)ay1 + 1 || by1 < (int)ay0 - 1)
		return 0;

	int a_area = ((int)ax1 - (int)ax0 + 1) * ((int)ay1 - (int)ay0 + 1);
	int b_area = (bx1 - bx0 + 1) * (by1 - by0 + 1);
	int mx0 = (int)ax0 < bx0 ? (int)ax0 : bx0;
	int my0 = (int)ay0 < by0 ? (int)ay0 : by0;
	int mx1 = (int)ax1 > bx1 ? (int)ax1 : bx1;
	int my1 = (int)ay1 > by1 ? (int)ay1 : by1;
	int merged_area = (mx1 - mx0 + 1) * (my1 - my0 + 1);

	/* Allow a small expansion (~25%) to absorb adjacent-char gap. */
	return merged_area * 4 <= (a_area + b_area) * 5;
}

void osd_dirty_add_rect(OsdDirty *d, const OsdCanvas *c,
	int x0, int y0, int x1, int y1)
{
	/* Normalize + clamp. */
	if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
	if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
	if (x1 < 0 || y1 < 0) return;
	if (x0 >= (int)c->width || y0 >= (int)c->height) return;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 >= (int)c->width)  x1 = (int)c->width - 1;
	if (y1 >= (int)c->height) y1 = (int)c->height - 1;

	union_bbox(d, x0, y0);
	union_bbox(d, x1, y1);

	if (d->count < 0) return;  /* already in overflow → bbox-only mode */

	/* Coalesce with the previous rect if they touch.  This handles the
	 * common case of consecutive char-cells (string) merging into one
	 * row-strip, and adjacent strip pieces merging on the same line. */
	if (d->count > 0) {
		int last = d->count - 1;
		if (rects_touch(d->rects[last].x0, d->rects[last].y0,
		                d->rects[last].x1, d->rects[last].y1,
		                x0, y0, x1, y1)) {
			if ((uint16_t)x0 < d->rects[last].x0)
				d->rects[last].x0 = (uint16_t)x0;
			if ((uint16_t)y0 < d->rects[last].y0)
				d->rects[last].y0 = (uint16_t)y0;
			if ((uint16_t)x1 > d->rects[last].x1)
				d->rects[last].x1 = (uint16_t)x1;
			if ((uint16_t)y1 > d->rects[last].y1)
				d->rects[last].y1 = (uint16_t)y1;
			return;
		}
	}

	if (d->count >= OSD_DIRTY_MAX_RECTS) {
		/* Overflow: fall back to single-bbox clear (already maintained). */
		d->count = -1;
		return;
	}

	d->rects[d->count].x0 = (uint16_t)x0;
	d->rects[d->count].y0 = (uint16_t)y0;
	d->rects[d->count].x1 = (uint16_t)x1;
	d->rects[d->count].y1 = (uint16_t)y1;
	d->count++;
}

void osd_dirty_expand(OsdDirty *d, const OsdCanvas *c, int x, int y)
{
	/* Legacy single-point expand: clamp the point to canvas bounds and
	 * grow the bbox unconditionally — the original behavior was to keep
	 * out-of-bounds expand calls visible by snapping them to the edge.
	 * add_rect rejects fully-off-canvas rects, so we route through it
	 * only after clamping. */
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x >= (int)c->width)  x = (int)c->width - 1;
	if (y >= (int)c->height) y = (int)c->height - 1;
	osd_dirty_add_rect(d, c, x, y, x, y);
}

/* ── I4 nibble helpers ──────────────────────────────────────────────
 *
 * Pixel x is stored in:
 *   byte = pixels[y * stride_bytes + x/2]
 *   nibble = byte >> ((x & 1) * 4)        ← even-x in low nibble
 *
 * The masks below are indexed by (x & 1):
 *   keep_mask[0] = 0xF0 (preserve high nibble when writing low)
 *   keep_mask[1] = 0x0F (preserve low nibble when writing high) */
static const uint8_t i4_keep_mask[2] = { 0xF0, 0x0F };
static const uint8_t i4_shift[2]      = {    0,    4 };

/* ── Pixel R/W ──────────────────────────────────────────────────────── */

void osd_put_pixel(const OsdCanvas *c, int x, int y, uint8_t color)
{
	if (x < 0 || x >= (int)c->width || y < 0 || y >= (int)c->height)
		return;
	uint8_t *byte = &c->pixels[y * c->stride_bytes + (x >> 1)];
	uint8_t nibble = (uint8_t)(color & 0x0F);
	int idx = x & 1;
	*byte = (uint8_t)((*byte & i4_keep_mask[idx]) |
	                  (nibble << i4_shift[idx]));
}

uint8_t osd_get_pixel(const OsdCanvas *c, int x, int y)
{
	if (x < 0 || x >= (int)c->width || y < 0 || y >= (int)c->height)
		return 0;
	uint8_t byte = c->pixels[y * c->stride_bytes + (x >> 1)];
	return (uint8_t)((byte >> i4_shift[x & 1]) & 0x0F);
}

/* ── Row fill ──────────────────────────────────────────────────────── */

void osd_fill_pixels(const OsdCanvas *c, int x, int y, int count,
                     uint8_t color)
{
	if (count <= 0 || y < 0 || y >= (int)c->height)
		return;
	if (x >= (int)c->width)
		return;
	if (x < 0) {
		count += x;  /* x is negative — shrink count */
		x = 0;
		if (count <= 0)
			return;
	}
	if (x + count > (int)c->width)
		count = (int)c->width - x;

	uint8_t *row = &c->pixels[y * c->stride_bytes];
	uint8_t nibble = (uint8_t)(color & 0x0F);
	uint8_t packed = (uint8_t)((nibble << 4) | nibble);

	int end = x + count;  /* exclusive */

	/* Unaligned start: x is odd → only the high nibble of byte (x>>1)
	 * gets written, low nibble preserved. */
	if (x & 1) {
		uint8_t *byte = &row[x >> 1];
		*byte = (uint8_t)((*byte & 0x0F) | (nibble << 4));
		x++;
	}
	/* Byte-aligned middle: x is now even.  Write packed bytes until we
	 * hit a tail nibble. */
	int aligned_end = end & ~1;  /* round down to even */
	if (aligned_end > x)
		memset(&row[x >> 1], packed, (size_t)((aligned_end - x) >> 1));
	x = aligned_end;
	/* Unaligned tail: end is odd → write only the low nibble of the
	 * final byte, high nibble preserved. */
	if (x < end) {
		uint8_t *byte = &row[x >> 1];
		*byte = (uint8_t)((*byte & 0xF0) | nibble);
	}
}

void osd_clear_dirty(const OsdCanvas *c, const OsdDirty *d)
{
	if (osd_dirty_empty(d))
		return;

	/* Overflow path: clear the union bbox in one go. */
	if (d->count < 0) {
		int clear_w = d->x1 - d->x0 + 1;
		for (uint32_t y = d->y0; y <= d->y1 && y < c->height; y++)
			osd_fill_pixels(c, d->x0, (int)y, clear_w, 0);
		return;
	}

	/* Normal path: clear each tracked rect.  Adjacent rects have already
	 * been coalesced by add_rect so this loop visits at most one rect per
	 * disjoint region of the previous frame's painting. */
	for (int i = 0; i < d->count; i++) {
		int rx0 = d->rects[i].x0;
		int ry0 = d->rects[i].y0;
		int rx1 = d->rects[i].x1;
		int ry1 = d->rects[i].y1;
		int rw = rx1 - rx0 + 1;
		for (int y = ry0; y <= ry1 && y < (int)c->height; y++)
			osd_fill_pixels(c, rx0, y, rw, 0);
	}
}

/* ── Rectangles ────────────────────────────────────────────────────── */

void osd_draw_rect(const OsdCanvas *c, OsdDirty *d,
                   uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                   uint8_t color, int filled)
{
	if (!w || !h) return;

	/* Clamp to canvas bounds */
	int x0 = x, y0 = y;
	int x1 = x + w - 1, y1 = y + h - 1;
	if (x0 >= (int)c->width || y0 >= (int)c->height) return;
	if (x1 >= (int)c->width) x1 = (int)c->width - 1;
	if (y1 >= (int)c->height) y1 = (int)c->height - 1;
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;

	int span = x1 - x0 + 1;

	if (filled) {
		osd_dirty_add_rect(d, c, x0, y0, x1, y1);
		for (int row = y0; row <= y1; row++)
			osd_fill_pixels(c, x0, row, span, color);
	} else {
		/* Outline: paint 4 thin strips and add each as its own dirty
		 * rect, so the next frame's clear only zeroes the perimeter
		 * (~4*(w+h) pixels) rather than the full inner area (w*h). */
		osd_dirty_add_rect(d, c, x0, y0, x1, y0);          /* top */
		osd_fill_pixels(c, x0, y0, span, color);
		osd_dirty_add_rect(d, c, x0, y1, x1, y1);          /* bottom */
		osd_fill_pixels(c, x0, y1, span, color);
		osd_dirty_add_rect(d, c, x0, y0, x0, y1);          /* left */
		osd_dirty_add_rect(d, c, x1, y0, x1, y1);          /* right */
		for (int row = y0; row <= y1; row++) {
			osd_put_pixel(c, x0, row, color);
			osd_put_pixel(c, x1, row, color);
		}
	}
}

/* ── Point ─────────────────────────────────────────────────────────── */

void osd_draw_point(const OsdCanvas *c, OsdDirty *d,
                    uint16_t x, uint16_t y, uint8_t color, int size)
{
	osd_dirty_expand(d, c, (int)x - size, (int)y - size);
	osd_dirty_expand(d, c, (int)x + size, (int)y + size);
	for (int dd = -size; dd <= size; dd++) {
		osd_put_pixel(c, (int)x + dd, (int)y, color);
		osd_put_pixel(c, (int)x, (int)y + dd, color);
	}
}

/* ── Line ──────────────────────────────────────────────────────────── */

void osd_draw_line(const OsdCanvas *c, OsdDirty *d,
                   uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                   uint8_t color)
{
	osd_dirty_expand(d, c, x0, y0);
	osd_dirty_expand(d, c, x1, y1);

	int dx = abs((int)x1 - (int)x0);
	int dy = -abs((int)y1 - (int)y0);
	int sx = x0 < x1 ? 1 : -1;
	int sy = y0 < y1 ? 1 : -1;
	int err = dx + dy;
	int cx = x0, cy = y0;

	for (;;) {
		osd_put_pixel(c, cx, cy, color);
		if (cx == (int)x1 && cy == (int)y1) break;
		int e2 = 2 * err;
		if (e2 >= dy) { err += dy; cx += sx; }
		if (e2 <= dx) { err += dx; cy += sy; }
	}
}

/* ── Glyphs ────────────────────────────────────────────────────────── */

void osd_draw_char(const OsdCanvas *c, OsdDirty *d,
                   int px, int py, char ch, int scale, uint8_t color)
{
	if (ch < 0x20 || ch > 0x7e) ch = '?';
	const uint8_t *glyph = g_font8x8[(unsigned char)ch];
	int s = scale;

	osd_dirty_expand(d, c, px, py);
	osd_dirty_expand(d, c, px + 5 * s - 1, py + 8 * s - 1);

	for (int gy = 0; gy < 8; gy++) {
		uint8_t bits = glyph[gy];
		for (int gx = 0; gx < 5; gx++) {
			if (!(bits & (0x10 >> gx)))
				continue;
			int cx = px + gx * s;
			int cy = py + gy * s;
			for (int sy = 0; sy < s; sy++) {
				int row_y = cy + sy;
				if (row_y < 0 || row_y >= (int)c->height)
					continue;
				/* osd_fill_pixels handles clipping + nibble
				 * alignment internally — no need to special-case
				 * partially-offscreen glyph rows. */
				osd_fill_pixels(c, cx, row_y, s, color);
			}
		}
	}
}

void osd_draw_string(const OsdCanvas *c, OsdDirty *d,
                     int x, int y, const char *str, int scale,
                     uint8_t color)
{
	int cw = 6 * scale;  /* 5px glyph + 1px gap, scaled */
	for (int i = 0; str[i]; i++)
		osd_draw_char(c, d, x + i * cw, y, str[i], scale, color);
}
