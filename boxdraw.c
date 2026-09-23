#include <X11/Xft/Xft.h>
#include "st.h"
#include "boxdraw_data.h"

#define DIV(n, d) (((n) + (d) / 2) / (d))

static Display *xdpy;
static Colormap xcmap;
static XftDraw *xd;
static Visual *xvis;

static void drawbox(int, int, int, int, XftColor *, XftColor *, ushort);
static void drawboxlines(int, int, int, int, XftColor *, ushort);

void
boxdraw_xinit(Display *dpy, Colormap cmap, XftDraw *draw, Visual *vis)
{
	xdpy = dpy; xcmap = cmap; xd = draw, xvis = vis;
}

int
isboxdraw(Rune u)
{
	Rune block = u & BOXDRAW_BLOCK_MASK;
	return (boxdraw && block == BOXDRAW_BLOCK && boxdata[(uint8_t)u]) ||
	       (boxdraw_braille && block == BRAILLE_BLOCK);
}

ushort
boxdrawindex(const Glyph *g)
{
	if (boxdraw_braille && (g->u & BRAILLE_BLOCK_MASK) == BRAILLE_BLOCK)
		return BRL | (uint8_t)g->u;
	if (boxdraw_bold && (g->mode & ATTR_BOLD))
		return BDB | boxdata[(uint8_t)g->u];
	return boxdata[(uint8_t)g->u];
}

void
drawboxes(int x, int y, int cw, int ch, XftColor *fg, XftColor *bg,
          const XftGlyphFontSpec *specs, int len)
{
	for ( ; len-- > 0; x += cw, specs++)
		drawbox(x, y, cw, ch, fg, bg, (ushort)specs->glyph);
}

void
drawbox(int x, int y, int w, int h, XftColor *fg, XftColor *bg, ushort bd)
{
	ushort cat = bd & ~(BDB | 0xff);
	if (bd & (BDL | BDA)) {

		drawboxlines(x, y, w, h, fg, bd);

	} else if (cat == BBD) {

		int d = DIV((uint8_t)bd * h, 8);
		XftDrawRect(xd, fg, x, y + d, w, h - d);

	} else if (cat == BBU) {

		XftDrawRect(xd, fg, x, y, w, DIV((uint8_t)bd * h, 8));

	} else if (cat == BBL) {

		XftDrawRect(xd, fg, x, y, DIV((uint8_t)bd * w, 8), h);

	} else if (cat == BBR) {

		int d = DIV((uint8_t)bd * w, 8);
		XftDrawRect(xd, fg, x + d, y, w - d, h);

	} else if (cat == BBQ) {

		int w2 = DIV(w, 2), h2 = DIV(h, 2);
		if (bd & TL)
			XftDrawRect(xd, fg, x, y, w2, h2);
		if (bd & TR)
			XftDrawRect(xd, fg, x + w2, y, w - w2, h2);
		if (bd & BL)
			XftDrawRect(xd, fg, x, y + h2, w2, h - h2);
		if (bd & BR)
			XftDrawRect(xd, fg, x + w2, y + h2, w - w2, h - h2);

	} else if (bd & BBS) {

		int d = (uint8_t)bd;
		XftColor xfc;
		XRenderColor xrc = { .alpha = 0xffff };

		xrc.red = DIV(fg->color.red * d + bg->color.red * (4 - d), 4);
		xrc.green = DIV(fg->color.green * d + bg->color.green * (4 - d), 4);
		xrc.blue = DIV(fg->color.blue * d + bg->color.blue * (4 - d), 4);

		XftColorAllocValue(xdpy, xvis, xcmap, &xrc, &xfc);
		XftDrawRect(xd, &xfc, x, y, w, h);
		XftColorFree(xdpy, xvis, xcmap, &xfc);

	} else if (cat == BRL) {

		int w1 = DIV(w, 2);
		int h1 = DIV(h, 4), h2 = DIV(h, 2), h3 = DIV(3 * h, 4);

		if (bd & 1)   XftDrawRect(xd, fg, x, y, w1, h1);
		if (bd & 2)   XftDrawRect(xd, fg, x, y + h1, w1, h2 - h1);
		if (bd & 4)   XftDrawRect(xd, fg, x, y + h2, w1, h3 - h2);
		if (bd & 8)   XftDrawRect(xd, fg, x + w1, y, w - w1, h1);
		if (bd & 16)  XftDrawRect(xd, fg, x + w1, y + h1, w - w1, h2 - h1);
		if (bd & 32)  XftDrawRect(xd, fg, x + w1, y + h2, w - w1, h3 - h2);
		if (bd & 64)  XftDrawRect(xd, fg, x, y + h3, w1, h - h3);
		if (bd & 128) XftDrawRect(xd, fg, x + w1, y + h3, w - w1, h - h3);

	}
}

void
drawboxlines(int x, int y, int w, int h, XftColor *fg, ushort bd)
{

	int mwh = MIN(w, h);
	int base_s = MAX(1, DIV(mwh, 8));
	int bold = (bd & BDB) && mwh >= 6;
	int s = bold ? MAX(base_s + 1, DIV(3 * base_s, 2)) : base_s;
	int w2 = DIV(w - s, 2), h2 = DIV(h - s, 2);

	int light = bd & (LL | LU | LR | LD);
	int double_ = bd & (DL | DU | DR | DD);

	if (light) {

		int arc = bd & BDA;
		int multi_light = light & (light - 1);
		int multi_double = double_ & (double_ - 1);

		int d = arc || (multi_double && !multi_light) ? -s : 0;

		if (bd & LL)
			XftDrawRect(xd, fg, x, y + h2, w2 + s + d, s);
		if (bd & LU)
			XftDrawRect(xd, fg, x + w2, y, s, h2 + s + d);
		if (bd & LR)
			XftDrawRect(xd, fg, x + w2 - d, y + h2, w - w2 + d, s);
		if (bd & LD)
			XftDrawRect(xd, fg, x + w2, y + h2 - d, s, h - h2 + d);
	}

	if (double_) {

		int dl = bd & DL, du = bd & DU, dr = bd & DR, dd = bd & DD;
		if (dl) {
			int p = dd ? -s : 0, n = du ? -s : dd ? s : 0;
			XftDrawRect(xd, fg, x, y + h2 + s, w2 + s + p, s);
			XftDrawRect(xd, fg, x, y + h2 - s, w2 + s + n, s);
		}
		if (du) {
			int p = dl ? -s : 0, n = dr ? -s : dl ? s : 0;
			XftDrawRect(xd, fg, x + w2 - s, y, s, h2 + s + p);
			XftDrawRect(xd, fg, x + w2 + s, y, s, h2 + s + n);
		}
		if (dr) {
			int p = du ? -s : 0, n = dd ? -s : du ? s : 0;
			XftDrawRect(xd, fg, x + w2 - p, y + h2 - s, w - w2 + p, s);
			XftDrawRect(xd, fg, x + w2 - n, y + h2 + s, w - w2 + n, s);
		}
		if (dd) {
			int p = dr ? -s : 0, n = dl ? -s : dr ? s : 0;
			XftDrawRect(xd, fg, x + w2 + s, y + h2 - p, s, h - h2 + p);
			XftDrawRect(xd, fg, x + w2 - s, y + h2 - n, s, h - h2 + n);
		}
	}
}
