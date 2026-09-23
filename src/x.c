#include <errno.h>
#include <math.h>
#include <limits.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>
#include <libgen.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>
#include <X11/Xft/Xft.h>
#include <X11/XKBlib.h>

char *argv0;
#include "arg.h"
#include "st.h"
#include "win.h"
#include "graphics.h"
#include "hb.h"
#include "st_config.h"
#include <Imlib2.h>

typedef struct {
	uint mod;
	KeySym keysym;
	void (*func)(const Arg *);
	const Arg arg;
} Shortcut;

typedef struct {
	uint mod;
	uint button;
	void (*func)(const Arg *);
	const Arg arg;
	uint  release;
	int   altscrn;
} MouseShortcut;

typedef struct {
	KeySym k;
	uint mask;
	char *s;

	signed char appkey;
	signed char appcursor;
} Key;

#define XK_ANY_MOD    UINT_MAX
#define XK_NO_MOD     0
#define XK_SWITCH_MOD (1<<13|1<<14)

static void clipcopy(const Arg *);
static void clippaste(const Arg *);
static void numlock(const Arg *);
static void selpaste(const Arg *);
static void zoom(const Arg *);
static void zoomabs(const Arg *);
static void zoomreset(const Arg *);
static void ttysend(const Arg *);
static void changealpha(const Arg *);
static void reloadconfig(const Arg *);
void kscrollup(const Arg *);
void kscrolldown(const Arg *);
static void previewimage(const Arg *);
static void showimageinfo(const Arg *);
static void togglegrdebug(const Arg *);
static void dumpgrstate(const Arg *);
static void unloadimages(const Arg *);
static void toggleimages(const Arg *);
void fullscreen(const Arg *);

#include "config.h"

#define XEMBED_FOCUS_IN  4
#define XEMBED_FOCUS_OUT 5

#define IS_SET(flag)		((win.mode & (flag)) != 0)
#define TRUERED(x)		(((x) & 0xff0000) >> 8)
#define TRUEGREEN(x)		(((x) & 0xff00))
#define TRUEBLUE(x)		(((x) & 0xff) << 8)

typedef XftDraw *Draw;
typedef XftColor Color;
typedef XftGlyphFontSpec GlyphFontSpec;

typedef struct {
	int tw, th;
	int w, h;
	int hborderpx, vborderpx;
	int ch;
	int cw;
	int mode;
	int cursor;
} TermWindow;

typedef struct {
	Display *dpy;
	Colormap cmap;
	Window win;
	Drawable buf;
	GlyphFontSpec *specbuf;
	Atom xembed, wmdeletewin, netwmname, netwmiconname, netwmpid,
	     netwmstate, netwmfullscreen;
	Atom XdndTypeList, XdndSelection, XdndEnter, XdndPosition, XdndStatus,
	     XdndLeave, XdndDrop, XdndFinished, XdndActionCopy, XdndActionMove,
	     XdndActionLink, XdndActionAsk, XdndActionPrivate, XtextUriList,
	     XtextPlain, XdndAware;
	Window XdndSourceWin;
	long XdndSourceVersion;
	Atom XdndSourceFormat;
	struct {
		XIM xim;
		XIC xic;
		XPoint spot;
		XVaNestedList spotlist;
	} ime;
	Draw draw;
	Visual *vis;
	XSetWindowAttributes attrs;
	int scr;
	int isfixed;
	int depth;
	int l, t;
	int gm;
} XWindow;

typedef struct {
	Atom xtarget;
	char *primary, *clipboard;
	struct timespec tclick1;
	struct timespec tclick2;
} XSelection;

#define Font Font_
typedef struct {
	int height;
	int width;
	int ascent;
	int descent;
	int badslant;
	int badweight;
	short lbearing;
	short rbearing;
	XftFont *match;
	FcFontSet *set;
	FcPattern *pattern;
} Font;

typedef struct {
	Color *col;
	size_t collen;
	Font font, bfont, ifont, ibfont;
	GC gc;
	Color url_col;
	int has_url_col;
	Color selbg_col;
	int has_selbg_col;
	Color selfg_col;
	int has_selfg_col;
	Color scrollbar_col;
	int has_scrollbar_col;
} DC;

static inline ushort sixd_to_16bit(int);
static void xresetfontsettings(ushort mode, Font **font, int *frcflags);
static int xmakeglyphfontspecs(XftGlyphFontSpec *, const Glyph *, int, int, int);
static void xdrawglyphfontspecs(const XftGlyphFontSpec *, Glyph, int, int, int, int, int);
static void xdrawglyph(Glyph, int, int);
static void xdrawimages(Glyph, Line, int x1, int y1, int x2);
static void xdrawoneimagecell(Glyph, int x, int y);
static void xclear(int, int, int, int);
static int xgeommasktogravity(int);
static int ximopen(Display *);
static void ximinstantiate(Display *, XPointer, XPointer);
static void ximdestroy(XIM, XPointer, XPointer);
static int xicdestroy(XIC, XPointer, XPointer);
static void xinit(int, int);
static void cresize(int, int);
static void xresize(int, int);
static void xhints(void);
static int xloadcolor(int, const char *, Color *);
static int xloadfont(Font *, FcPattern *);
static void xloadfonts(const char *, double);
static int xloadsparefont(FcPattern *, int);
static void xloadsparefonts(void);
static void xunloadfont(Font *);
static void xunloadfonts(void);
static void setnetwmicon(const char *);
static void apply_config(void);
static void xloadalpha(void);
static inline void xsetalpha(Color *);
static void xsetenv(void);
static void xseturgency(int);
static int evcol(XEvent *);
static int evrow(XEvent *);

static void expose(XEvent *);
static void visibility(XEvent *);
static void unmap(XEvent *);
static void kpress(XEvent *);
static void cmessage(XEvent *);
static void xdndenter(XEvent *);
static void xdndpos(XEvent *);
static void xdnddrop(XEvent *);
static void xdndsel(XEvent *);
static void xdndpastedata(char *);
static int xdndurldecode(char *, char *);
static void resize(XEvent *);
static void focus(XEvent *);
static uint buttonmask(uint);
static int mouseaction(XEvent *, uint);
static void brelease(XEvent *);
static void bpress(XEvent *);
static void bmotion(XEvent *);
static void bleave(XEvent *);
static void propnotify(XEvent *);
static void selnotify(XEvent *);
static void selclear_(XEvent *);
static void selrequest(XEvent *);
static void setsel(char *, Time);
static void mousesel(XEvent *, int);
static void mousereport(XEvent *);
static char *kmap(KeySym, uint);
static int match(uint, uint);

static void run(void);
static void usage(void);

static void (*handler[LASTEvent])(XEvent *) = {
	[KeyPress] = kpress,
	[ClientMessage] = cmessage,
	[ConfigureNotify] = resize,
	[VisibilityNotify] = visibility,
	[UnmapNotify] = unmap,
	[Expose] = expose,
	[FocusIn] = focus,
	[FocusOut] = focus,
	[MotionNotify] = bmotion,
	[LeaveNotify] = bleave,
	[ButtonPress] = bpress,
	[ButtonRelease] = brelease,

	[SelectionNotify] = selnotify,

	[PropertyNotify] = propnotify,
	[SelectionRequest] = selrequest,
};

static DC dc;
static XWindow xw;
static XSelection xsel;
static TermWindow win;
enum {
	CURSOR_DEFAULT = 0,
	CURSOR_HAND,
	CURSOR_POINTER
};

static Cursor default_cursor, hand_cursor, pointer_cursor;
static int active_cursor = CURSOR_DEFAULT;
static int scrollbar_hover = 0;
static uint32_t hover_link_id = 0;
static int bpress_col = -1, bpress_row = -1;
static int bpress_dragged = 0;
static int bpress_snap = 0;
static unsigned int mouse_col = 0, mouse_row = 0;
static int scrollbar_dragging = 0;
static int autoscroll_active = 0;
static int autoscroll_delta = 0;
static XEvent autoscroll_last_ev;
static struct timespec last_autoscroll_tick = {0};
static int prev_thumb_y = -1;
static int prev_thumb_h = -1;

enum {
	FRC_NORMAL,
	FRC_ITALIC,
	FRC_BOLD,
	FRC_ITALICBOLD
};

typedef struct {
	XftFont *font;
	int flags;
	Rune unicodep;
} Fontcache;

static Fontcache *frc = NULL;
static int frclen = 0;
static int frccap = 0;
static char *usedfont = NULL;
static double usedfontsize = 0;
static double defaultfontsize = 0;

static char *opt_class = NULL;
static char **opt_cmd  = NULL;
static char *opt_embed = NULL;
static char *opt_font  = NULL;
static char *opt_io    = NULL;
static char *opt_line  = NULL;
static char *opt_name  = NULL;
static char *opt_title = NULL;
static char *opt_config = NULL;
static int cursorblinks = 0;
static int focused = 1;

static uint buttons;

void
clipcopy(const Arg *dummy)
{
	Atom clipboard;

	free(xsel.clipboard);
	xsel.clipboard = NULL;

	if (xsel.primary != NULL) {
		xsel.clipboard = xstrdup(xsel.primary);
		clipboard = XInternAtom(xw.dpy, "CLIPBOARD", 0);
		XSetSelectionOwner(xw.dpy, clipboard, xw.win, CurrentTime);
	}
}

void
clippaste(const Arg *dummy)
{
	Atom clipboard;

	clipboard = XInternAtom(xw.dpy, "CLIPBOARD", 0);
	XConvertSelection(xw.dpy, clipboard, xsel.xtarget, clipboard,
			xw.win, CurrentTime);
}

void
selpaste(const Arg *dummy)
{
	XConvertSelection(xw.dpy, XA_PRIMARY, xsel.xtarget, XA_PRIMARY,
			xw.win, CurrentTime);
}

void
numlock(const Arg *dummy)
{
	win.mode ^= MODE_NUMLOCK;
}

void
zoom(const Arg *arg)
{
	Arg larg;

	larg.f = usedfontsize + arg->f;
	zoomabs(&larg);
}

void
zoomabs(const Arg *arg)
{
	xunloadfonts();
	xloadfonts(usedfont, arg->f);
	xloadsparefonts();
	cresize(0, 0);
	redraw();
	xhints();
}

void
zoomreset(const Arg *arg)
{
	Arg larg;

	if (defaultfontsize > 0) {
		larg.f = defaultfontsize;
		zoomabs(&larg);
	}
}

void
ttysend(const Arg *arg)
{
	ttywrite(arg->s, strlen(arg->s), 1);
}

void
fullscreen(const Arg *arg)
{
	XEvent ev;

	memset(&ev, 0, sizeof(ev));

	ev.xclient.type = ClientMessage;
	ev.xclient.message_type = xw.netwmstate;
	ev.xclient.display = xw.dpy;
	ev.xclient.window = xw.win;
	ev.xclient.format = 32;
	ev.xclient.data.l[0] = 2; /* _NET_WM_STATE_TOGGLE */
	ev.xclient.data.l[1] = xw.netwmfullscreen;
	ev.xclient.data.l[2] = 0;
	ev.xclient.data.l[3] = 1; /* source indication: normal application */

	XSendEvent(xw.dpy, DefaultRootWindow(xw.dpy), False,
	           SubstructureNotifyMask | SubstructureRedirectMask, &ev);
	XFlush(xw.dpy);
}

void
previewimage(const Arg *arg)
{
	Glyph g = getglyphat(mouse_col, mouse_row);
	if (g.mode & ATTR_IMAGE) {
		uint32_t image_id = tgetimgid(&g);
		const char *cmd = g_st_config.image_preview_cmd[0] ? g_st_config.image_preview_cmd : arg->s;
		fprintf(stderr, "Clicked on placeholder %u/%u, x=%d, y=%d\n",
			image_id, tgetimgplacementid(&g), tgetimgcol(&g),
			tgetimgrow(&g));
		gr_preview_image(image_id, cmd);
	}
}

void
showimageinfo(const Arg *arg)
{
	Glyph g = getglyphat(mouse_col, mouse_row);
	if (g.mode & ATTR_IMAGE) {
		uint32_t image_id = tgetimgid(&g);
		fprintf(stderr, "Clicked on placeholder %u/%u, x=%d, y=%d\n",
			image_id, tgetimgplacementid(&g), tgetimgcol(&g),
			tgetimgrow(&g));
		char stcommand[256] = {0};
		size_t len = snprintf(stcommand, sizeof(stcommand), "%s -e less", argv0);
		if (len > sizeof(stcommand) - 1) {
			fprintf(stderr, "Executable name too long: %s\n",
				argv0);
			return;
		}
		gr_show_image_info(image_id, tgetimgplacementid(&g),
				   tgetimgcol(&g), tgetimgrow(&g),
				   tgetisclassicplaceholder(&g),
				   tgetimgdiacriticcount(&g), argv0);
	}
}

void
togglegrdebug(const Arg *arg)
{
	graphics_debug_mode = (graphics_debug_mode + 1) % 3;
	redraw();
}

void
dumpgrstate(const Arg *arg)
{
	gr_dump_state();
}

void
unloadimages(const Arg *arg)
{
	gr_unload_images_to_reduce_ram();
}

void
toggleimages(const Arg *arg)
{
	graphics_display_images = !graphics_display_images;
	redraw();
}

int
evcol(XEvent *e)
{
	int x = e->xbutton.x - win.hborderpx;
	LIMIT(x, 0, win.tw - 1);
	return x / win.cw;
}

int
evrow(XEvent *e)
{
	int y = e->xbutton.y - win.vborderpx;
	LIMIT(y, 0, win.th - 1);
	return y / win.ch;
}

static void
setcursor(int cursor_type)
{
	if (active_cursor == cursor_type)
		return;
	active_cursor = cursor_type;
	if (cursor_type == CURSOR_HAND)
		XDefineCursor(xw.dpy, xw.win, hand_cursor);
	else if (cursor_type == CURSOR_POINTER)
		XDefineCursor(xw.dpy, xw.win, pointer_cursor);
	else
		XDefineCursor(xw.dpy, xw.win, default_cursor);
}

void
mousesel(XEvent *e, int done)
{
	int type, seltype = SEL_REGULAR;
	uint state = e->xbutton.state & ~(Button1Mask | forcemousemod);

	for (type = 1; type < LEN(selmasks); ++type) {
		if (match(selmasks[type], state)) {
			seltype = type;
			break;
		}
	}
	selextend(evcol(e), evrow(e), seltype, done);
	if (done)
		setsel(getsel(), e->xbutton.time);
}

void
mousereport(XEvent *e)
{
	int len, btn, code;
	int x = evcol(e), y = evrow(e);
	int state = e->xbutton.state;
	char buf[40];
	static int ox, oy;

	if (!st_mouse_active())
		return;

	if (e->type == MotionNotify) {
		if (x == ox && y == oy)
			return;
		if (!IS_SET(MODE_MOUSEMOTION) && !IS_SET(MODE_MOUSEMANY))
			return;

		if (IS_SET(MODE_MOUSEMOTION) && buttons == 0)
			return;

		for (btn = 1; btn <= 11 && !(buttons & (1<<(btn-1))); btn++)
			;
		code = 32;
	} else {
		btn = e->xbutton.button;

		if (btn < 1 || btn > 11)
			return;
		if (e->type == ButtonRelease) {

			if (IS_SET(MODE_MOUSEX10))
				return;

			if (btn == 4 || btn == 5)
				return;
		}
		code = 0;
	}

	ox = x;
	oy = y;

	if ((!IS_SET(MODE_MOUSESGR) && e->type == ButtonRelease) || btn == 12)
		code += 3;
	else if (btn >= 8)
		code += 128 + btn - 8;
	else if (btn >= 4)
		code += 64 + btn - 4;
	else
		code += btn - 1;

	if (!IS_SET(MODE_MOUSEX10)) {
		code += ((state & ShiftMask  ) ?  4 : 0)
		      + ((state & Mod1Mask   ) ?  8 : 0)
		      + ((state & ControlMask) ? 16 : 0);
	}

	if (IS_SET(MODE_MOUSESGR)) {
		len = snprintf(buf, sizeof(buf), "\033[<%d;%d;%d%c",
				code, x+1, y+1,
				e->type == ButtonRelease ? 'm' : 'M');
	} else if (x < 223 && y < 223) {
		len = snprintf(buf, sizeof(buf), "\033[M%c%c%c",
				32+code, 32+x+1, 32+y+1);
	} else {
		return;
	}

	ttywrite(buf, len, 0);
}

uint
buttonmask(uint button)
{
	return button == Button1 ? Button1Mask
	     : button == Button2 ? Button2Mask
	     : button == Button3 ? Button3Mask
	     : button == Button4 ? Button4Mask
	     : button == Button5 ? Button5Mask
	     : 0;
}

int
mouseaction(XEvent *e, uint release)
{
	MouseShortcut *ms;

	uint state = e->xbutton.state & ~buttonmask(e->xbutton.button);

	mouse_col = evcol(e);
	mouse_row = evrow(e);

	for (ms = mshortcuts; ms < mshortcuts + LEN(mshortcuts); ms++) {
		if (ms->release == release &&
		    ms->button == e->xbutton.button &&
		    (!ms->altscrn || (ms->altscrn == (tisaltscr() ? 1 : -1))) &&
		    (match(ms->mod, state) ||
		     match(ms->mod, state & ~forcemousemod))) {
			if ((ms->func == kscrollup || ms->func == kscrolldown) &&
			    g_st_config.mouse_scroll_multiplier > 0 && ms->arg.i > 0) {
				Arg a = ms->arg;
				a.i = g_st_config.mouse_scroll_multiplier;
				ms->func(&a);
				XFlush(xw.dpy);
			} else if (ms->func == ttysend && (ms->button == Button4 || ms->button == Button5) &&
			           g_st_config.mouse_scroll_multiplier > 1) {
				for (int k = 0; k < g_st_config.mouse_scroll_multiplier; k++)
					ms->func(&(ms->arg));
			} else {
				ms->func(&(ms->arg));
				if (ms->func == kscrollup || ms->func == kscrolldown)
					XFlush(xw.dpy);
			}
			return 1;
		}
	}

	return 0;
}

void
bpress(XEvent *e)
{
	int btn = e->xbutton.button;
	struct timespec now;
	int snap;

	if (1 <= btn && btn <= 11)
		buttons |= 1 << (btn-1);

	int sw = g_st_config.scrollbar_width > 0 ? g_st_config.scrollbar_width : 4;
	int is_scrollbar_area = (g_st_config.scrollbar != SCROLLBAR_HIDE) &&
	                        (e->xbutton.x >= win.w - (sw + 4)) &&
	                        (get_sb_len() > 0);

	if (is_scrollbar_area) {
		setcursor(CURSOR_POINTER);
		if (btn == Button4) {
			kscrollup(&((Arg){.i = g_st_config.mouse_scroll_multiplier}));
			XFlush(xw.dpy);
			return;
		} else if (btn == Button5) {
			kscrolldown(&((Arg){.i = g_st_config.mouse_scroll_multiplier}));
			XFlush(xw.dpy);
			return;
		} else if (btn == Button1) {
			int rows = win.ch > 0 ? win.th / win.ch : 1;
			int total = get_sb_len() + rows;
			int thumb_h = (int)((long)win.th * rows / total);
			thumb_h = MAX(thumb_h, 24);
			thumb_h = MIN(thumb_h, win.th);
			int available = win.th - thumb_h;
			if (available > 0) {
				int y = e->xbutton.y - win.vborderpx;
				int rel_y = y - thumb_h / 2;
				LIMIT(rel_y, 0, available);
				int new_offset = get_sb_len() - (int)((long)rel_y * get_sb_len() / available);
				LIMIT(new_offset, 0, get_sb_len());
				scrollbar_dragging = 1;
				scrollbar_hover = 1;
				if (new_offset != get_sb_view_offset())
					set_sb_view_offset(new_offset);
				else
					redraw();
				XFlush(xw.dpy);
			}
			return;
		}
	}

	if (st_mouse_active() && !(e->xbutton.state & forcemousemod)) {
		mousereport(e);
		return;
	}

	if (mouseaction(e, 0))
		return;

	if (btn == Button1) {
		bpress_col = evcol(e);
		bpress_row = evrow(e);
		bpress_dragged = 0;

		if (e->xbutton.state & ControlMask) {
			if (openlinkat(evcol(e), evrow(e))) {
				bpress_dragged = 1;
				return;
			}
		}

		clock_gettime(CLOCK_MONOTONIC, &now);
		if (TIMEDIFF(now, xsel.tclick2) <= tripleclicktimeout) {
			snap = SNAP_LINE;
		} else if (TIMEDIFF(now, xsel.tclick1) <= doubleclicktimeout) {
			snap = SNAP_WORD;
		} else {
			snap = 0;
		}
		xsel.tclick2 = xsel.tclick1;
		xsel.tclick1 = now;

		bpress_snap = snap;
		selstart(evcol(e), evrow(e), snap);
	}
}

void
propnotify(XEvent *e)
{
	XPropertyEvent *xpev;
	Atom clipboard = XInternAtom(xw.dpy, "CLIPBOARD", 0);

	xpev = &e->xproperty;
	if (xpev->state == PropertyNewValue &&
			(xpev->atom == XA_PRIMARY ||
			 xpev->atom == clipboard)) {
		selnotify(e);
	}
}

void
selnotify(XEvent *e)
{
	ulong nitems, ofs, rem;
	int format;
	uchar *data, *last, *repl;
	Atom type, incratom, property = None;

	incratom = XInternAtom(xw.dpy, "INCR", 0);

	ofs = 0;
	if (e->type == SelectionNotify)
		property = e->xselection.property;
	else if (e->type == PropertyNotify)
		property = e->xproperty.atom;

	if (property == None)
		return;

	if (property == xw.XdndSelection) {
		xdndsel(e);
		return;
	}

	do {
		if (XGetWindowProperty(xw.dpy, xw.win, property, ofs,
					BUFSIZ/4, False, AnyPropertyType,
					&type, &format, &nitems, &rem,
					&data)) {
			fprintf(stderr, "Clipboard allocation failed\n");
			return;
		}

		if (e->type == PropertyNotify && nitems == 0 && rem == 0) {

			MODBIT(xw.attrs.event_mask, 0, PropertyChangeMask);
			XChangeWindowAttributes(xw.dpy, xw.win, CWEventMask,
					&xw.attrs);
		}

		if (type == incratom) {

			MODBIT(xw.attrs.event_mask, 1, PropertyChangeMask);
			XChangeWindowAttributes(xw.dpy, xw.win, CWEventMask,
					&xw.attrs);

			XDeleteProperty(xw.dpy, xw.win, (int)property);
			continue;
		}

		if (!IS_SET(MODE_BRCKTPASTE)) {
			/* In non-bracketed paste, convert \n to \r for compatibility */
			repl = data;
			last = data + nitems * format / 8;
			while ((repl = memchr(repl, '\n', last - repl))) {
				*repl++ = '\r';
			}
		}

		if (IS_SET(MODE_BRCKTPASTE) && ofs == 0)
			ttywrite("\033[200~", 6, 0);
		ttywrite((char *)data, nitems * format / 8, 0);
		if (IS_SET(MODE_BRCKTPASTE) && rem == 0)
			ttywrite("\033[201~", 6, 0);
		XFree(data);

		ofs += nitems * format / 32;
	} while (rem > 0);

	XDeleteProperty(xw.dpy, xw.win, (int)property);
}

void
xclipcopy(void)
{
	clipcopy(NULL);
}

void
selclear_(XEvent *e)
{
	selclear();
}

void
selrequest(XEvent *e)
{
	XSelectionRequestEvent *xsre;
	XSelectionEvent xev;
	Atom xa_targets, string, clipboard;
	char *seltext;

	xsre = (XSelectionRequestEvent *) e;
	xev.type = SelectionNotify;
	xev.requestor = xsre->requestor;
	xev.selection = xsre->selection;
	xev.target = xsre->target;
	xev.time = xsre->time;
	if (xsre->property == None)
		xsre->property = xsre->target;

	xev.property = None;

	xa_targets = XInternAtom(xw.dpy, "TARGETS", 0);
	if (xsre->target == xa_targets) {

		string = xsel.xtarget;
		XChangeProperty(xsre->display, xsre->requestor, xsre->property,
				XA_ATOM, 32, PropModeReplace,
				(uchar *) &string, 1);
		xev.property = xsre->property;
	} else if (xsre->target == xsel.xtarget || xsre->target == XA_STRING) {

		clipboard = XInternAtom(xw.dpy, "CLIPBOARD", 0);
		if (xsre->selection == XA_PRIMARY) {
			seltext = xsel.primary;
		} else if (xsre->selection == clipboard) {
			seltext = xsel.clipboard;
		} else {
			fprintf(stderr,
				"Unhandled clipboard selection 0x%lx\n",
				xsre->selection);
			return;
		}
		if (seltext != NULL) {
			XChangeProperty(xsre->display, xsre->requestor,
					xsre->property, xsre->target,
					8, PropModeReplace,
					(uchar *)seltext, strlen(seltext));
			xev.property = xsre->property;
		}
	}

	if (!XSendEvent(xsre->display, xsre->requestor, 1, 0, (XEvent *) &xev))
		fprintf(stderr, "Error sending SelectionNotify event\n");
}

void
setsel(char *str, Time t)
{
	if (!str)
		return;

	free(xsel.primary);
	xsel.primary = str;

	XSetSelectionOwner(xw.dpy, XA_PRIMARY, xw.win, t);
	if (XGetSelectionOwner(xw.dpy, XA_PRIMARY) != xw.win)
		selclear();
}

void
xsetsel(char *str)
{
	setsel(str, CurrentTime);
}

void
brelease(XEvent *e)
{
	int btn = e->xbutton.button;

	if (1 <= btn && btn <= 11)
		buttons &= ~(1 << (btn-1));

	autoscroll_active = 0;
	autoscroll_delta = 0;

	if (scrollbar_dragging) {
		scrollbar_dragging = 0;
		int sw = g_st_config.scrollbar_width > 0 ? g_st_config.scrollbar_width : 4;
		int is_sb = (g_st_config.scrollbar != SCROLLBAR_HIDE) &&
		            (e->xbutton.x >= win.w - (sw + 4)) &&
		            (get_sb_len() > 0);
		if (!is_sb) {
			scrollbar_hover = 0;
			setcursor(CURSOR_DEFAULT);
		}
		if (g_st_config.scrollbar == SCROLLBAR_OVERLAY && get_sb_view_offset() == 0 && !is_sb) {
			redraw();
			XFlush(xw.dpy);
		}
		return;
	}

	if (st_mouse_active() && !(e->xbutton.state & forcemousemod)) {
		mousereport(e);
		return;
	}

	if (mouseaction(e, 1))
		return;
	if (btn == Button1) {
		mousesel(e, 1);
		if (!bpress_dragged && bpress_snap == 0 &&
		    evcol(e) == bpress_col && evrow(e) == bpress_row) {
			int mod = g_st_config.url_click_modifiers;
			int match_mod = 0;
			if (mod == URL_CLICK_NONE)
				match_mod = 1;
			else if (mod == URL_CLICK_CTRL && (e->xbutton.state & ControlMask))
				match_mod = 1;
			else if (mod == URL_CLICK_SHIFT && (e->xbutton.state & ShiftMask))
				match_mod = 1;
			else if (mod == URL_CLICK_ALT && (e->xbutton.state & Mod1Mask))
				match_mod = 1;

			if (match_mod && openlinkat(evcol(e), evrow(e)))
				selclear();
		}
	}
}

static void
bleave(XEvent *e)
{
	if (scrollbar_hover) {
		scrollbar_hover = 0;
		if (g_st_config.scrollbar == SCROLLBAR_OVERLAY && get_sb_view_offset() == 0) {
			redraw();
			XFlush(xw.dpy);
		}
	}
	setcursor(CURSOR_DEFAULT);
	if (hover_link_id != 0) {
		hover_link_id = 0;
		if (g_st_config.underline_hyperlinks == URL_UNDERLINE_HOVER) {
			redraw();
			XFlush(xw.dpy);
		}
	}
}

void
bmotion(XEvent *e)
{
	int col = evcol(e);
	int row = evrow(e);
	int sw = g_st_config.scrollbar_width > 0 ? g_st_config.scrollbar_width : 4;
	int in_sb = (g_st_config.scrollbar != SCROLLBAR_HIDE) &&
	            (e->xbutton.x >= win.w - (sw + 4)) &&
	            (get_sb_len() > 0);

	if (scrollbar_dragging) {
		setcursor(CURSOR_POINTER);
		int rows = win.ch > 0 ? win.th / win.ch : 1;
		int total = get_sb_len() + rows;
		int thumb_h = (int)((long)win.th * rows / total);
		thumb_h = MAX(thumb_h, 24);
		thumb_h = MIN(thumb_h, win.th);
		int available = win.th - thumb_h;
		if (available > 0) {
			int y = e->xbutton.y - win.vborderpx;
			int rel_y = y - thumb_h / 2;
			LIMIT(rel_y, 0, available);
			int new_offset = get_sb_len() - (int)((long)rel_y * get_sb_len() / available);
			LIMIT(new_offset, 0, get_sb_len());
			if (new_offset != get_sb_view_offset()) {
				set_sb_view_offset(new_offset);
				XFlush(xw.dpy);
			}
		}
		return;
	}

	if (in_sb) {
		if (!scrollbar_hover) {
			scrollbar_hover = 1;
			if (g_st_config.scrollbar == SCROLLBAR_OVERLAY && get_sb_view_offset() == 0) {
				redraw();
				XFlush(xw.dpy);
			}
		}
		setcursor(CURSOR_POINTER);
		if (hover_link_id != 0) {
			hover_link_id = 0;
			if (g_st_config.underline_hyperlinks == URL_UNDERLINE_HOVER) {
				redraw();
				XFlush(xw.dpy);
			}
		}
		return;
	} else if (scrollbar_hover) {
		scrollbar_hover = 0;
		if (g_st_config.scrollbar == SCROLLBAR_OVERLAY && get_sb_view_offset() == 0) {
			redraw();
			XFlush(xw.dpy);
		}
	}

	int in_grid = (e->xbutton.x >= win.hborderpx &&
	               e->xbutton.x < win.hborderpx + win.tw &&
	               e->xbutton.y >= win.vborderpx &&
	               e->xbutton.y < win.vborderpx + win.th);
	uint32_t new_hover_id = in_grid ? getlinkidat(col, row) : 0;
	int over_link = (new_hover_id > 0);

	if (over_link)
		setcursor(CURSOR_HAND);
	else
		setcursor(CURSOR_DEFAULT);

	if (g_st_config.underline_hyperlinks == URL_UNDERLINE_HOVER) {
		if (new_hover_id != hover_link_id) {
			hover_link_id = new_hover_id;
			redraw();
		}
	} else {
		hover_link_id = new_hover_id;
	}

	if (st_mouse_active() && !(e->xbutton.state & forcemousemod)) {
		if (buttons || IS_SET(MODE_MOUSEMOTION))
			mousereport(e);
		return;
	}

	if (buttons) {
		if (buttons & 1) {
			if (col != bpress_col || row != bpress_row)
				bpress_dragged = 1;

			int y = e->xbutton.y - win.vborderpx;
			if (y < 0) {
				int dist = -y;
				int step = (dist / win.ch) / 2 + 1;
				LIMIT(step, 1, 5);
				kscrollup(&((Arg){.i = step}));
				autoscroll_active = 1;
				autoscroll_delta = -step;
				autoscroll_last_ev = *e;
				autoscroll_last_ev.xbutton.y = win.vborderpx;
				mousesel(&autoscroll_last_ev, 0);
				return;
			} else if (y >= win.th) {
				int dist = y - win.th;
				int step = (dist / win.ch) / 2 + 1;
				LIMIT(step, 1, 5);
				if (get_sb_view_offset() > 0) {
					kscrolldown(&((Arg){.i = step}));
					autoscroll_active = 1;
					autoscroll_delta = step;
					autoscroll_last_ev = *e;
					autoscroll_last_ev.xbutton.y = win.vborderpx + win.th - 1;
					mousesel(&autoscroll_last_ev, 0);
					return;
				}
			} else {
				autoscroll_active = 0;
				autoscroll_delta = 0;
			}
		}
		mousesel(e, 0);
	}
}

void
cresize(int width, int height)
{
	int col, row;

	if (width != 0)
		win.w = width;
	if (height != 0)
		win.h = height;

	col = (win.w - 2 * borderpx) / win.cw;
	row = (win.h - 2 * borderpx) / win.ch;
	col = MAX(1, col);
	row = MAX(1, row);

	win.hborderpx = MAX(0, (win.w - col * win.cw) * anysize_halign / 100);
	win.vborderpx = MAX(0, (win.h - row * win.ch) * anysize_valign / 100);

	tresize(col, row);
	xresize(col, row);
	ttyresize(win.tw, win.th);
}

void
xresize(int col, int row)
{
	win.tw = col * win.cw;
	win.th = row * win.ch;

	XFreePixmap(xw.dpy, xw.buf);
	xw.buf = XCreatePixmap(xw.dpy, xw.win, win.w, win.h,
			xw.depth);
	XftDrawChange(xw.draw, xw.buf);
	xclear(0, 0, win.w, win.h);

	xw.specbuf = xrealloc(xw.specbuf, col * sizeof(GlyphFontSpec) * 4);
}

ushort
sixd_to_16bit(int x)
{
	return x == 0 ? 0 : 0x3737 + 0x2828 * x;
}

int
xloadcolor(int i, const char *name, Color *ncolor)
{
	XRenderColor color = { .alpha = 0xffff };

	if (!name) {
		if (BETWEEN(i, 16, 255)) {
			if (i < 6*6*6+16) {
				color.red   = sixd_to_16bit( ((i-16)/36)%6 );
				color.green = sixd_to_16bit( ((i-16)/6) %6 );
				color.blue  = sixd_to_16bit( ((i-16)/1) %6 );
			} else {
				color.red = 0x0808 + 0x0a0a * (i - (6*6*6+16));
				color.green = color.blue = color.red;
			}
			return XftColorAllocValue(xw.dpy, xw.vis,
			                          xw.cmap, &color, ncolor);
		} else
			name = colorname[i];
	}

	return XftColorAllocName(xw.dpy, xw.vis, xw.cmap, name, ncolor);
}

static Color base_defaultbg;
static int base_defaultbg_set = 0;

static inline void
xsetalpha(Color *color)
{
	float a = (focused || alpha_unfocused < 0.0f) ? alpha : alpha_unfocused;
	LIMIT(a, 0.0f, 1.0f);

	if (base_defaultbg_set) {
		unsigned char r = (unsigned char)(((base_defaultbg.pixel >> 16) & 0xff) * a);
		unsigned char g = (unsigned char)(((base_defaultbg.pixel >> 8) & 0xff) * a);
		unsigned char b = (unsigned char)((base_defaultbg.pixel & 0xff) * a);
		unsigned char a_byte = (unsigned char)(0xff * a);

		color->color.alpha = (unsigned short)(base_defaultbg.color.alpha * a);
		color->color.red   = (unsigned short)(base_defaultbg.color.red * a);
		color->color.green = (unsigned short)(base_defaultbg.color.green * a);
		color->color.blue  = (unsigned short)(base_defaultbg.color.blue * a);
		color->pixel = ((unsigned long)a_byte << 24) | ((unsigned long)r << 16) | ((unsigned long)g << 8) | b;
	} else {
		color->color.alpha = (unsigned short)(0xffff * a);
		color->pixel &= 0x00FFFFFF;
		color->pixel |= (unsigned char)(0xff * a) << 24;
	}
}

void
xloadalpha(void)
{
	xsetalpha(&dc.col[defaultbg]);
	if (dc.gc)
		XSetForeground(xw.dpy, dc.gc, dc.col[defaultbg].pixel);
	if (xw.win)
		XSetWindowBackground(xw.dpy, xw.win, dc.col[defaultbg].pixel);
}

void
changealpha(const Arg *arg)
{
	if (arg->f == 2.0f) {
		alpha = 1.0f;
	} else {
		alpha += arg->f;
		LIMIT(alpha, 0.0f, 1.0f);
	}
	xloadalpha();
	redraw();
}

void
xloadcols(void)
{
	int i;
	static int loaded;
	Color *cp;

	if (loaded) {
		for (cp = dc.col; cp < &dc.col[dc.collen]; ++cp)
			XftColorFree(xw.dpy, xw.vis, xw.cmap, cp);
	} else {
		dc.collen = MAX(LEN(colorname), 256);
		dc.col = xmalloc(dc.collen * sizeof(Color));
	}

	for (i = 0; i < dc.collen; i++)
		if (!xloadcolor(i, NULL, &dc.col[i])) {
			if (colorname[i])
				die("could not allocate color '%s'\n", colorname[i]);
			else
				die("could not allocate color %d\n", i);
		}

	base_defaultbg = dc.col[defaultbg];
	base_defaultbg_set = 1;
	xloadalpha();

	if (dc.has_url_col)
		XftColorFree(xw.dpy, xw.vis, xw.cmap, &dc.url_col);
	dc.has_url_col = 0;
	if (g_st_config.url_color[0]) {
		if (XftColorAllocName(xw.dpy, xw.vis, xw.cmap, g_st_config.url_color, &dc.url_col))
			dc.has_url_col = 1;
	}

	if (dc.has_selbg_col)
		XftColorFree(xw.dpy, xw.vis, xw.cmap, &dc.selbg_col);
	dc.has_selbg_col = 0;
	if (g_st_config.selection_bg[0]) {
		if (XftColorAllocName(xw.dpy, xw.vis, xw.cmap, g_st_config.selection_bg, &dc.selbg_col))
			dc.has_selbg_col = 1;
	}

	if (dc.has_selfg_col)
		XftColorFree(xw.dpy, xw.vis, xw.cmap, &dc.selfg_col);
	dc.has_selfg_col = 0;
	if (g_st_config.selection_fg[0]) {
		if (XftColorAllocName(xw.dpy, xw.vis, xw.cmap, g_st_config.selection_fg, &dc.selfg_col))
			dc.has_selfg_col = 1;
	}

	if (dc.has_scrollbar_col)
		XftColorFree(xw.dpy, xw.vis, xw.cmap, &dc.scrollbar_col);
	dc.has_scrollbar_col = 0;
	if (g_st_config.scrollbar_color[0]) {
		if (XftColorAllocName(xw.dpy, xw.vis, xw.cmap, g_st_config.scrollbar_color, &dc.scrollbar_col))
			dc.has_scrollbar_col = 1;
	}

	loaded = 1;
}

int
xgetcolor(int x, unsigned char *r, unsigned char *g, unsigned char *b)
{
	if (!BETWEEN(x, 0, dc.collen - 1))
		return 1;

	*r = dc.col[x].color.red >> 8;
	*g = dc.col[x].color.green >> 8;
	*b = dc.col[x].color.blue >> 8;

	return 0;
}

int
xsetcolorname(int x, const char *name)
{
	Color ncolor;

	if (!BETWEEN(x, 0, dc.collen - 1))
		return 1;

	if (!xloadcolor(x, name, &ncolor))
		return 1;

	XftColorFree(xw.dpy, xw.vis, xw.cmap, &dc.col[x]);
	dc.col[x] = ncolor;

	if (x == defaultbg) {
		base_defaultbg = dc.col[defaultbg];
		base_defaultbg_set = 1;
		xloadalpha();
	}

	return 0;
}

void
xclear(int x1, int y1, int x2, int y2)
{
	XftDrawRect(xw.draw,
			&dc.col[IS_SET(MODE_REVERSE)? defaultfg : defaultbg],
			x1, y1, x2-x1, y2-y1);
}

void
xhints(void)
{
	XClassHint class = {opt_name ? opt_name : termname,
	                    opt_class ? opt_class : termname};
	XWMHints wm = {.flags = InputHint, .input = 1};
	XSizeHints *sizeh;

	sizeh = XAllocSizeHints();

	sizeh->flags = PSize | PResizeInc | PBaseSize | PMinSize;
	sizeh->height = win.h;
	sizeh->width = win.w;
	sizeh->height_inc = 1;
	sizeh->width_inc = 1;
	sizeh->base_height = 2 * borderpx;
	sizeh->base_width = 2 * borderpx;
	sizeh->min_height = 4 * win.ch + 2 * borderpx;
	sizeh->min_width = 20 * win.cw + 2 * borderpx;
	if (xw.isfixed) {
		sizeh->flags |= PMaxSize;
		sizeh->min_width = sizeh->max_width = win.w;
		sizeh->min_height = sizeh->max_height = win.h;
	}
	if (xw.gm & (XValue|YValue)) {
		sizeh->flags |= USPosition | PWinGravity;
		sizeh->x = xw.l;
		sizeh->y = xw.t;
		sizeh->win_gravity = xgeommasktogravity(xw.gm);
	}

	XSetWMProperties(xw.dpy, xw.win, NULL, NULL, NULL, 0, sizeh, &wm,
			&class);
	XFree(sizeh);
}

int
xgeommasktogravity(int mask)
{
	switch (mask & (XNegative|YNegative)) {
	case 0:
		return NorthWestGravity;
	case XNegative:
		return NorthEastGravity;
	case YNegative:
		return SouthWestGravity;
	}

	return SouthEastGravity;
}

int
xloadfont(Font *f, FcPattern *pattern)
{
	FcPattern *configured;
	FcPattern *match;
	FcResult result;
	XGlyphInfo extents;
	int wantattr, haveattr;

	configured = FcPatternDuplicate(pattern);
	if (!configured)
		return 1;

	FcConfigSubstitute(NULL, configured, FcMatchPattern);
	XftDefaultSubstitute(xw.dpy, xw.scr, configured);

	match = FcFontMatch(NULL, configured, &result);
	if (!match) {
		FcPatternDestroy(configured);
		return 1;
	}

	if (!(f->match = XftFontOpenPattern(xw.dpy, match))) {
		FcPatternDestroy(configured);
		FcPatternDestroy(match);
		return 1;
	}

	if ((XftPatternGetInteger(pattern, "slant", 0, &wantattr) ==
	    XftResultMatch)) {

		if ((XftPatternGetInteger(f->match->pattern, "slant", 0,
		    &haveattr) != XftResultMatch) || haveattr < wantattr) {
			f->badslant = 1;
			fputs("font slant does not match\n", stderr);
		}
	}

	if ((XftPatternGetInteger(pattern, "weight", 0, &wantattr) ==
	    XftResultMatch)) {
		if ((XftPatternGetInteger(f->match->pattern, "weight", 0,
		    &haveattr) != XftResultMatch) || haveattr != wantattr) {
			f->badweight = 1;
			fputs("font weight does not match\n", stderr);
		}
	}

	XftTextExtentsUtf8(xw.dpy, f->match,
		(const FcChar8 *) ascii_printable,
		strlen(ascii_printable), &extents);

	f->set = NULL;
	f->pattern = configured;

	f->ascent = f->match->ascent;
	f->descent = f->match->descent;
	f->lbearing = 0;
	f->rbearing = f->match->max_advance_width;

	f->height = f->ascent + f->descent;
	f->width = DIVCEIL(extents.xOff, strlen(ascii_printable));

	return 0;
}

void
xloadfonts(const char *fontstr, double fontsize)
{
	FcPattern *pattern;
	double fontval;

	if (fontstr[0] == '-')
		pattern = XftXlfdParse(fontstr, False, False);
	else
		pattern = FcNameParse((const FcChar8 *)fontstr);

	if (!pattern)
		die("can't open font %s\n", fontstr);

	if (fontsize > 1) {
		FcPatternDel(pattern, FC_PIXEL_SIZE);
		FcPatternDel(pattern, FC_SIZE);
		FcPatternAddDouble(pattern, FC_PIXEL_SIZE, (double)fontsize);
		usedfontsize = fontsize;
	} else {
		if (FcPatternGetDouble(pattern, FC_PIXEL_SIZE, 0, &fontval) ==
				FcResultMatch) {
			usedfontsize = fontval;
		} else if (FcPatternGetDouble(pattern, FC_SIZE, 0, &fontval) ==
				FcResultMatch) {
			usedfontsize = -1;
		} else {

			FcPatternAddDouble(pattern, FC_PIXEL_SIZE, 12);
			usedfontsize = 12;
		}
		if (defaultfontsize <= 0)
			defaultfontsize = usedfontsize;
	}

	if (xloadfont(&dc.font, pattern))
		die("can't open font %s\n", fontstr);

	if (usedfontsize < 0) {
		FcPatternGetDouble(dc.font.match->pattern,
		                   FC_PIXEL_SIZE, 0, &fontval);
		usedfontsize = fontval;
		if (defaultfontsize <= 0 && fontsize == 0)
			defaultfontsize = fontval;
	}

	win.cw = ceilf(dc.font.width * cwscale);
	win.ch = ceilf(dc.font.height * chscale);

	FcPatternDel(pattern, FC_SLANT);
	FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ITALIC);
	if (xloadfont(&dc.ifont, pattern))
		die("can't open font %s\n", fontstr);

	FcPatternDel(pattern, FC_WEIGHT);
	FcPatternAddInteger(pattern, FC_WEIGHT, FC_WEIGHT_BOLD);
	if (xloadfont(&dc.ibfont, pattern))
		die("can't open font %s\n", fontstr);

	FcPatternDel(pattern, FC_SLANT);
	FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ROMAN);
	if (xloadfont(&dc.bfont, pattern))
		die("can't open font %s\n", fontstr);

	FcPatternDestroy(pattern);
}

int
xloadsparefont(FcPattern *pattern, int flags)
{
	FcPattern *match;
	FcResult result;

	match = FcFontMatch(NULL, pattern, &result);
	if (!match)
		return 1;

	if (frclen >= frccap) {
		frccap += 16;
		frc = xrealloc(frc, frccap * sizeof(Fontcache));
	}

	if (!(frc[frclen].font = XftFontOpenPattern(xw.dpy, match))) {
		FcPatternDestroy(match);
		return 1;
	}

	frc[frclen].flags = flags;
	frc[frclen].unicodep = 0;
	frclen++;

	return 0;
}

void
xloadsparefonts(void)
{
	FcPattern *pattern;
	double sizeshift, fontval;
	int fc;

	if (frclen != 0)
		return;

	fc = LEN(sparefonts);
	if (fc == 0)
		return;

	for (int i = 0; i < fc; ++i) {
		char *fp = sparefonts[i];
		if (!fp || !*fp)
			continue;

		if (*fp == '-')
			pattern = XftXlfdParse(fp, False, False);
		else
			pattern = FcNameParse((FcChar8 *)fp);

		if (!pattern)
			continue;

		if (defaultfontsize > 0) {
			sizeshift = usedfontsize - defaultfontsize;
			if (sizeshift != 0 &&
					FcPatternGetDouble(pattern, FC_PIXEL_SIZE, 0, &fontval) ==
					FcResultMatch) {
				fontval += sizeshift;
				FcPatternDel(pattern, FC_PIXEL_SIZE);
				FcPatternDel(pattern, FC_SIZE);
				FcPatternAddDouble(pattern, FC_PIXEL_SIZE, fontval);
			}
		}

		FcPatternAddBool(pattern, FC_SCALABLE, 1);

		FcConfigSubstitute(NULL, pattern, FcMatchPattern);
		XftDefaultSubstitute(xw.dpy, xw.scr, pattern);

		if (xloadsparefont(pattern, FRC_NORMAL)) {
			FcPatternDestroy(pattern);
			continue;
		}

		FcPatternDel(pattern, FC_SLANT);
		FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ITALIC);
		if (xloadsparefont(pattern, FRC_ITALIC)) {
			FcPatternDestroy(pattern);
			continue;
		}

		FcPatternDel(pattern, FC_WEIGHT);
		FcPatternAddInteger(pattern, FC_WEIGHT, FC_WEIGHT_BOLD);
		if (xloadsparefont(pattern, FRC_ITALICBOLD)) {
			FcPatternDestroy(pattern);
			continue;
		}

		FcPatternDel(pattern, FC_SLANT);
		FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ROMAN);
		if (xloadsparefont(pattern, FRC_BOLD)) {
			FcPatternDestroy(pattern);
			continue;
		}

		FcPatternDestroy(pattern);
	}
}

void
xunloadfont(Font *f)
{
	XftFontClose(xw.dpy, f->match);
	FcPatternDestroy(f->pattern);
	if (f->set)
		FcFontSetDestroy(f->set);
}

void
xunloadfonts(void)
{

	hbunloadfonts();

	while (frclen > 0)
		XftFontClose(xw.dpy, frc[--frclen].font);

	xunloadfont(&dc.font);
	xunloadfont(&dc.bfont);
	xunloadfont(&dc.ifont);
	xunloadfont(&dc.ibfont);
}

int
ximopen(Display *dpy)
{
	XIMCallback imdestroy = { .client_data = NULL, .callback = ximdestroy };
	XICCallback icdestroy = { .client_data = NULL, .callback = xicdestroy };

	xw.ime.xim = XOpenIM(xw.dpy, NULL, NULL, NULL);
	if (xw.ime.xim == NULL)
		return 0;

	if (XSetIMValues(xw.ime.xim, XNDestroyCallback, &imdestroy, NULL))
		fprintf(stderr, "XSetIMValues: "
		                "Could not set XNDestroyCallback.\n");

	xw.ime.spotlist = XVaCreateNestedList(0, XNSpotLocation, &xw.ime.spot,
	                                      NULL);

	if (xw.ime.xic == NULL) {
		xw.ime.xic = XCreateIC(xw.ime.xim, XNInputStyle,
		                       XIMPreeditNothing | XIMStatusNothing,
		                       XNClientWindow, xw.win,
		                       XNDestroyCallback, &icdestroy,
		                       NULL);
	}
	if (xw.ime.xic == NULL)
		fprintf(stderr, "XCreateIC: Could not create input context.\n");

	return 1;
}

void
ximinstantiate(Display *dpy, XPointer client, XPointer call)
{
	if (ximopen(dpy))
		XUnregisterIMInstantiateCallback(xw.dpy, NULL, NULL, NULL,
		                                 ximinstantiate, NULL);
}

void
ximdestroy(XIM xim, XPointer client, XPointer call)
{
	xw.ime.xim = NULL;
	XRegisterIMInstantiateCallback(xw.dpy, NULL, NULL, NULL,
	                               ximinstantiate, NULL);
	XFree(xw.ime.spotlist);
}

int
xicdestroy(XIC xim, XPointer client, XPointer call)
{
	xw.ime.xic = NULL;
	return 1;
}

void
xinit(int cols, int rows)
{
	XGCValues gcvalues;
	Window parent, root;
	pid_t thispid = getpid();
	XColor xmousefg, xmousebg;
	XWindowAttributes attr;
	XVisualInfo vis;

	if (!(xw.dpy = XOpenDisplay(NULL)))
		die("can't open display\n");
	xw.scr = XDefaultScreen(xw.dpy);

	root = XRootWindow(xw.dpy, xw.scr);
	if (!(opt_embed && (parent = strtol(opt_embed, NULL, 0))))
		parent = root;

	if (XMatchVisualInfo(xw.dpy, xw.scr, 32, TrueColor, &vis) != 0) {
		xw.vis = vis.visual;
		xw.depth = vis.depth;
	} else {
		XGetWindowAttributes(xw.dpy, parent, &attr);
		xw.vis = attr.visual;
		xw.depth = attr.depth;
	}

	if (!FcInit())
		die("could not init fontconfig.\n");

	usedfont = (opt_font == NULL)? font : opt_font;
	xloadfonts(usedfont, 0);
	xloadsparefonts();

	xw.cmap = XCreateColormap(xw.dpy, parent, xw.vis, None);
	xloadcols();

	win.w = 2 * win.hborderpx + 2 * borderpx + cols * win.cw;
	win.h = 2 * win.vborderpx + 2 * borderpx + rows * win.ch;
	if (xw.gm & XNegative)
		xw.l += DisplayWidth(xw.dpy, xw.scr) - win.w - 2;
	if (xw.gm & YNegative)
		xw.t += DisplayHeight(xw.dpy, xw.scr) - win.h - 2;

	xw.attrs.background_pixel = dc.col[defaultbg].pixel;
	xw.attrs.border_pixel = dc.col[defaultbg].pixel;
	xw.attrs.bit_gravity = NorthWestGravity;
	xw.attrs.event_mask = FocusChangeMask | KeyPressMask | KeyReleaseMask
		| ExposureMask | VisibilityChangeMask | StructureNotifyMask
		| ButtonMotionMask | ButtonPressMask | ButtonReleaseMask
		| PointerMotionMask | LeaveWindowMask;
	xw.attrs.colormap = xw.cmap;

	xw.win = XCreateWindow(xw.dpy, parent, xw.l, xw.t,
			win.w, win.h, 0, xw.depth, InputOutput,
			xw.vis, CWBackPixel | CWBorderPixel | CWBitGravity
			| CWEventMask | CWColormap, &xw.attrs);
	if (parent != root)
		XReparentWindow(xw.dpy, xw.win, parent, xw.l, xw.t);

	memset(&gcvalues, 0, sizeof(gcvalues));
	gcvalues.graphics_exposures = False;
	dc.gc = XCreateGC(xw.dpy, xw.win, GCGraphicsExposures,
			&gcvalues);
	xw.buf = XCreatePixmap(xw.dpy, xw.win, win.w, win.h,
			xw.depth);
	XSetForeground(xw.dpy, dc.gc, dc.col[defaultbg].pixel);
	XFillRectangle(xw.dpy, xw.buf, dc.gc, 0, 0, win.w, win.h);

	xw.specbuf = xmalloc(cols * sizeof(GlyphFontSpec) * 4);

	xw.draw = XftDrawCreate(xw.dpy, xw.buf, xw.vis, xw.cmap);

	if (!ximopen(xw.dpy)) {
		XRegisterIMInstantiateCallback(xw.dpy, NULL, NULL, NULL,
	                                       ximinstantiate, NULL);
	}

	default_cursor = XCreateFontCursor(xw.dpy, mouseshape);
	hand_cursor = XCreateFontCursor(xw.dpy, XC_hand2);
	pointer_cursor = XCreateFontCursor(xw.dpy, XC_left_ptr);
	XDefineCursor(xw.dpy, xw.win, default_cursor);

	if (XParseColor(xw.dpy, xw.cmap, colorname[mousefg], &xmousefg) == 0) {
		xmousefg.red   = 0xffff;
		xmousefg.green = 0xffff;
		xmousefg.blue  = 0xffff;
	}

	if (XParseColor(xw.dpy, xw.cmap, colorname[mousebg], &xmousebg) == 0) {
		xmousebg.red   = 0x0000;
		xmousebg.green = 0x0000;
		xmousebg.blue  = 0x0000;
	}

	XRecolorCursor(xw.dpy, default_cursor, &xmousefg, &xmousebg);
	XRecolorCursor(xw.dpy, hand_cursor, &xmousefg, &xmousebg);
	XRecolorCursor(xw.dpy, pointer_cursor, &xmousefg, &xmousebg);

	xw.xembed = XInternAtom(xw.dpy, "_XEMBED", False);
	xw.wmdeletewin = XInternAtom(xw.dpy, "WM_DELETE_WINDOW", False);
	xw.netwmname = XInternAtom(xw.dpy, "_NET_WM_NAME", False);
	xw.netwmiconname = XInternAtom(xw.dpy, "_NET_WM_ICON_NAME", False);
	XSetWMProtocols(xw.dpy, xw.win, &xw.wmdeletewin, 1);

	xw.netwmpid = XInternAtom(xw.dpy, "_NET_WM_PID", False);
	XChangeProperty(xw.dpy, xw.win, xw.netwmpid, XA_CARDINAL, 32,
			PropModeReplace, (uchar *)&thispid, 1);

	xw.netwmstate = XInternAtom(xw.dpy, "_NET_WM_STATE", False);
	xw.netwmfullscreen = XInternAtom(xw.dpy, "_NET_WM_STATE_FULLSCREEN", False);

	setnetwmicon(g_st_config.icon);

	const Atom XdndVersion = 5;

	xw.XdndTypeList = XInternAtom(xw.dpy, "XdndTypeList", False);
	xw.XdndSelection = XInternAtom(xw.dpy, "XdndSelection", False);
	xw.XdndEnter = XInternAtom(xw.dpy, "XdndEnter", False);
	xw.XdndPosition = XInternAtom(xw.dpy, "XdndPosition", False);
	xw.XdndStatus = XInternAtom(xw.dpy, "XdndStatus", False);
	xw.XdndLeave = XInternAtom(xw.dpy, "XdndLeave", False);
	xw.XdndDrop = XInternAtom(xw.dpy, "XdndDrop", False);
	xw.XdndFinished = XInternAtom(xw.dpy, "XdndFinished", False);
	xw.XdndActionCopy = XInternAtom(xw.dpy, "XdndActionCopy", False);
	xw.XdndActionMove = XInternAtom(xw.dpy, "XdndActionMove", False);
	xw.XdndActionLink = XInternAtom(xw.dpy, "XdndActionLink", False);
	xw.XdndActionAsk = XInternAtom(xw.dpy, "XdndActionAsk", False);
	xw.XdndActionPrivate = XInternAtom(xw.dpy, "XdndActionPrivate", False);
	xw.XtextUriList = XInternAtom(xw.dpy, "text/uri-list", False);
	xw.XtextPlain = XInternAtom(xw.dpy, "text/plain", False);
	xw.XdndAware = XInternAtom(xw.dpy, "XdndAware", False);
	XChangeProperty(xw.dpy, xw.win, xw.XdndAware, XA_ATOM, 32, PropModeReplace,
			(unsigned char *)&XdndVersion, 1);

	win.mode = MODE_NUMLOCK;
	resettitle();
	xhints();
	XMapWindow(xw.dpy, xw.win);
	XSync(xw.dpy, False);

	clock_gettime(CLOCK_MONOTONIC, &xsel.tclick1);
	clock_gettime(CLOCK_MONOTONIC, &xsel.tclick2);
	xsel.primary = NULL;
	xsel.clipboard = NULL;
	xsel.xtarget = XInternAtom(xw.dpy, "UTF8_STRING", 0);
	if (xsel.xtarget == None)
		xsel.xtarget = XA_STRING;

	gr_init(xw.dpy, xw.vis, xw.cmap);

	boxdraw_xinit(xw.dpy, xw.cmap, xw.draw, xw.vis);
}

void
xresetfontsettings(ushort mode, Font **font, int *frcflags)
{
	*font = &dc.font;
	if ((mode & ATTR_ITALIC) && (mode & ATTR_BOLD)) {
		*font = &dc.ibfont;
		*frcflags = FRC_ITALICBOLD;
	} else if (mode & ATTR_ITALIC) {
		*font = &dc.ifont;
		*frcflags = FRC_ITALIC;
	} else if (mode & ATTR_BOLD) {
		*font = &dc.bfont;
		*frcflags = FRC_BOLD;
	}
}

int
xmakeglyphfontspecs(XftGlyphFontSpec *specs, const Glyph *glyphs, int len, int x, int y)
{
	float winx = win.hborderpx + x * win.cw, winy = win.vborderpx + y * win.ch, xp, yp;
	ushort mode, prevmode = USHRT_MAX;
	Font *font = &dc.font;
	int frcflags = FRC_NORMAL;
	float runewidth = win.cw;
	Rune rune;
	FT_UInt glyphidx;
	FcResult fcres;
	FcPattern *fcpattern, *fontpattern;
	FcFontSet *fcsets[] = { NULL };
	FcCharSet *fccharset;
	int i, f, length = 0, start = 0, numspecs = 0;
	float cluster_xp = xp, cluster_yp = yp;
	HbTransformData shaped = { 0 };

	mode = prevmode = glyphs[0].mode & ~ATTR_WRAP;
	xresetfontsettings(mode, &font, &frcflags);

	for (i = 0, xp = winx, yp = winy + font->ascent; i < len; ++i) {
		mode = glyphs[i].mode & ~ATTR_WRAP;

		if (mode & ATTR_WDUMMY && i < (len - 1))
			continue;

		if (
			prevmode != mode
			|| ATTRCMP(glyphs[start], glyphs[i])
			|| selected(x + i, y) != selected(x + start, y)
			|| i == (len - 1)
		) {

			length = i - start;
			if (i == start) {
				length = 1;
			} else if (i == (len - 1)) {
				length = (i - start + 1);
			}

			hbtransform(&shaped, font->match, glyphs, start, length);
			runewidth = win.cw * ((glyphs[start].mode & ATTR_WIDE) ? 2.0f : 1.0f);
			cluster_xp = xp; cluster_yp = yp;
			for (int code_idx = 0; code_idx < shaped.count; code_idx++) {
				int idx = shaped.glyphs[code_idx].cluster;

				if (glyphs[start + idx].mode & ATTR_WDUMMY)
					continue;

				if (glyphs[start + idx].mode & ATTR_IMAGE)
					continue;

				if (code_idx > 0 && idx != shaped.glyphs[code_idx - 1].cluster) {
					xp += runewidth;
					cluster_xp = xp;
					cluster_yp = yp;
					runewidth = win.cw * ((glyphs[start + idx].mode & ATTR_WIDE) ? 2.0f : 1.0f);
				}

				if (glyphs[start + idx].mode & ATTR_BOXDRAW) {

					specs[numspecs].font = font->match;
					specs[numspecs].glyph = boxdrawindex(&glyphs[start + idx]);
					specs[numspecs].x = xp;
					specs[numspecs].y = yp;
					numspecs++;
				} else if (shaped.glyphs[code_idx].codepoint != 0) {

					specs[numspecs].font = font->match;
					specs[numspecs].glyph = shaped.glyphs[code_idx].codepoint;
					specs[numspecs].x = cluster_xp + (short)(shaped.positions[code_idx].x_offset / 64.);
					specs[numspecs].y = cluster_yp - (short)(shaped.positions[code_idx].y_offset / 64.);
					cluster_xp += shaped.positions[code_idx].x_advance / 64.;
					cluster_yp += shaped.positions[code_idx].y_advance / 64.;
					numspecs++;
				} else {

					rune = glyphs[start + idx].u;
					if (rune == 0 || BETWEEN(rune, 0xD800, 0xDFFF) || rune > 0x10FFFF)
						continue;

					for (f = 0; f < frclen; f++) {
						glyphidx = XftCharIndex(xw.dpy, frc[f].font, rune);

						if (glyphidx && frc[f].flags == frcflags)
							break;

						if (!glyphidx && frc[f].flags == frcflags
								&& frc[f].unicodep == rune) {
							break;
						}
					}

					if (f >= frclen) {
						if (!font->set && font->pattern)
							font->set = FcFontSort(0, font->pattern, 1, 0, &fcres);
						fcsets[0] = font->set;

						fcpattern = font->pattern ? FcPatternDuplicate(font->pattern) : NULL;
						fccharset = FcCharSetCreate();

						if (fcpattern && fccharset && font->set) {
							FcCharSetAddChar(fccharset, rune);
							FcPatternAddCharSet(fcpattern, FC_CHARSET, fccharset);
							FcPatternAddBool(fcpattern, FC_SCALABLE, 1);
							FcConfigSubstitute(0, fcpattern, FcMatchPattern);
							FcDefaultSubstitute(fcpattern);
							fontpattern = FcFontSetMatch(0, fcsets, 1, fcpattern, &fcres);
						} else {
							fontpattern = NULL;
						}

						if (frclen >= frccap) {
							frccap += 16;
							frc = xrealloc(frc, frccap * sizeof(Fontcache));
						}

						frc[frclen].font = fontpattern ? XftFontOpenPattern(xw.dpy, fontpattern) : NULL;
						if (!frc[frclen].font)
							frc[frclen].font = font->match;
						frc[frclen].flags = frcflags;
						frc[frclen].unicodep = rune;

						glyphidx = XftCharIndex(xw.dpy, frc[frclen].font, rune);

						f = frclen;
						frclen++;

						if (fcpattern)
							FcPatternDestroy(fcpattern);
						if (fccharset)
							FcCharSetDestroy(fccharset);
					}

					specs[numspecs].font = frc[f].font;
					specs[numspecs].glyph = glyphidx;
					specs[numspecs].x = (short)xp;
					specs[numspecs].y = (short)yp;
					numspecs++;
				}
			}

			hbcleanup(&shaped);
			start = i;

			if (prevmode != mode) {
				prevmode = mode;
				xresetfontsettings(mode, &font, &frcflags);
				yp = winy + font->ascent;
			}
		}
	}

	hbcleanup(&shaped);
	return numspecs;
}

static void
xdrawunderdashed(Draw draw, Color *color, int x, int y, int w,
		 int wavelen, float fraction, int thick)
{
	int dashw = MAX(1, fraction * wavelen);
	for (int i = x - x % wavelen; i < x + w; i += wavelen) {
		int startx = MAX(i, x);
		int endx = MIN(i + dashw, x + w);
		if (startx < endx)
			XftDrawRect(xw.draw, color, startx, y, endx - startx,
				    thick);
	}
}

static void
xdrawundercurl(Draw draw, Color *color, int x, int y, int w, int h, int thick)
{
	XGCValues gcvals = {.foreground = color->pixel,
			    .line_width = thick,
			    .line_style = LineSolid,
			    .cap_style = CapRound,
			    .join_style = JoinRound};
	GC gc = XCreateGC(xw.dpy, XftDrawDrawable(xw.draw),
			  GCForeground | GCLineWidth | GCLineStyle | GCCapStyle | GCJoinStyle,
			  &gcvals);

	XRectangle clip = {.x = x, .y = y, .width = w, .height = h};
	XSetClipRectangles(xw.dpy, gc, 0, 0, &clip, 1, Unsorted);

	int yoffset = thick / 2;
	int segh = MAX(2, h - thick);
	int wavelen = (g_st_config.undercurl_style == UNDERCURL_DENSE) ? MAX(4, win.cw / 2) : MAX(4, win.cw);

	for (int i = x - (x % wavelen); i < x + w; i += wavelen) {
		XPoint points[5] = {
			{.x = i,                   .y = y + yoffset + segh / 2},
			{.x = i + wavelen / 4,     .y = y + yoffset},
			{.x = i + wavelen / 2,     .y = y + yoffset + segh / 2},
			{.x = i + 3 * wavelen / 4, .y = y + yoffset + segh},
			{.x = i + wavelen,         .y = y + yoffset + segh / 2},
		};
		XDrawLines(xw.dpy, XftDrawDrawable(xw.draw), gc, points, 5,
			   CoordModeOrigin);
	}

	XFreeGC(xw.dpy, gc);
}

void
xdrawglyphfontspecs(const XftGlyphFontSpec *specs, Glyph base, int len, int x, int y, int charlen, int dmode)
{
	int winx = win.hborderpx + x * win.cw, winy = win.vborderpx + y * win.ch,
	    width = charlen * win.cw;
	Color *fg, *bg, *temp, revfg, revbg, truefg, truebg;
	XRenderColor colfg, colbg;
	XRectangle r;

	if (base.mode & ATTR_ITALIC && base.mode & ATTR_BOLD) {
		if (dc.ibfont.badslant || dc.ibfont.badweight)
			base.fg = defaultattr;
	} else if ((base.mode & ATTR_ITALIC && dc.ifont.badslant) ||
	    (base.mode & ATTR_BOLD && dc.bfont.badweight)) {
		base.fg = defaultattr;
	}

	if (IS_TRUECOL(base.fg)) {
		colfg.alpha = 0xffff;
		colfg.red = TRUERED(base.fg);
		colfg.green = TRUEGREEN(base.fg);
		colfg.blue = TRUEBLUE(base.fg);
		XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colfg, &truefg);
		fg = &truefg;
	} else if (base.fg < dc.collen) {
		fg = &dc.col[base.fg];
	} else {
		fg = &dc.col[defaultfg];
	}

	if (IS_TRUECOL(base.bg)) {
		colbg.alpha = 0xffff;
		colbg.green = TRUEGREEN(base.bg);
		colbg.red = TRUERED(base.bg);
		colbg.blue = TRUEBLUE(base.bg);
		XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colbg, &truebg);
		bg = &truebg;
	} else if (base.bg < dc.collen) {
		bg = &dc.col[base.bg];
	} else {
		bg = &dc.col[defaultbg];
	}

	if (!bold_is_not_bright && (base.mode & ATTR_BOLD_FAINT) == ATTR_BOLD && BETWEEN(base.fg, 0, 7))
		fg = &dc.col[base.fg + 8];

	if (IS_SET(MODE_REVERSE)) {
		if (fg == &dc.col[defaultfg]) {
			fg = &dc.col[defaultbg];
		} else {
			colfg.red = ~fg->color.red;
			colfg.green = ~fg->color.green;
			colfg.blue = ~fg->color.blue;
			colfg.alpha = fg->color.alpha;
			XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colfg,
					&revfg);
			fg = &revfg;
		}

		if (bg == &dc.col[defaultbg]) {
			bg = &dc.col[defaultfg];
		} else {
			colbg.red = ~bg->color.red;
			colbg.green = ~bg->color.green;
			colbg.blue = ~bg->color.blue;
			colbg.alpha = bg->color.alpha;
			XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colbg,
					&revbg);
			bg = &revbg;
		}
	}

	if ((base.mode & ATTR_BOLD_FAINT) == ATTR_FAINT) {
		colfg.red = fg->color.red / 2;
		colfg.green = fg->color.green / 2;
		colfg.blue = fg->color.blue / 2;
		colfg.alpha = fg->color.alpha;
		XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colfg, &revfg);
		fg = &revfg;
	}

	if (base.mode & ATTR_REVERSE) {
		temp = fg;
		fg = bg;
		bg = temp;
	}

	if (base.mode & ATTR_SELECTED) {
		if (dc.has_selbg_col)
			bg = &dc.selbg_col;
		if (dc.has_selfg_col)
			fg = &dc.selfg_col;
	}

	if (base.mode & ATTR_BLINK && win.mode & MODE_BLINK)
		fg = bg;

	if (base.mode & ATTR_INVISIBLE)
		fg = bg;

	if (dmode & DRAW_BG) {
		if (x == 0) {
			xclear(0, (y == 0)? 0 : winy, win.hborderpx,
			       winy + win.ch +
			       ((winy + win.ch >= win.vborderpx + win.th)? win.h : 0));
		}
		if (winx + width >= win.hborderpx + win.tw) {
			xclear(winx + width, (y == 0)? 0 : winy, win.w,
			       ((winy + win.ch >= win.vborderpx + win.th)? win.h : (winy + win.ch)));
		}
		if (y == 0)
			xclear(winx, 0, winx + width, win.vborderpx);
		if (winy + win.ch >= win.vborderpx + win.th)
			xclear(winx, winy + win.ch, winx + width, win.h);

		XftDrawRect(xw.draw, bg, winx, winy, width, win.ch);
	}

	if (!(dmode & DRAW_FG))
		return;

	r.x = 0;
	r.y = 0;
	r.height = win.ch;
	r.width = width;
	XftDrawSetClipRectangles(xw.draw, winx, winy, &r, 1);

	int is_hyperlink = (base.mode & ATTR_HYPERLINK);
	int is_hovered = (hover_link_id > 0 && base.link_id == hover_link_id);
	int draw_url_style = 0;

	if (is_hyperlink) {
		if (g_st_config.underline_hyperlinks == URL_UNDERLINE_ALWAYS)
			draw_url_style = 1;
		else if (g_st_config.underline_hyperlinks == URL_UNDERLINE_HOVER && is_hovered)
			draw_url_style = 1;
	}

	Color decor;
	uint32_t decorcolor = tgetdecorcolor(&base);
	if (draw_url_style && dc.has_url_col) {
		decor = dc.url_col;
	} else if (decorcolor == DECOR_DEFAULT_COLOR) {
		decor = *fg;
	} else if (IS_TRUECOL(decorcolor)) {
		colfg.alpha = 0xffff;
		colfg.red = TRUERED(decorcolor);
		colfg.green = TRUEGREEN(decorcolor);
		colfg.blue = TRUEBLUE(decorcolor);
		XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colfg, &decor);
	} else if (decorcolor < dc.collen) {
		decor = dc.col[decorcolor];
	} else {
		decor = *fg;
	}
	decor.color.alpha = 0xffff;
	decor.pixel |= (unsigned long)0xff << 24;

	float fthick = dc.font.height / 18.0;

	int thick = MAX(1, roundf(fthick));

	int gap = roundf(fthick * 2);

	int doubleh = thick * 2 + ceilf(fthick * 0.5);

	int curlh = thick * 2 + roundf(fthick * 0.75);

	if ((base.mode & ATTR_UNDERLINE) || draw_url_style) {
		uint32_t style = tgetdecorstyle(&base);
		if (draw_url_style && (style == 0 || style == UNDERLINE_STRAIGHT))
			style = (g_st_config.url_style > 0) ? g_st_config.url_style : UNDERLINE_CURLY;
		int liney = winy + dc.font.ascent + gap;

		liney -= MAX(0, liney + thick - (winy + win.ch));
		if (style == UNDERLINE_DOUBLE) {
			liney -= MAX(0, liney + doubleh - (winy + win.ch));
			XftDrawRect(xw.draw, &decor, winx, liney, width, thick);
			XftDrawRect(xw.draw, &decor, winx,
				    liney + doubleh - thick, width, thick);
		} else if (style == UNDERLINE_DOTTED) {
			xdrawunderdashed(xw.draw, &decor, winx, liney, width,
					 thick * 2, 0.5, thick);
		} else if (style == UNDERLINE_DASHED) {
			int wavelen = MAX(2, win.cw * 0.9);
			xdrawunderdashed(xw.draw, &decor, winx, liney, width,
					 wavelen, 0.65, thick);
		} else if (style == UNDERLINE_CURLY) {
			liney -= MAX(0, liney + curlh - (winy + win.ch));
			xdrawundercurl(xw.draw, &decor, winx, liney, width,
				       curlh, thick);
		} else {
			XftDrawRect(xw.draw, &decor, winx, liney, width, thick);
		}
	}

	XftDrawSetClip(xw.draw, 0);

	if (len > 0) {
		if (base.mode & ATTR_BOXDRAW) {
			drawboxes(winx, winy, win.cw, win.ch, fg, bg, specs, len);
		} else {
			XftDrawGlyphFontSpec(xw.draw, fg, specs, len);
		}
	}

	if (base.mode & ATTR_STRUCK) {
		XftDrawRect(xw.draw, fg, winx, winy + 2 * dc.font.ascent / 3,
			    width, thick);
	}
}

void
xdrawglyph(Glyph g, int x, int y)
{
	int numspecs;
	XftGlyphFontSpec *specs = xw.specbuf;

	numspecs = xmakeglyphfontspecs(specs, &g, 1, x, y);
	xdrawglyphfontspecs(specs, g, numspecs, x, y, (g.mode & ATTR_WIDE) ? 2 : 1, DRAW_BG | DRAW_FG);
	if (g.mode & ATTR_IMAGE) {
		gr_start_drawing(xw.buf, win.cw, win.ch);
		xdrawoneimagecell(g, x, y);
		gr_finish_drawing(xw.buf);
	}
}

void
xdrawcursor(int cx, int cy, Glyph g, int ox, int oy, Glyph og, Line line, int len)
{
	Color drawcol;
	XRenderColor colbg;

	if (selected(ox, oy)) {
		if (dc.has_selbg_col)
			og.mode |= ATTR_SELECTED;
		else
			og.mode ^= ATTR_REVERSE;
	}

	gr_start_drawing(xw.buf, win.cw, win.ch);
	xdrawline(line, 0, oy, len);
	gr_finish_drawing(xw.buf);

	if (IS_SET(MODE_HIDE))
		return;

	if (g.mode & ATTR_IMAGE)
		g.u = 0x2610;

	g.mode &= ATTR_BOLD|ATTR_ITALIC|ATTR_UNDERLINE|ATTR_STRUCK|ATTR_WIDE|ATTR_BOXDRAW;

	if (IS_SET(MODE_REVERSE)) {
		g.mode |= ATTR_REVERSE;
		g.bg = defaultfg;
		if (selected(cx, cy)) {
			drawcol = dc.col[defaultcs];
			g.fg = defaultrcs;
		} else {
			drawcol = dc.col[defaultrcs];
			g.fg = defaultcs;
		}
	} else {
		if (selected(cx, cy)) {
			g.fg = dc.has_selfg_col ? defaultrcs : defaultfg;
			g.bg = dc.has_selbg_col ? defaultcs : defaultrcs;
		} else if (cursor_dynamic_color && !(og.mode & (ATTR_REVERSE | ATTR_SELECTED))) {
			unsigned long col = g.bg;
			g.bg = g.fg;
			g.fg = col;
		} else {
			g.fg = defaultrcs;
			g.bg = defaultcs;
		}

		if (cursor_dynamic_color && IS_TRUECOL(g.bg)) {
			colbg.alpha = 0xffff;
			colbg.red = TRUERED(g.bg);
			colbg.green = TRUEGREEN(g.bg);
			colbg.blue = TRUEBLUE(g.bg);
			XftColorAllocValue(xw.dpy, xw.vis, xw.cmap, &colbg, &drawcol);
		} else {
			drawcol = dc.col[g.bg];
		}
	}

	if (IS_SET(MODE_FOCUSED)) {
		switch (win.cursor) {
		case 7:
			g.u = 0x2603;

		case 0:
		case 1:
			if (IS_SET(MODE_BLINK))
				break;

		case 2:
			xdrawglyph(g, cx, cy);
			break;
		case 3:
			if (IS_SET(MODE_BLINK))
				break;

		case 4:
			XftDrawRect(xw.draw, &drawcol,
					win.hborderpx + cx * win.cw,
					win.vborderpx + (cy + 1) * win.ch - \
						cursorthickness,
					win.cw, cursorthickness);
			break;
		case 5:
			if (IS_SET(MODE_BLINK))
				break;

		case 6:
			XftDrawRect(xw.draw, &drawcol,
					win.hborderpx + cx * win.cw,
					win.vborderpx + cy * win.ch,
					cursorthickness, win.ch);
			break;
		}
	} else {
		XftDrawRect(xw.draw, &drawcol,
				win.hborderpx + cx * win.cw,
				win.vborderpx + cy * win.ch,
				win.cw - 1, 1);
		XftDrawRect(xw.draw, &drawcol,
				win.hborderpx + cx * win.cw,
				win.vborderpx + cy * win.ch,
				1, win.ch - 1);
		XftDrawRect(xw.draw, &drawcol,
				win.hborderpx + (cx + 1) * win.cw - 1,
				win.vborderpx + cy * win.ch,
				1, win.ch - 1);
		XftDrawRect(xw.draw, &drawcol,
				win.hborderpx + cx * win.cw,
				win.vborderpx + (cy + 1) * win.ch - 1,
				win.cw, 1);
	}
}

void
xdrawimages(Glyph base, Line line, int x1, int y1, int x2) {
	int y_pix = win.vborderpx + y1 * win.ch;
	uint32_t image_id_24bits = base.fg & 0xFFFFFF;
	uint32_t placement_id = tgetimgplacementid(&base);

	int last_col = 0;
	int last_row = 0;
	int last_start_col = 0;
	int last_start_x = x1;

	uint32_t last_id_4thbyteplus1 = 0;

	Glyph *prev = &line[x1 - 1];
	if (x1 > 0 && (prev->mode & ATTR_IMAGE) &&
	    (prev->fg & 0xFFFFFF) == image_id_24bits &&
	    prev->decor == base.decor) {
		last_row = tgetimgrow(prev);
		last_col = tgetimgcol(prev);
		last_id_4thbyteplus1 = tgetimgid4thbyteplus1(prev);
		last_start_col = last_col + 1;
	}
	for (int x = x1; x < x2; ++x) {
		Glyph *g = &line[x];
		uint32_t cur_row = tgetimgrow(g);
		uint32_t cur_col = tgetimgcol(g);
		uint32_t cur_id_4thbyteplus1 = tgetimgid4thbyteplus1(g);
		uint32_t num_diacritics = tgetimgdiacriticcount(g);

		if (last_row && (num_diacritics == 0 || !cur_row))
			cur_row = last_row;

		if (last_col && (num_diacritics <= 1 || !cur_col) &&
		    cur_row == last_row)
			cur_col = last_col + 1;

		if (last_id_4thbyteplus1 &&
		    (num_diacritics <= 2 || !cur_id_4thbyteplus1) &&
		    cur_row == last_row && cur_col == last_col + 1)
			cur_id_4thbyteplus1 = last_id_4thbyteplus1;

		if (cur_row == 0)
			cur_row = 1;
		if (cur_col == 0)
			cur_col = 1;

		if (cur_col != last_col + 1 || cur_row != last_row ||
		    cur_id_4thbyteplus1 != last_id_4thbyteplus1) {
			uint32_t image_id = image_id_24bits;
			if (last_id_4thbyteplus1)
				image_id |= (last_id_4thbyteplus1 - 1) << 24;
			if (last_row != 0) {
				int x_pix =
					win.hborderpx + last_start_x * win.cw;
				gr_append_imagerect(
					xw.buf, image_id, placement_id,
					last_start_col - 1, last_col,
					last_row - 1, last_row, last_start_x,
					y1, x_pix, y_pix, win.cw, win.ch,
					base.mode & ATTR_REVERSE);
			}
			last_start_col = cur_col;
			last_start_x = x;
		}
		last_row = cur_row;
		last_col = cur_col;
		last_id_4thbyteplus1 = cur_id_4thbyteplus1;

		if (!tgetimgrow(g))
			tsetimgrow(g, cur_row);

		if (!tgetimgcol(g) && (cur_col & ~0x1ff) == 0)
			tsetimgcol(g, cur_col);
		if (!tgetimgid4thbyteplus1(g))
			tsetimg4thbyteplus1(g, cur_id_4thbyteplus1);
	}
	uint32_t image_id = image_id_24bits;
	if (last_id_4thbyteplus1)
		image_id |= (last_id_4thbyteplus1 - 1) << 24;

	if (last_row != 0) {
		int x_pix = win.hborderpx + last_start_x * win.cw;
		gr_append_imagerect(xw.buf, image_id, placement_id,
				    last_start_col - 1, last_col, last_row - 1,
				    last_row, last_start_x, y1, x_pix, y_pix,
				    win.cw, win.ch, base.mode & ATTR_REVERSE);
	}
}

void xdrawoneimagecell(Glyph g, int x, int y) {
	if (!(g.mode & ATTR_IMAGE))
		return;
	int x_pix = win.hborderpx + x * win.cw;
	int y_pix = win.vborderpx + y * win.ch;
	uint32_t row = tgetimgrow(&g) - 1;
	uint32_t col = tgetimgcol(&g) - 1;
	uint32_t placement_id = tgetimgplacementid(&g);
	uint32_t image_id = tgetimgid(&g);
	gr_append_imagerect(xw.buf, image_id, placement_id, col, col + 1, row,
			    row + 1, x, y, x_pix, y_pix, win.cw, win.ch,
			    g.mode & ATTR_REVERSE);
}

void xstartimagedraw(int *dirty, int rows) {
	gr_start_drawing(xw.buf, win.cw, win.ch);
	gr_mark_dirty_animations(dirty, rows);
}

void xfinishimagedraw() {
	gr_finish_drawing(xw.buf);
}

void
xsetenv(void)
{
	char buf[sizeof(long) * 8 + 1];

	snprintf(buf, sizeof(buf), "%lu", xw.win);
	setenv("WINDOWID", buf, 1);
}

void
xseticontitle(char *p)
{
	XTextProperty prop;
	DEFAULT(p, opt_title);

	if (p[0] == '\0')
		p = opt_title;

	if (Xutf8TextListToTextProperty(xw.dpy, &p, 1, XUTF8StringStyle,
	                                &prop) != Success)
		return;
	XSetWMIconName(xw.dpy, xw.win, &prop);
	XSetTextProperty(xw.dpy, xw.win, &prop, xw.netwmiconname);
	XFree(prop.value);
}

void
xsettitle(char *p)
{
	XTextProperty prop;
	DEFAULT(p, opt_title);

	if (p[0] == '\0')
		p = opt_title;

	if (Xutf8TextListToTextProperty(xw.dpy, &p, 1, XUTF8StringStyle,
	                                &prop) != Success)
		return;
	XSetWMName(xw.dpy, xw.win, &prop);
	XSetTextProperty(xw.dpy, xw.win, &prop, xw.netwmname);
	XFree(prop.value);
}

int
xstartdraw(void)
{
	return IS_SET(MODE_VISIBLE);
}

void
xdrawline(Line line, int x1, int y1, int x2)
{
	int i, x, ox, numspecs;
	Glyph base, new;
	XftGlyphFontSpec *specs;

	for (int dmode = DRAW_BG; dmode <= DRAW_FG; dmode <<= 1) {
		specs = xw.specbuf;
		i = ox = 0;
		for (x = x1; x < x2; x++) {
			new = line[x];
			if (new.mode == ATTR_WDUMMY)
				continue;
			if (selected(x, y1)) {
				if (dc.has_selbg_col)
					new.mode |= ATTR_SELECTED;
				else
					new.mode ^= ATTR_REVERSE;
			}
			if (i > 0 && ATTRCMP(base, new)) {
				numspecs = xmakeglyphfontspecs(specs, &line[ox], x - ox, ox, y1);
				xdrawglyphfontspecs(specs, base, numspecs, ox, y1, x - ox, dmode);
				if (base.mode & ATTR_IMAGE && dmode == DRAW_FG)
					xdrawimages(base, line, ox, y1, x);
				i = 0;
			}
			if (i == 0) {
				ox = x;
				base = new;
			}
			i++;
		}
		if (i > 0) {
			numspecs = xmakeglyphfontspecs(specs, &line[ox], x2 - ox, ox, y1);
			xdrawglyphfontspecs(specs, base, numspecs, ox, y1, x2 - ox, dmode);
		}
		if (i > 0 && base.mode & ATTR_IMAGE && dmode == DRAW_FG)
			xdrawimages(base, line, ox, y1, x);
	}
}

void
drawscrollbar(void)
{
	int sblen = get_sb_len();
	int view_offset = get_sb_view_offset();
	int mode = g_st_config.scrollbar;
	int sw = g_st_config.scrollbar_width > 0 ? g_st_config.scrollbar_width : 4;
	int rows = win.ch > 0 ? win.th / win.ch : 1;
	int total, thumb_h, available, thumb_y, sx;
	XftColor *thumb_col;

	sx = win.w - sw - 1;

	if (mode == SCROLLBAR_HIDE || sblen <= 0) {
		if (prev_thumb_y != -1) {
			xclear(sx, prev_thumb_y, sx + sw, prev_thumb_y + prev_thumb_h);
			prev_thumb_y = -1;
		}
		return;
	}

	if (mode == SCROLLBAR_OVERLAY && view_offset == 0 && !scrollbar_dragging && !autoscroll_active && !scrollbar_hover) {
		if (prev_thumb_y != -1) {
			xclear(sx, prev_thumb_y, sx + sw, prev_thumb_y + prev_thumb_h);
			prev_thumb_y = -1;
		}
		return;
	}

	total = sblen + rows;
	thumb_h = (int)((long)win.th * rows / total);
	thumb_h = MAX(thumb_h, 24);
	thumb_h = MIN(thumb_h, win.th);

	available = win.th - thumb_h;
	if (available <= 0) {
		if (prev_thumb_y != -1) {
			xclear(sx, prev_thumb_y, sx + sw, prev_thumb_y + prev_thumb_h);
			prev_thumb_y = -1;
		}
		return;
	}

	thumb_y = win.vborderpx + (int)((long)available * (sblen - view_offset) / sblen);

	if (prev_thumb_y != -1 && (prev_thumb_y != thumb_y || prev_thumb_h != thumb_h)) {
		xclear(sx, prev_thumb_y, sx + sw, prev_thumb_y + prev_thumb_h);
	}

	thumb_col = dc.has_scrollbar_col ? &dc.scrollbar_col : &dc.col[defaultfg];
	XftDrawRect(xw.draw, thumb_col, sx, thumb_y, sw, thumb_h);

	prev_thumb_y = thumb_y;
	prev_thumb_h = thumb_h;
}

void
xfinishdraw(void)
{
	drawscrollbar();
	XCopyArea(xw.dpy, xw.buf, xw.win, dc.gc, 0, 0, win.w,
			win.h, 0, 0);
	XSetForeground(xw.dpy, dc.gc,
			dc.col[IS_SET(MODE_REVERSE)?
				defaultfg : defaultbg].pixel);
}

void
xximspot(int x, int y)
{
	if (xw.ime.xic == NULL)
		return;

	xw.ime.spot.x = borderpx + x * win.cw;
	xw.ime.spot.y = borderpx + (y + 1) * win.ch;

	XSetICValues(xw.ime.xic, XNPreeditAttributes, xw.ime.spotlist, NULL);
}

void
expose(XEvent *ev)
{
	redraw();
}

void
visibility(XEvent *ev)
{
	XVisibilityEvent *e = &ev->xvisibility;

	MODBIT(win.mode, e->state != VisibilityFullyObscured, MODE_VISIBLE);
}

void
unmap(XEvent *ev)
{
	win.mode &= ~MODE_VISIBLE;
}

void
xsetpointermotion(int set)
{
	(void)set;
	if (!xw.dpy || !xw.win)
		return;
	xw.attrs.event_mask |= PointerMotionMask;
	XChangeWindowAttributes(xw.dpy, xw.win, CWEventMask, &xw.attrs);
}

void
xsetpointershape(const char *name)
{
	if (!xw.dpy || !xw.win)
		return;
	if (!name || *name == '\0' || !strcmp(name, "default") || !strcmp(name, "text")) {
		setcursor(CURSOR_DEFAULT);
	} else if (!strcmp(name, "pointer")) {
		setcursor(CURSOR_HAND);
	}
}

static struct timespec last_sync_time;
static int kitty_kbd_flags = 0;
static int kitty_kbd_stack[8];
static int kitty_kbd_stack_len = 0;
static int modifyotherkeys = 0;

void
xsetkittyflags(int flags, int mode)
{
	switch (mode) {
	case 1: /* set */
		kitty_kbd_flags = flags;
		break;
	case 2: /* OR */
		kitty_kbd_flags |= flags;
		break;
	case 3: /* AND NOT */
		kitty_kbd_flags &= ~flags;
		break;
	}
}

int
xgetkittyflags(void)
{
	return kitty_kbd_flags;
}

void
xpushkittyflags(int flags)
{
	if (kitty_kbd_stack_len < (int)LEN(kitty_kbd_stack))
		kitty_kbd_stack[kitty_kbd_stack_len++] = kitty_kbd_flags;
	if (flags >= 0)
		kitty_kbd_flags = flags;
}

void
xpopkittyflags(int count)
{
	while (count-- > 0 && kitty_kbd_stack_len > 0)
		kitty_kbd_flags = kitty_kbd_stack[--kitty_kbd_stack_len];
}

void
xsetmodifyotherkeys(int mode)
{
	modifyotherkeys = mode;
}

void
xsetmode(int set, unsigned int flags)
{
	int mode = win.mode;
	MODBIT(win.mode, set, flags);
	if ((win.mode & MODE_REVERSE) != (mode & MODE_REVERSE))
		redraw();
	if (flags & MODE_SYNC) {
		if (set) {
			clock_gettime(CLOCK_MONOTONIC, &last_sync_time);
		} else if (mode & MODE_SYNC) {
			draw();
			XFlush(xw.dpy);
		}
	}
}

int
xismode(unsigned int flags)
{
	return (win.mode & flags) != 0;
}

unsigned int
xgetmode(void)
{
	return win.mode;
}

int
xsetcursor(int cursor)
{
	if (!BETWEEN(cursor, 0, 7))
		return 1;
	win.cursor = cursor;
	cursorblinks = (win.cursor == 0 || win.cursor == 1 ||
	                win.cursor == 3 || win.cursor == 5);
	return 0;
}

void
xseturgency(int add)
{
	XWMHints *h = XGetWMHints(xw.dpy, xw.win);

	MODBIT(h->flags, add, XUrgencyHint);
	XSetWMHints(xw.dpy, xw.win, h);
	XFree(h);
}

void
xbell(void)
{
	if (!(IS_SET(MODE_FOCUSED)))
		xseturgency(1);
	if (bellvolume)
		XkbBell(xw.dpy, xw.win, bellvolume, (Atom)NULL);
}

void
focus(XEvent *ev)
{
	XFocusChangeEvent *e = &ev->xfocus;

	if (e->mode == NotifyGrab || e->mode == NotifyUngrab || e->mode == NotifyWhileGrabbed)
		return;
	if (e->detail == NotifyPointer || e->detail == NotifyPointerRoot || e->detail == NotifyDetailNone)
		return;

	if (ev->type == FocusIn) {
		if (win.mode & MODE_FOCUSED)
			return;
		if (xw.ime.xic)
			XSetICFocus(xw.ime.xic);
		win.mode |= MODE_FOCUSED;
		xseturgency(0);
		if (IS_SET(MODE_FOCUS))
			ttywrite("\033[I", 3, 0);
		if (!focused) {
			focused = 1;
			if (alpha_unfocused >= 0.0f) {
				xloadalpha();
				redraw();
			}
		}
	} else {
		if (!(win.mode & MODE_FOCUSED))
			return;
		if (xw.ime.xic)
			XUnsetICFocus(xw.ime.xic);
		win.mode &= ~MODE_FOCUSED;
		if (IS_SET(MODE_FOCUS))
			ttywrite("\033[O", 3, 0);
		if (focused) {
			focused = 0;
			if (alpha_unfocused >= 0.0f) {
				xloadalpha();
				redraw();
			}
		}
	}
}

int
match(uint mask, uint state)
{
	return mask == XK_ANY_MOD || mask == (state & ~ignoremod);
}

char*
kmap(KeySym k, uint state)
{
	Key *kp;
	int i;

	for (i = 0; i < LEN(mappedkeys); i++) {
		if (mappedkeys[i] == k)
			break;
	}
	if (i == LEN(mappedkeys)) {
		if ((k & 0xFFFF) < 0xFD00)
			return NULL;
	}

	for (kp = key; kp < key + LEN(key); kp++) {
		if (kp->k != k)
			continue;

		if (!match(kp->mask, state))
			continue;

		if (IS_SET(MODE_APPKEYPAD) ? kp->appkey < 0 : kp->appkey > 0)
			continue;
		if (IS_SET(MODE_NUMLOCK) && kp->appkey == 2)
			continue;

		if (IS_SET(MODE_APPCURSOR) ? kp->appcursor < 0 : kp->appcursor > 0)
			continue;

		return kp->s;
	}

	return NULL;
}

static int
run_config_shortcut(KeySym ksym, uint state)
{
	size_t i;

	for (i = 0; i < g_st_config.shortcut_count; i++) {
		ConfigShortcut *sc = &g_st_config.shortcuts[i];
		if (!match(sc->mod, state))
			continue;

		KeySym target_ksym = sc->keysym;
		if (state & ShiftMask) {
			if (target_ksym >= XK_a && target_ksym <= XK_z)
				target_ksym = target_ksym - XK_a + XK_A;
		} else {
			if (target_ksym >= XK_A && target_ksym <= XK_Z)
				target_ksym = target_ksym - XK_A + XK_a;
		}

		if (ksym == target_ksym) {
			switch (sc->act) {
			case ACT_COPY:
				clipcopy(NULL);
				return 1;
			case ACT_PASTE:
				clippaste(NULL);
				return 1;
			case ACT_SELPASTE:
				selpaste(NULL);
				return 1;
			case ACT_ZOOM_IN: {
				Arg a = {.f = +1};
				zoom(&a);
				return 1;
			}
			case ACT_ZOOM_OUT: {
				Arg a = {.f = -1};
				zoom(&a);
				return 1;
			}
			case ACT_ZOOM_RESET: {
				Arg a = {.f = 0};
				zoomreset(&a);
				return 1;
			}
			case ACT_SCROLL_UP: {
				Arg a = {.i = -1};
				kscrollup(&a);
				return 1;
			}
			case ACT_SCROLL_DOWN: {
				Arg a = {.i = -1};
				kscrolldown(&a);
				return 1;
			}
			case ACT_OPEN_URL: {
				Arg a = {.v = openurlcmd};
				externalpipe(&a);
				return 1;
			}
			case ACT_COPY_URL: {
				Arg a = {.v = copyurlcmd};
				externalpipe(&a);
				return 1;
			}
			case ACT_RELOAD_CONFIG:
				reloadconfig(NULL);
				return 1;
			case ACT_NEW_TERMINAL:
				spawnterminalcwd();
				return 1;
			case ACT_SCROLLBACK_PAGER:
				openscrollbackpager(g_st_config.pager_cmd);
				return 1;
			case ACT_CHANGE_ALPHA_UP: {
				Arg a = {.f = +0.05f};
				changealpha(&a);
				return 1;
			}
			case ACT_CHANGE_ALPHA_DOWN: {
				Arg a = {.f = -0.05f};
				changealpha(&a);
				return 1;
			}
			case ACT_CHANGE_ALPHA_RESET: {
				Arg a = {.f = +2.0f};
				changealpha(&a);
				return 1;
			}
			case ACT_PIPE: {
				char *cmd[] = {"/bin/sh", "-c", sc->cmd, NULL};
				Arg a = {.v = cmd};
				externalpipe(&a);
				return 1;
			}
			case ACT_SPAWN: {
				switch (fork()) {
				case -1:
					break;
				case 0:
					execl("/bin/sh", "sh", "-c", sc->cmd, NULL);
					exit(1);
				}
				return 1;
			}
			case ACT_RESET_TERMINAL: {
				Arg a = {.i = 0};
				resetterm(&a);
				return 1;
			}
			case ACT_FULLSCREEN: {
				Arg a = {.i = 0};
				fullscreen(&a);
				return 1;
			}
			default:
				break;
			}
		}
	}
	return 0;
}

void
kpress(XEvent *ev)
{
	XKeyEvent *e = &ev->xkey;
	KeySym ksym = NoSymbol;
	char buf[64], *customkey;
	int len;
	Rune c;
	Status status;
	Shortcut *bp;

	if (IS_SET(MODE_KBDLOCK))
		return;

	if (xw.ime.xic) {
		len = XmbLookupString(xw.ime.xic, e, buf, sizeof buf, &ksym, &status);
		if (status == XBufferOverflow)
			return;
	} else {
		len = XLookupString(e, buf, sizeof buf, &ksym, NULL);
	}

	if (run_config_shortcut(ksym, e->state))
		return;

	for (bp = shortcuts; bp < shortcuts + LEN(shortcuts); bp++) {
		if (ksym == bp->keysym && match(bp->mod, e->state)) {
			bp->func(&(bp->arg));
			return;
		}
	}

	/* Kitty keyboard protocol progressive enhancement (mode 1: disambiguate escape codes) */
	if (kitty_kbd_flags & 1) {
		int mod = 1;
		if (e->state & ShiftMask)
			mod += 1;
		if (e->state & Mod1Mask)
			mod += 2;
		if (e->state & ControlMask)
			mod += 4;
		if (e->state & Mod4Mask)
			mod += 8;

		if (ksym == XK_BackSpace) {
			if (mod > 1 || (kitty_kbd_flags & 8)) {
				char kbuf[32];
				int klen = snprintf(kbuf, sizeof(kbuf), "\033[127;%du", mod);
				ttywrite(kbuf, klen, 1);
				return;
			}
		} else if (ksym == XK_Return) {
			if (mod > 1 || (kitty_kbd_flags & 8)) {
				char kbuf[32];
				int klen = snprintf(kbuf, sizeof(kbuf), "\033[13;%du", mod);
				ttywrite(kbuf, klen, 1);
				return;
			}
		} else if (ksym == XK_Tab || ksym == XK_ISO_Left_Tab) {
			if (mod > 1 || (kitty_kbd_flags & 8)) {
				char kbuf[32];
				int klen = snprintf(kbuf, sizeof(kbuf), "\033[9;%du", mod);
				ttywrite(kbuf, klen, 1);
				return;
			}
		} else if (ksym == XK_Escape) {
			if (mod > 1 || (kitty_kbd_flags & 8)) {
				char kbuf[32];
				int klen = snprintf(kbuf, sizeof(kbuf), "\033[27;%du", mod);
				ttywrite(kbuf, klen, 1);
				return;
			}
		}
	} else if (modifyotherkeys == 2) {
		int mod = 1;
		if (e->state & ShiftMask)
			mod += 1;
		if (e->state & Mod1Mask)
			mod += 2;
		if (e->state & ControlMask)
			mod += 4;
		if (e->state & Mod4Mask)
			mod += 8;

		if (mod > 1) {
			if (ksym == XK_BackSpace) {
				char kbuf[32];
				int klen = snprintf(kbuf, sizeof(kbuf), "\033[27;%d;127~", mod);
				ttywrite(kbuf, klen, 1);
				return;
			} else if (ksym == XK_Return) {
				char kbuf[32];
				int klen = snprintf(kbuf, sizeof(kbuf), "\033[27;%d;13~", mod);
				ttywrite(kbuf, klen, 1);
				return;
			} else if (ksym == XK_Tab || ksym == XK_ISO_Left_Tab) {
				char kbuf[32];
				int klen = snprintf(kbuf, sizeof(kbuf), "\033[27;%d;9~", mod);
				ttywrite(kbuf, klen, 1);
				return;
			}
		}
	}

	if ((customkey = kmap(ksym, e->state))) {
		ttywrite(customkey, strlen(customkey), 1);
		return;
	}

	if (len == 0)
		return;
	if (len == 1 && e->state & Mod1Mask) {
		if (IS_SET(MODE_8BIT)) {
			if (*buf < 0177) {
				c = *buf | 0x80;
				len = utf8encode(c, buf);
			}
		} else {
			buf[1] = buf[0];
			buf[0] = '\033';
			len = 2;
		}
	}
	ttywrite(buf, len, 1);
}

void
xdndsel(XEvent *e)
{
	char *data = NULL;
	unsigned long result = 0;
	Atom actualType;
	int actualFormat;
	unsigned long bytesAfter;
	XEvent reply;

	memset(&reply, 0, sizeof(reply));
	reply.type = ClientMessage;
	reply.xclient.window = xw.XdndSourceWin;
	reply.xclient.format = 32;
	reply.xclient.data.l[0] = (long) xw.win;
	reply.xclient.data.l[2] = 0;
	reply.xclient.data.l[3] = 0;

	XGetWindowProperty(xw.dpy, e->xselection.requestor,
			e->xselection.property, 0, LONG_MAX, False,
			AnyPropertyType, &actualType, &actualFormat, &result,
			&bytesAfter, (unsigned char **) &data);

	if (result > 0 && data) {
		xdndpastedata(data);
		XFree(data);
	}

	if (xw.XdndSourceVersion >= 2) {
		reply.xclient.message_type = xw.XdndFinished;
		reply.xclient.data.l[1] = (result > 0) ? 1 : 0;
		reply.xclient.data.l[2] = xw.XdndActionCopy;

		XSendEvent(xw.dpy, xw.XdndSourceWin, False, NoEventMask, &reply);
		XFlush(xw.dpy);
	}
}

int
xdndurldecode(char *src, char *dest)
{
	char c;
	int i = 0;

	while (*src) {
		if (*src == '%' && HEX_TO_INT(src[1]) != -1 && HEX_TO_INT(src[2]) != -1) {

			c = (char)((HEX_TO_INT(src[1]) << 4) | HEX_TO_INT(src[2]));
			src += 3;
		} else {
			c = *src++;
		}
		if (xdndescchar && strchr(xdndescchar, c) != NULL) {
			*dest++ = '\\';
			i++;
		}
		*dest++ = c;
		i++;
	}
	*dest++ = ' ';
	*dest = '\0';
	return i + 1;
}

void
xdndpastedata(char *data)
{
	char *pastedata, *t, *saveptr = NULL;
	int i = 0;
	size_t len;

	pastedata = (char *)malloc(strlen(data) * 3 + 2);
	if (!pastedata)
		return;
	*pastedata = '\0';

	t = strtok_r(data, "\r\n", &saveptr);
	while (t != NULL) {

		if (strncmp(t, "file://localhost/", 17) == 0)
			t += 16;
		else if (strncmp(t, "file://", 7) == 0)
			t += 7;
		i += xdndurldecode(t, pastedata + i);
		t = strtok_r(NULL, "\r\n", &saveptr);
	}

	len = strlen(pastedata);
	if (len > 0) {
		if (IS_SET(MODE_BRCKTPASTE))
			ttywrite("\033[200~", 6, 0);
		ttywrite(pastedata, len, 0);
		if (IS_SET(MODE_BRCKTPASTE))
			ttywrite("\033[201~", 6, 0);
	}
	free(pastedata);
}

void
xdndenter(XEvent *e)
{
	unsigned long count = 0;
	Atom *formats = NULL;
	Atom real_formats[6];
	Bool list;
	Atom actualType;
	int actualFormat;
	unsigned long bytesAfter;
	unsigned long i;

	list = (e->xclient.data.l[1] & 1) != 0;

	if (list) {
		XGetWindowProperty(xw.dpy,
			xw.XdndSourceWin,
			xw.XdndTypeList,
			0,
			LONG_MAX,
			False,
			XA_ATOM,
			&actualType,
			&actualFormat,
			&count,
			&bytesAfter,
			(unsigned char **) &formats);
	} else {
		count = 0;
		if (e->xclient.data.l[2] != None)
			real_formats[count++] = (Atom)e->xclient.data.l[2];
		if (e->xclient.data.l[3] != None)
			real_formats[count++] = (Atom)e->xclient.data.l[3];
		if (e->xclient.data.l[4] != None)
			real_formats[count++] = (Atom)e->xclient.data.l[4];

		formats = real_formats;
	}

	xw.XdndSourceFormat = None;
	for (i = 0; formats && i < count; i++) {
		if (formats[i] == xw.XtextUriList || formats[i] == xw.XtextPlain) {
			xw.XdndSourceFormat = formats[i];
			break;
		}
	}

	if (list && formats)
		XFree(formats);
}

void
xdndpos(XEvent *e)
{
	const int32_t xabs = (e->xclient.data.l[2] >> 16) & 0xffff;
	const int32_t yabs = (e->xclient.data.l[2]) & 0xffff;
	Window dummy;
	int xpos, ypos;
	XEvent reply;

	memset(&reply, 0, sizeof(reply));
	reply.type = ClientMessage;
	reply.xclient.window = xw.XdndSourceWin;
	reply.xclient.message_type = xw.XdndStatus;
	reply.xclient.format = 32;
	reply.xclient.data.l[0] = (long) xw.win;
	reply.xclient.data.l[2] = 0;
	reply.xclient.data.l[3] = 0;

	XTranslateCoordinates(xw.dpy,
		XDefaultRootWindow(xw.dpy),
		xw.win,
		xabs, yabs,
		&xpos, &ypos,
		&dummy);

	if (xw.XdndSourceFormat != None) {
		reply.xclient.data.l[1] = 1;
		if (xw.XdndSourceVersion >= 2)
			reply.xclient.data.l[4] = xw.XdndActionCopy;
	}

	XSendEvent(xw.dpy, xw.XdndSourceWin, False, NoEventMask, &reply);
	XFlush(xw.dpy);
}

void
xdnddrop(XEvent *e)
{
	Time time = CurrentTime;
	XEvent reply;

	memset(&reply, 0, sizeof(reply));
	reply.type = ClientMessage;
	reply.xclient.window = xw.XdndSourceWin;
	reply.xclient.format = 32;
	reply.xclient.data.l[0] = (long) xw.win;
	reply.xclient.data.l[2] = 0;
	reply.xclient.data.l[3] = 0;

	if (xw.XdndSourceFormat != None) {
		if (xw.XdndSourceVersion >= 1)
			time = e->xclient.data.l[2];

		XConvertSelection(xw.dpy, xw.XdndSelection,
				xw.XdndSourceFormat, xw.XdndSelection, xw.win, time);
	} else if (xw.XdndSourceVersion >= 2) {
		reply.xclient.message_type = xw.XdndFinished;
		XSendEvent(xw.dpy, xw.XdndSourceWin, False, NoEventMask, &reply);
		XFlush(xw.dpy);
	}
}

void
cmessage(XEvent *e)
{

	if (e->xclient.message_type == xw.xembed && e->xclient.format == 32) {
		if (e->xclient.data.l[1] == XEMBED_FOCUS_IN) {
			win.mode |= MODE_FOCUSED;
			xseturgency(0);
		} else if (e->xclient.data.l[1] == XEMBED_FOCUS_OUT) {
			win.mode &= ~MODE_FOCUSED;
		}
	} else if (e->xclient.data.l[0] == xw.wmdeletewin) {
		ttyhangup();
		exit(0);
	} else if (e->xclient.message_type == xw.XdndEnter) {
		xw.XdndSourceWin = (Window)e->xclient.data.l[0];
		xw.XdndSourceVersion = e->xclient.data.l[1] >> 24;
		xw.XdndSourceFormat = None;
		if (xw.XdndSourceVersion > 5)
			return;
		xdndenter(e);
	} else if (e->xclient.message_type == xw.XdndPosition
			&& xw.XdndSourceVersion <= 5) {
		xdndpos(e);
	} else if (e->xclient.message_type == xw.XdndDrop
			&& xw.XdndSourceVersion <= 5) {
		xdnddrop(e);
	} else if (e->xclient.message_type == xw.XdndLeave) {
		xw.XdndSourceFormat = None;
	}
}

static int pending_resize = 0;
static int pending_w = 0, pending_h = 0;
static struct timespec last_resize_time = {0};

void
resize(XEvent *e)
{
	XEvent next;
	while (XCheckTypedWindowEvent(xw.dpy, xw.win, ConfigureNotify, &next))
		*e = next;

	if (e->xconfigure.width == win.w && e->xconfigure.height == win.h) {
		pending_resize = 0;
		return;
	}

	int new_col = (e->xconfigure.width - 2 * borderpx) / win.cw;
	int new_row = (e->xconfigure.height - 2 * borderpx) / win.ch;
	new_col = MAX(1, new_col);
	new_row = MAX(1, new_row);

	int cur_col = win.cw ? (win.tw / win.cw) : 0;
	int cur_row = win.ch ? (win.th / win.ch) : 0;

	if (new_col == cur_col && new_row == cur_row) {
		win.w = e->xconfigure.width;
		win.h = e->xconfigure.height;
		win.hborderpx = MAX(0, (win.w - cur_col * win.cw) * anysize_halign / 100);
		win.vborderpx = MAX(0, (win.h - cur_row * win.ch) * anysize_valign / 100);
		xresize(cur_col, cur_row);
		redraw();
		pending_resize = 0;
		return;
	}

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	if (TIMEDIFF(now, last_resize_time) >= 20) {
		last_resize_time = now;
		pending_resize = 0;
		cresize(e->xconfigure.width, e->xconfigure.height);
	} else {
		pending_resize = 1;
		pending_w = e->xconfigure.width;
		pending_h = e->xconfigure.height;
	}
}

static volatile sig_atomic_t sigusr1_received = 0;

static void
sigusr1_handler(int sig)
{
	(void)sig;
	sigusr1_received = 1;
}

void
run(void)
{

	XEvent ev;
	int w = win.w, h = win.h;
	fd_set rfd;
	int xfd = XConnectionNumber(xw.dpy), ttyfd, xev, drawing;
	struct timespec seltv, *tv, now, lastblink, trigger;
	double timeout;

	do {
		XNextEvent(xw.dpy, &ev);

		if (XFilterEvent(&ev, None))
			continue;
		if (ev.type == ConfigureNotify) {
			w = ev.xconfigure.width;
			h = ev.xconfigure.height;
		}
	} while (ev.type != MapNotify);

	cresize(w, h);
	ttyfd = ttynew(opt_line, shell, opt_io, opt_cmd);

	struct timespec last_cfg_check = {0};
	clock_gettime(CLOCK_MONOTONIC, &last_cfg_check);

	for (timeout = -1, drawing = 0, lastblink = (struct timespec){0};;) {
		FD_ZERO(&rfd);
		FD_SET(ttyfd, &rfd);
		FD_SET(xfd, &rfd);

		if (XPending(xw.dpy))
			timeout = 0;

		if (graphics_next_redraw_delay != INT_MAX &&
		    IS_SET(MODE_VISIBLE))
			timeout = timeout < 0 ? graphics_next_redraw_delay
					      : MIN(timeout,
						    graphics_next_redraw_delay);

		if (pending_resize) {
			clock_gettime(CLOCK_MONOTONIC, &now);
			int diff = TIMEDIFF(now, last_resize_time);
			if (diff >= 20) {
				timeout = 0;
			} else {
				int rem = 20 - diff;
				timeout = timeout < 0 ? rem : MIN(timeout, rem);
			}
		}

		if (autoscroll_active && (buttons & 1)) {
			clock_gettime(CLOCK_MONOTONIC, &now);
			int diff = TIMEDIFF(now, last_autoscroll_tick);
			if (diff >= 35) {
				timeout = 0;
			} else {
				int rem = 35 - diff;
				timeout = timeout < 0 ? rem : MIN(timeout, rem);
			}
		}

		if (timeout < 0 || timeout > 500)
			timeout = 500;

		seltv.tv_sec = timeout / 1E3;
		seltv.tv_nsec = 1E6 * (timeout - 1E3 * seltv.tv_sec);
		tv = timeout >= 0 ? &seltv : NULL;

		if (pselect(MAX(xfd, ttyfd)+1, &rfd, NULL, NULL, tv, NULL) < 0) {
			if (errno == EINTR) {
				if (sigusr1_received) {
					sigusr1_received = 0;
					reloadconfig(NULL);
				}
				continue;
			}
			die("select failed: %s\n", strerror(errno));
		}

		if (sigusr1_received) {
			sigusr1_received = 0;
			reloadconfig(NULL);
		}

		clock_gettime(CLOCK_MONOTONIC, &now);

		if (TIMEDIFF(now, last_cfg_check) > 500) {
			last_cfg_check = now;
			if (st_config_check_modified())
				reloadconfig(NULL);
		}

		if (FD_ISSET(ttyfd, &rfd))
			ttyread();

		xev = 0;
		while (XPending(xw.dpy)) {
			xev = 1;
			XNextEvent(xw.dpy, &ev);
			if (XFilterEvent(&ev, None))
				continue;
			if (handler[ev.type])
				(handler[ev.type])(&ev);
		}

		if (autoscroll_active && (buttons & 1)) {
			clock_gettime(CLOCK_MONOTONIC, &now);
			if (TIMEDIFF(now, last_autoscroll_tick) >= 35) {
				last_autoscroll_tick = now;
				if (autoscroll_delta < 0) {
					kscrollup(&((Arg){.i = -autoscroll_delta}));
					mousesel(&autoscroll_last_ev, 0);
				} else if (autoscroll_delta > 0 && get_sb_view_offset() > 0) {
					kscrolldown(&((Arg){.i = autoscroll_delta}));
					mousesel(&autoscroll_last_ev, 0);
				} else if (autoscroll_delta > 0 && get_sb_view_offset() == 0) {
					autoscroll_active = 0;
				}
			}
		}

		if (FD_ISSET(ttyfd, &rfd) || xev) {
			if (!drawing) {
				trigger = now;
				if (IS_SET(MODE_BLINK)) {
					win.mode ^= MODE_BLINK;
				}
				lastblink = now;
				drawing = 1;
			}
			if (maxlatency > 0 && minlatency > 0) {
				timeout = (maxlatency - TIMEDIFF(now, trigger)) \
				          / maxlatency * minlatency;
				if (timeout > 0)
					continue;
			}
		}

		timeout = -1;
		if (blinktimeout && (cursorblinks || tattrset(ATTR_BLINK))) {
			timeout = blinktimeout - TIMEDIFF(now, lastblink);
			if (timeout <= 0) {
				if (-timeout > blinktimeout)
					win.mode |= MODE_BLINK;
				win.mode ^= MODE_BLINK;
				tsetdirtattr(ATTR_BLINK);
				lastblink = now;
				timeout = blinktimeout;
			}
		}

		if (IS_SET(MODE_SYNC)) {
			if (TIMEDIFF(now, last_sync_time) < 150) {
				timeout = (minlatency > 0) ? minlatency : 2;
				continue;
			}
			win.mode &= ~MODE_SYNC;
		}

		clock_gettime(CLOCK_MONOTONIC, &now);
		if (pending_resize) {
			if (TIMEDIFF(now, last_resize_time) >= 20) {
				pending_resize = 0;
				last_resize_time = now;
				cresize(pending_w, pending_h);
			}
		}

		draw();
		XFlush(xw.dpy);
		drawing = 0;
	}
}

void
setnetwmicon(const char *icon_path)
{
	char resolved[1024];
	Imlib_Image img;
	int w, h, n;
	unsigned int *data;
	unsigned long *icon;
	Atom netwmicon;

	if (icon_path && *icon_path) {
		if (icon_path[0] == '~' && (icon_path[1] == '/' || icon_path[1] == '\0')) {
			const char *home = getenv("HOME");
			if (home)
				snprintf(resolved, sizeof(resolved), "%s%s", home, icon_path + 1);
			else
				strncpy(resolved, icon_path, sizeof(resolved) - 1);
			icon_path = resolved;
		}
	} else {
		const char *home = getenv("HOME");
		const char *data_home = getenv("XDG_DATA_HOME");
		if (data_home && *data_home) {
			snprintf(resolved, sizeof(resolved), "%s/icons/st.png", data_home);
			if (access(resolved, R_OK) == 0)
				icon_path = resolved;
		}
		if ((!icon_path || !*icon_path) && home && *home) {
			snprintf(resolved, sizeof(resolved), "%s/.local/share/icons/st.png", home);
			if (access(resolved, R_OK) == 0)
				icon_path = resolved;
			else {
				snprintf(resolved, sizeof(resolved), "%s/.config/st/st.png", home);
				if (access(resolved, R_OK) == 0)
					icon_path = resolved;
			}
		}
		if (!icon_path || !*icon_path) {
			if (access("/usr/local/share/pixmaps/st.png", R_OK) == 0)
				icon_path = "/usr/local/share/pixmaps/st.png";
			else if (access("/usr/share/pixmaps/st.png", R_OK) == 0)
				icon_path = "/usr/share/pixmaps/st.png";
			else if (access("st.png", R_OK) == 0)
				icon_path = "st.png";
			else
				return;
		}
	}

	img = imlib_load_image(icon_path);
	if (!img)
		return;

	imlib_context_set_image(img);
	w = imlib_image_get_width();
	h = imlib_image_get_height();
	data = (unsigned int *)imlib_image_get_data_for_reading_only();
	if (data) {
		n = 2 + w * h;
		icon = xmalloc(n * sizeof(unsigned long));
		icon[0] = w;
		icon[1] = h;
		for (int i = 0; i < w * h; i++)
			icon[2 + i] = data[i];

		netwmicon = XInternAtom(xw.dpy, "_NET_WM_ICON", False);
		XChangeProperty(xw.dpy, xw.win, netwmicon, XA_CARDINAL, 32,
		                PropModeReplace, (unsigned char *)icon, n);
		free(icon);
	}
	imlib_free_image();
}

static void
apply_config(void)
{
	int i;

	if (g_st_config.font[0])
		font = g_st_config.font;

	if (g_st_config.sparefont[0])
		sparefonts[0] = g_st_config.sparefont;

	cwscale = g_st_config.cwscale;
	chscale = g_st_config.chscale;
	alpha = g_st_config.alpha;
	alpha_unfocused = g_st_config.alpha_unfocused;
	borderpx = g_st_config.borderpx;
	anysize_halign = g_st_config.anysize_halign;
	anysize_valign = g_st_config.anysize_valign;

	if (g_st_config.cols > 0)
		cols = g_st_config.cols;
	if (g_st_config.rows > 0)
		rows = g_st_config.rows;

	cursorstyle = g_st_config.cursor_style;
	blinktimeout = g_st_config.cursor_blink_timeout;
	cursor_dynamic_color = g_st_config.cursor_dynamic_color;
	cursorthickness = g_st_config.cursor_thickness;

	bold_is_not_bright = g_st_config.bold_is_not_bright;
	tabspaces = g_st_config.tabspaces;
	bellvolume = g_st_config.bellvolume;
	minlatency = g_st_config.minlatency;
	maxlatency = g_st_config.maxlatency;

	for (i = 0; i < ST_CONFIG_COLOR_COUNT && i < LEN(colorname); i++) {
		if (g_st_config.has_color[i])
			colorname[i] = g_st_config.colorname[i];
	}

	if (g_st_config.xdndescchar[0])
		xdndescchar = g_st_config.xdndescchar;

	if (g_st_config.histsize > 0)
		histsize = g_st_config.histsize;

	if (g_st_config.worddelimiters[0]) {
		static wchar_t w_delims[128];
		mbstowcs(w_delims, g_st_config.worddelimiters, LEN(w_delims) - 1);
		w_delims[LEN(w_delims) - 1] = L'\0';
		worddelimiters = w_delims;
	}

	if (g_st_config.doubleclicktimeout > 0)
		doubleclicktimeout = g_st_config.doubleclicktimeout;
	if (g_st_config.tripleclicktimeout > 0)
		tripleclicktimeout = g_st_config.tripleclicktimeout;

	allowaltscreen = g_st_config.allowaltscreen;
	allowwindowops = g_st_config.allowwindowops;

	if (g_st_config.graphics_max_file_size > 0)
		graphics_max_single_image_file_size = g_st_config.graphics_max_file_size;
	if (g_st_config.graphics_max_ram_size > 0)
		graphics_max_total_ram_size = g_st_config.graphics_max_ram_size;
}

void
reloadconfig(const Arg *arg)
{
	(void)arg;
	if (st_config_reload()) {
		apply_config();
		usedfont = (opt_font == NULL)? font : opt_font;
		xunloadfonts();
		xloadfonts(usedfont, 0);
		xloadsparefonts();
		xloadcols();
		XSetWindowBackground(xw.dpy, xw.win, dc.col[defaultbg].pixel);
		XClearWindow(xw.dpy, xw.win);
		pending_resize = 0;
		cresize(0, 0);
		redraw();
		xhints();
		setnetwmicon(g_st_config.icon);
	}
}

void
usage(void)

{
	die("usage: %s [-aiv] [-c class] [-C config] [-f font] [-g geometry]"
	    " [-n name] [-o file]\n"
	    "          [-T title] [-t title] [-w windowid]"
	    " [[-e] command [args ...]]\n"
	    "       %s [-aiv] [-c class] [-C config] [-f font] [-g geometry]"
	    " [-n name] [-o file]\n"
	    "          [-T title] [-t title] [-w windowid] -l line"
	    " [stty_args ...]\n", argv0, argv0);
}

int
main(int argc, char *argv[])
{
	const char *config_path;

	xw.l = xw.t = 0;
	xw.isfixed = False;

	ARGBEGIN {
	case 'a':
		allowaltscreen = 0;
		break;
	case 'A':
		alpha = strtof(EARGF(usage()), NULL);
		LIMIT(alpha, 0.0, 1.0);
		break;
	case 'c':
		opt_class = EARGF(usage());
		break;
	case 'C':
		opt_config = EARGF(usage());
		break;
	case 'e':
		if (argc > 0)
			--argc, ++argv;
		goto run;
	case 'f':
		opt_font = EARGF(usage());
		break;
	case 'g':
		xw.gm = XParseGeometry(EARGF(usage()),
				&xw.l, &xw.t, &cols, &rows);
		break;
	case 'i':
		xw.isfixed = 1;
		break;
	case 'o':
		opt_io = EARGF(usage());
		break;
	case 'l':
		opt_line = EARGF(usage());
		break;
	case 'n':
		opt_name = EARGF(usage());
		break;
	case 't':
	case 'T':
		opt_title = EARGF(usage());
		break;
	case 'w':
		opt_embed = EARGF(usage());
		break;
	case 'v':
		die("%s " VERSION "\n", argv0);
		break;
	default:
		usage();
	} ARGEND;

run:
	if (argc > 0)
		opt_cmd = argv;

	if (!opt_title)
		opt_title = (opt_line || !opt_cmd) ? "st" : opt_cmd[0];

	st_config_init_defaults(font, alpha, borderpx, anysize_halign, anysize_valign,
	                        cols, rows, cursorstyle, blinktimeout, cursorthickness,
	                        termname, shell, tabspaces, bellvolume, minlatency,
	                        maxlatency, cwscale, chscale, boxdraw, boxdraw_bold,
	                        boxdraw_braille, (const char **)colorname, LEN(colorname));

	config_path = st_config_find_file(opt_config);
	if (config_path) {
		if (st_config_load(config_path))
			apply_config();
	}

	xsetcursor(cursorstyle);
	{
		struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = sigusr1_handler;
		sa.sa_flags = SA_RESTART;
		sigemptyset(&sa.sa_mask);
		sigaction(SIGUSR1, &sa, NULL);
	}

	setlocale(LC_CTYPE, "");

	XSetLocaleModifiers("");
	cols = MAX(cols, 1);
	rows = MAX(rows, 1);
	tnew(cols, rows);
	xinit(cols, rows);
	xsetenv();
	selinit();
	run();

	return 0;
}
