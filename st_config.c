#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>

#include "st_config.h"

StConfig g_st_config;

extern unsigned graphics_max_single_image_file_size;
extern unsigned graphics_total_file_cache_size;
extern unsigned graphics_max_single_image_ram_size;
extern unsigned graphics_max_total_ram_size;
extern unsigned graphics_max_total_placements;
extern double graphics_excess_tolerance_ratio;
extern unsigned graphics_animation_min_delay;

static void st_config_sync_graphics(void) {
	graphics_max_single_image_file_size = g_st_config.graphics_max_single_file_size;
	graphics_total_file_cache_size = g_st_config.graphics_total_file_size;
	graphics_max_single_image_ram_size = g_st_config.graphics_max_single_ram_size;
	graphics_max_total_ram_size = g_st_config.graphics_max_total_ram_size;
	graphics_max_total_placements = g_st_config.graphics_max_placements;
	graphics_excess_tolerance_ratio = g_st_config.graphics_tolerance;
	graphics_animation_min_delay = g_st_config.graphics_anim_delay;
}

static char *
trim(char *str)
{
	char *end;

	while (isspace((unsigned char)*str))
		str++;

	if (*str == 0)
		return str;

	end = str + strlen(str) - 1;
	while (end > str && isspace((unsigned char)*end))
		end--;

	end[1] = '\0';
	return str;
}

static char *
unquote(char *str)
{
	size_t len;

	str = trim(str);
	len = strlen(str);

	if (len >= 2 && ((str[0] == '"' && str[len - 1] == '"') ||
	                 (str[0] == '\'' && str[len - 1] == '\''))) {
		str[len - 1] = '\0';
		str++;
	}

	return trim(str);
}

static int
parse_bool(const char *val)
{
	if (!strcasecmp(val, "true") || !strcasecmp(val, "1") ||
	    !strcasecmp(val, "yes") || !strcasecmp(val, "on"))
		return 1;
	return 0;
}

int
st_config_parse_key_combo(const char *str, unsigned int *out_mod, KeySym *out_keysym)
{
	char buf[128], *tok;
	char keyname[64] = "";
	unsigned int mod = 0;
	KeySym ksym = NoSymbol;

	if (!str || !*str)
		return 0;

	strncpy(buf, str, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';

	size_t slen = strlen(buf);
	if (slen >= 2 && buf[slen - 1] == '+' && buf[slen - 2] == '+') {
		strcpy(keyname, "plus");
		buf[slen - 2] = '\0';
	} else if (slen >= 2 && buf[slen - 1] == '-' && buf[slen - 2] == '+') {
		strcpy(keyname, "minus");
		buf[slen - 2] = '\0';
	}

	tok = strtok(buf, "+- \t");
	while (tok) {
		if (!strcasecmp(tok, "ctrl") || !strcasecmp(tok, "control")) {
			mod |= ControlMask;
		} else if (!strcasecmp(tok, "shift")) {
			mod |= ShiftMask;
		} else if (!strcasecmp(tok, "alt") || !strcasecmp(tok, "mod1")) {
			mod |= Mod1Mask;
		} else if (!strcasecmp(tok, "super") || !strcasecmp(tok, "win") ||
		           !strcasecmp(tok, "mod4")) {
			mod |= Mod4Mask;
		} else {
			strncpy(keyname, tok, sizeof(keyname) - 1);
		}
		tok = strtok(NULL, "+- \t");
	}

	if (!keyname[0])
		return 0;

	if (!strcasecmp(keyname, "return") || !strcasecmp(keyname, "enter"))
		strcpy(keyname, "Return");
	else if (!strcasecmp(keyname, "esc") || !strcasecmp(keyname, "escape"))
		strcpy(keyname, "Escape");
	else if (!strcasecmp(keyname, "backspace"))
		strcpy(keyname, "BackSpace");
	else if (!strcasecmp(keyname, "tab"))
		strcpy(keyname, "Tab");
	else if (!strcasecmp(keyname, "space"))
		strcpy(keyname, "space");
	else if (!strcasecmp(keyname, "plus") || !strcmp(keyname, "+"))
		strcpy(keyname, "plus");
	else if (!strcasecmp(keyname, "minus") || !strcmp(keyname, "-"))
		strcpy(keyname, "minus");
	else if (!strcasecmp(keyname, "equal") || !strcmp(keyname, "="))
		strcpy(keyname, "equal");
	else if (!strcasecmp(keyname, "page_up") || !strcasecmp(keyname, "pageup") || !strcasecmp(keyname, "prior"))
		strcpy(keyname, "Prior");
	else if (!strcasecmp(keyname, "page_down") || !strcasecmp(keyname, "pagedown") || !strcasecmp(keyname, "next"))
		strcpy(keyname, "Next");
	else if (!strcasecmp(keyname, "home"))
		strcpy(keyname, "Home");
	else if (!strcasecmp(keyname, "end"))
		strcpy(keyname, "End");
	else if (!strcasecmp(keyname, "insert") || !strcasecmp(keyname, "ins"))
		strcpy(keyname, "Insert");
	else if (!strcasecmp(keyname, "delete") || !strcasecmp(keyname, "del"))
		strcpy(keyname, "Delete");
	else if (strlen(keyname) == 1 && keyname[0] >= 'a' && keyname[0] <= 'z') {
		if (mod & ShiftMask)
			keyname[0] = keyname[0] - 'a' + 'A';
	}

	ksym = XStringToKeysym(keyname);
	if (ksym == NoSymbol) {
		if (strlen(keyname) == 1)
			ksym = (KeySym)(unsigned char)keyname[0];
		else
			return 0;
	}

	*out_mod = mod;
	*out_keysym = ksym;
	return 1;
}

ShortcutAction
st_config_parse_action(const char *str, char *out_cmd, size_t cmd_size)
{
	if (!str || !*str)
		return ACT_NONE;

	if (out_cmd && cmd_size > 0)
		out_cmd[0] = '\0';

	if (!strcasecmp(str, "copy") || !strcasecmp(str, "clipcopy") || !strcasecmp(str, "copy_to_clipboard"))
		return ACT_COPY;
	if (!strcasecmp(str, "paste") || !strcasecmp(str, "clippaste") || !strcasecmp(str, "paste_from_clipboard"))
		return ACT_PASTE;
	if (!strcasecmp(str, "selpaste"))
		return ACT_SELPASTE;
	if (!strcasecmp(str, "zoom_in") || !strcasecmp(str, "zoomin") || !strcasecmp(str, "zoom+"))
		return ACT_ZOOM_IN;
	if (!strcasecmp(str, "zoom_out") || !strcasecmp(str, "zoomout") || !strcasecmp(str, "zoom-"))
		return ACT_ZOOM_OUT;
	if (!strcasecmp(str, "zoom_reset") || !strcasecmp(str, "zoomreset") || !strcasecmp(str, "zoom0"))
		return ACT_ZOOM_RESET;
	if (!strcasecmp(str, "scroll_up") || !strcasecmp(str, "scrollup") || !strcasecmp(str, "kscrollup"))
		return ACT_SCROLL_UP;
	if (!strcasecmp(str, "scroll_down") || !strcasecmp(str, "scrolldown") || !strcasecmp(str, "kscrolldown"))
		return ACT_SCROLL_DOWN;
	if (!strcasecmp(str, "open_url") || !strcasecmp(str, "openurl"))
		return ACT_OPEN_URL;
	if (!strcasecmp(str, "copy_url") || !strcasecmp(str, "copyurl"))
		return ACT_COPY_URL;
	if (!strcasecmp(str, "reload_config") || !strcasecmp(str, "reload"))
		return ACT_RELOAD_CONFIG;
	if (!strcasecmp(str, "new_terminal") || !strcasecmp(str, "newterminal") ||
	    !strcasecmp(str, "new_window") || !strcasecmp(str, "newwindow") ||
	    !strcasecmp(str, "new_os_window"))
		return ACT_NEW_TERMINAL;
	if (!strcasecmp(str, "scrollback_pager") || !strcasecmp(str, "pager") ||
	    !strcasecmp(str, "show_scrollback"))
		return ACT_SCROLLBACK_PAGER;
	if (!strcasecmp(str, "change_alpha_up") || !strcasecmp(str, "alphaup"))
		return ACT_CHANGE_ALPHA_UP;
	if (!strcasecmp(str, "change_alpha_down") || !strcasecmp(str, "alphadown"))
		return ACT_CHANGE_ALPHA_DOWN;
	if (!strcasecmp(str, "change_alpha_reset") || !strcasecmp(str, "alphareset"))
		return ACT_CHANGE_ALPHA_RESET;

	if (!strcasecmp(str, "reset_terminal") || !strcasecmp(str, "reset") || !strcasecmp(str, "resetterm"))
		return ACT_RESET_TERMINAL;
	if (!strcasecmp(str, "fullscreen") || !strcasecmp(str, "toggle_fullscreen") || !strcasecmp(str, "togglefullscreen"))
		return ACT_FULLSCREEN;

	if (!strncasecmp(str, "pipe:", 5)) {
		if (out_cmd && cmd_size > 0)
			strncpy(out_cmd, str + 5, cmd_size - 1);
		return ACT_PIPE;
	}
	if (!strncasecmp(str, "spawn:", 6)) {
		if (out_cmd && cmd_size > 0)
			strncpy(out_cmd, str + 6, cmd_size - 1);
		return ACT_SPAWN;
	}

	return ACT_NONE;
}

void
st_config_add_shortcut(unsigned int mod, KeySym keysym, ShortcutAction act, const char *cmd)
{
	size_t i;

	for (i = 0; i < g_st_config.shortcut_count; i++) {
		if (g_st_config.shortcuts[i].mod == mod &&
		    g_st_config.shortcuts[i].keysym == keysym) {
			g_st_config.shortcuts[i].act = act;
			if (cmd)
				strncpy(g_st_config.shortcuts[i].cmd, cmd, sizeof(g_st_config.shortcuts[i].cmd) - 1);
			else
				g_st_config.shortcuts[i].cmd[0] = '\0';
			return;
		}
	}

	if (g_st_config.shortcut_count < ST_CONFIG_MAX_SHORTCUTS) {
		ConfigShortcut *sc = &g_st_config.shortcuts[g_st_config.shortcut_count++];
		sc->mod = mod;
		sc->keysym = keysym;
		sc->act = act;
		if (cmd)
			strncpy(sc->cmd, cmd, sizeof(sc->cmd) - 1);
		else
			sc->cmd[0] = '\0';
	}
}

static void
init_default_shortcuts(void)
{
	g_st_config.shortcut_count = 0;
	strncpy(g_st_config.pager_cmd, "nvim", sizeof(g_st_config.pager_cmd) - 1);

	st_config_add_shortcut(ControlMask | ShiftMask, XK_C, ACT_COPY, NULL);
	st_config_add_shortcut(ControlMask | ShiftMask, XK_V, ACT_PASTE, NULL);
	st_config_add_shortcut(ControlMask | ShiftMask, XK_Y, ACT_SELPASTE, NULL);
	st_config_add_shortcut(ShiftMask, XK_Insert, ACT_SELPASTE, NULL);

	st_config_add_shortcut(ControlMask | ShiftMask, XK_Return, ACT_NEW_TERMINAL, NULL);
	st_config_add_shortcut(ControlMask | ShiftMask, XK_H, ACT_SCROLLBACK_PAGER, NULL);
	st_config_add_shortcut(Mod1Mask, XK_u, ACT_OPEN_URL, NULL);
	st_config_add_shortcut(Mod1Mask, XK_y, ACT_COPY_URL, NULL);
	st_config_add_shortcut(ControlMask | ShiftMask, XK_F5, ACT_RELOAD_CONFIG, NULL);

	st_config_add_shortcut(ControlMask, XK_plus, ACT_ZOOM_IN, NULL);
	st_config_add_shortcut(ControlMask, XK_equal, ACT_ZOOM_IN, NULL);
	st_config_add_shortcut(ControlMask, XK_minus, ACT_ZOOM_OUT, NULL);
	st_config_add_shortcut(ControlMask, XK_0, ACT_ZOOM_RESET, NULL);

	st_config_add_shortcut(ShiftMask, XK_Prior, ACT_SCROLL_UP, NULL);
	st_config_add_shortcut(ShiftMask, XK_Next, ACT_SCROLL_DOWN, NULL);

	st_config_add_shortcut(Mod1Mask, XK_a, ACT_CHANGE_ALPHA_UP, NULL);
	st_config_add_shortcut(Mod1Mask, XK_s, ACT_CHANGE_ALPHA_DOWN, NULL);
	st_config_add_shortcut(Mod1Mask, XK_m, ACT_CHANGE_ALPHA_RESET, NULL);
	st_config_add_shortcut(ControlMask | ShiftMask, XK_Escape, ACT_RESET_TERMINAL, NULL);
	st_config_add_shortcut(ControlMask | ShiftMask, XK_R, ACT_RESET_TERMINAL, NULL);
}

void
st_config_init_defaults(const char *def_font, float def_alpha, int def_borderpx,
                         int def_halign, int def_valign, unsigned int def_cols,
                         unsigned int def_rows, unsigned int def_cursorstyle,
                         unsigned int def_blinktimeout, unsigned int def_cursorthickness,
                         const char *def_termname, const char *def_shell,
                         unsigned int def_tabspaces, int def_bellvolume,
                         double def_minlatency, double def_maxlatency,
                         float def_cwscale, float def_chscale,
                         int def_boxdraw, int def_boxdraw_bold, int def_boxdraw_braille,
                         const char *def_colorname[], size_t def_color_count)
{
	memset(&g_st_config, 0, sizeof(g_st_config));

	if (def_font)
		strncpy(g_st_config.font, def_font, sizeof(g_st_config.font) - 1);
	g_st_config.sparefont[0] = '\0';

	g_st_config.alpha = def_alpha;
	g_st_config.alpha_unfocused = -1.0f;
	g_st_config.borderpx = def_borderpx;
	g_st_config.anysize_halign = def_halign;
	g_st_config.anysize_valign = def_valign;
	g_st_config.cols = def_cols;
	g_st_config.rows = def_rows;

	g_st_config.cursor_style = def_cursorstyle;
	g_st_config.cursor_blink_timeout = def_blinktimeout;
	g_st_config.cursor_dynamic_color = 1;
	g_st_config.cursor_thickness = def_cursorthickness;

	g_st_config.bold_is_not_bright = 1;
	g_st_config.boxdraw = def_boxdraw;
	g_st_config.boxdraw_bold = def_boxdraw_bold;
	g_st_config.boxdraw_braille = def_boxdraw_braille;

	if (def_termname)
		strncpy(g_st_config.termname, def_termname, sizeof(g_st_config.termname) - 1);
	if (def_shell)
		strncpy(g_st_config.shell, def_shell, sizeof(g_st_config.shell) - 1);

	g_st_config.tabspaces = def_tabspaces;
	g_st_config.bellvolume = def_bellvolume;
	g_st_config.minlatency = def_minlatency;
	g_st_config.maxlatency = def_maxlatency;
	g_st_config.cwscale = def_cwscale;
	g_st_config.chscale = def_chscale;

	g_st_config.icon[0] = '\0';
	strncpy(g_st_config.xdndescchar, " !\"#$&'()*;<>?[\\]^`{|}~", sizeof(g_st_config.xdndescchar) - 1);

	g_st_config.histsize = 2000;
	g_st_config.mouse_scroll_multiplier = 3;
	g_st_config.scrollbar = SCROLLBAR_OVERLAY;
	g_st_config.scrollbar_width = 4;
	g_st_config.scrollbar_color[0] = '\0';
	strncpy(g_st_config.worddelimiters, " ", sizeof(g_st_config.worddelimiters) - 1);
	g_st_config.doubleclicktimeout = 300;
	g_st_config.tripleclicktimeout = 600;
	g_st_config.allowaltscreen = 1;
	g_st_config.allowwindowops = 0;
	strncpy(g_st_config.image_preview_cmd, "feh", sizeof(g_st_config.image_preview_cmd) - 1);
	strncpy(g_st_config.url_launcher, "xdg-open", sizeof(g_st_config.url_launcher) - 1);
	strncpy(g_st_config.url_prefixes, "magnet", sizeof(g_st_config.url_prefixes) - 1);
	g_st_config.url_style = 3;

	strncpy(g_st_config.url_color, "#7aa2f7", sizeof(g_st_config.url_color) - 1);
	g_st_config.underline_hyperlinks = URL_UNDERLINE_HOVER;
	g_st_config.undercurl_style = UNDERCURL_SPARSE;
	g_st_config.url_click_modifiers = URL_CLICK_NONE;
	g_st_config.selection_bg[0] = '\0';
	g_st_config.selection_fg[0] = '\0';
	g_st_config.graphics_max_file_size = 20 * 1024 * 1024;
	g_st_config.graphics_max_ram_size = 300 * 1024 * 1024;
	g_st_config.graphics_max_single_file_size = 20 * 1024 * 1024;
	g_st_config.graphics_total_file_size = 300 * 1024 * 1024;
	g_st_config.graphics_max_single_ram_size = 100 * 1024 * 1024;
	g_st_config.graphics_max_total_ram_size = 300 * 1024 * 1024;
	g_st_config.graphics_max_placements = 4096;
	g_st_config.graphics_tolerance = 0.05;
	g_st_config.graphics_anim_delay = 20;
	g_st_config.kitty_graphics = 1;
	g_st_config.sixel_graphics = 0;

	for (size_t i = 0; i < def_color_count && i < ST_CONFIG_COLOR_COUNT; i++) {
		if (def_colorname[i]) {
			strncpy(g_st_config.colorname[i], def_colorname[i], sizeof(g_st_config.colorname[i]) - 1);
			g_st_config.has_color[i] = 1;
		}
	}

	init_default_shortcuts();
	st_config_sync_graphics();
}

static int
file_exists(const char *path)
{
	struct stat st;
	return (stat(path, &st) == 0 && S_ISREG(st.st_mode));
}

const char *
st_config_find_file(const char *explicit_path)
{
	static char resolved_path[ST_CONFIG_MAX_PATH];
	const char *env;

	if (explicit_path && file_exists(explicit_path)) {
		strncpy(resolved_path, explicit_path, sizeof(resolved_path) - 1);
		return resolved_path;
	}

	env = getenv("ST_CONFIG");
	if (env && file_exists(env)) {
		strncpy(resolved_path, env, sizeof(resolved_path) - 1);
		return resolved_path;
	}

	env = getenv("XDG_CONFIG_HOME");
	if (env) {
		snprintf(resolved_path, sizeof(resolved_path), "%s/st/st.conf", env);
		if (file_exists(resolved_path))
			return resolved_path;

		snprintf(resolved_path, sizeof(resolved_path), "%s/st/config.toml", env);
		if (file_exists(resolved_path))
			return resolved_path;
	}

	env = getenv("HOME");
	if (env) {
		snprintf(resolved_path, sizeof(resolved_path), "%s/.config/st/st.conf", env);
		if (file_exists(resolved_path))
			return resolved_path;

		snprintf(resolved_path, sizeof(resolved_path), "%s/.config/st/config.toml", env);
		if (file_exists(resolved_path))
			return resolved_path;

		snprintf(resolved_path, sizeof(resolved_path), "%s/.st.conf", env);
		if (file_exists(resolved_path))
			return resolved_path;
	}

	env = getenv("XDG_CONFIG_DIRS");
	if (env && *env) {
		char dirs[ST_CONFIG_MAX_PATH];
		strncpy(dirs, env, sizeof(dirs) - 1);
		dirs[sizeof(dirs) - 1] = '\0';
		char *p = dirs;
		while (p && *p) {
			char *colon = strchr(p, ':');
			if (colon)
				*colon = '\0';
			snprintf(resolved_path, sizeof(resolved_path), "%s/st/st.conf", p);
			if (file_exists(resolved_path))
				return resolved_path;
			if (colon)
				p = colon + 1;
			else
				break;
		}
	} else if (file_exists("/etc/xdg/st/st.conf")) {
		return "/etc/xdg/st/st.conf";
	}

	if (file_exists("/etc/st/st.conf"))
		return "/etc/st/st.conf";

	return NULL;
}

static void
set_color(int index, const char *val)
{
	if (index >= 0 && index < ST_CONFIG_COLOR_COUNT && val && *val) {
		strncpy(g_st_config.colorname[index], val, sizeof(g_st_config.colorname[index]) - 1);
		g_st_config.has_color[index] = 1;
	}
}

static void
resolve_include_path(const char *parent_file, const char *inc_str, char *out_path, size_t maxlen)
{
	char expanded[ST_CONFIG_MAX_PATH];

	if (!inc_str || !*inc_str) {
		out_path[0] = '\0';
		return;
	}

	if (inc_str[0] == '~' && (inc_str[1] == '/' || inc_str[1] == '\0')) {
		const char *home = getenv("HOME");
		if (home) {
			snprintf(expanded, sizeof(expanded), "%s%s", home, inc_str + 1);
			inc_str = expanded;
		}
	}

	if (inc_str[0] == '/') {
		strncpy(out_path, inc_str, maxlen - 1);
		out_path[maxlen - 1] = '\0';
		return;
	}

	if (parent_file && *parent_file) {
		const char *slash = strrchr(parent_file, '/');
		if (slash) {
			size_t dirlen = slash - parent_file;
			if (dirlen >= maxlen)
				dirlen = maxlen - 1;
			strncpy(out_path, parent_file, dirlen);
			out_path[dirlen] = '\0';
			snprintf(out_path + dirlen, maxlen - dirlen, "/%s", inc_str);
			return;
		}
	}

	strncpy(out_path, inc_str, maxlen - 1);
	out_path[maxlen - 1] = '\0';
}

static int st_config_load_file(const char *path, int depth);

int
st_config_load(const char *path)
{
	g_st_config.include_count = 0;
	init_default_shortcuts();
	return st_config_load_file(path, 0);
}

static int
st_config_load_file(const char *path, int depth)
{
	FILE *fp;
	char line[1024];
	char current_section[64] = "";
	char *eq, *key, *val;
	struct stat st;

	if (!path || !*path || depth > 8)
		return 0;

	fp = fopen(path, "r");
	if (!fp) {
		if (depth > 0 && g_st_config.include_count < ST_CONFIG_MAX_INCLUDES) {
			ConfigIncludedFile *inc = &g_st_config.includes[g_st_config.include_count++];
			strncpy(inc->path, path, sizeof(inc->path) - 1);
			inc->mtime = 0;
		}
		fprintf(stderr, "[WARN] Could not open config file: %s\n", path);
		return 0;
	}

	if (depth == 0) {
		strncpy(g_st_config.loaded_path, path, sizeof(g_st_config.loaded_path) - 1);
		g_st_config.is_loaded = 1;
		if (stat(path, &st) == 0)
			g_st_config.last_mtime = (long)st.st_mtime;
	} else {

		if (g_st_config.include_count < ST_CONFIG_MAX_INCLUDES) {
			ConfigIncludedFile *inc = &g_st_config.includes[g_st_config.include_count++];
			strncpy(inc->path, path, sizeof(inc->path) - 1);
			inc->mtime = (stat(path, &st) == 0) ? (long)st.st_mtime : 0;
		}
	}

	while (fgets(line, sizeof(line), fp)) {
		char *p = trim(line);

		if (*p == '\0' || *p == '#' || *p == ';' || (p[0] == '/' && p[1] == '/'))
			continue;

		if (*p == '[') {
			char *endb = strchr(p, ']');
			if (endb)
				*endb = '\0';
			strncpy(current_section, p + 1, sizeof(current_section) - 1);
			continue;
		}

		eq = strchr(p, '=');
		if (!eq)
			eq = strchr(p, ':');

		if (eq) {
			*eq = '\0';
			key = trim(p);
			val = unquote(eq + 1);
		} else {
			char *sp = strpbrk(p, " \t");
			if (!sp)
				continue;
			*sp = '\0';
			key = trim(p);
			val = unquote(sp + 1);
		}

		if (!*key || !*val)
			continue;

		if (!strcasecmp(key, "include")) {
			char resolved[ST_CONFIG_MAX_PATH];
			resolve_include_path(path, val, resolved, sizeof(resolved));
			st_config_load_file(resolved, depth + 1);
			continue;
		}

		if (!strcasecmp(key, "font") || !strcasecmp(key, "font_family")) {
			strncpy(g_st_config.font, val, sizeof(g_st_config.font) - 1);
		} else if (!strcasecmp(key, "font_size") || !strcasecmp(key, "fontsize")) {

			if (!strstr(g_st_config.font, ":size=") && !strstr(g_st_config.font, ":pixelsize=")) {
				size_t curlen = strlen(g_st_config.font);
				snprintf(g_st_config.font + curlen, sizeof(g_st_config.font) - curlen, ":size=%s", val);
			}
		} else if (!strcasecmp(key, "sparefont") || !strcasecmp(key, "font2") ||
		           !strcasecmp(key, "fallback_font")) {
			strncpy(g_st_config.sparefont, val, sizeof(g_st_config.sparefont) - 1);
		} else if (!strcasecmp(key, "cwscale")) {
			g_st_config.cwscale = strtof(val, NULL);
		} else if (!strcasecmp(key, "chscale")) {
			g_st_config.chscale = strtof(val, NULL);

		} else if (!strcasecmp(key, "alpha") || !strcasecmp(key, "opacity") ||
		           !strcasecmp(key, "background_opacity")) {
			g_st_config.alpha = strtof(val, NULL);
			if (g_st_config.alpha < 0.0f) g_st_config.alpha = 0.0f;
			if (g_st_config.alpha > 1.0f) g_st_config.alpha = 1.0f;
		} else if (!strcasecmp(key, "alpha_unfocused") || !strcasecmp(key, "opacity_unfocused")) {
			g_st_config.alpha_unfocused = strtof(val, NULL);
			if (g_st_config.alpha_unfocused < 0.0f) g_st_config.alpha_unfocused = 0.0f;
			if (g_st_config.alpha_unfocused > 1.0f) g_st_config.alpha_unfocused = 1.0f;
		} else if (!strcasecmp(key, "borderpx") || !strcasecmp(key, "padding") ||
		           !strcasecmp(key, "window_padding_width")) {
			g_st_config.borderpx = atoi(val);
		} else if (!strcasecmp(key, "anysize_halign")) {
			g_st_config.anysize_halign = atoi(val);
		} else if (!strcasecmp(key, "anysize_valign")) {
			g_st_config.anysize_valign = atoi(val);
		} else if (!strcasecmp(key, "cols")) {
			g_st_config.cols = atoi(val);
		} else if (!strcasecmp(key, "rows")) {
			g_st_config.rows = atoi(val);

		} else if (!strcasecmp(key, "cursor_style") || !strcasecmp(key, "cursor_shape") ||
		           !strcasecmp(key, "cursorstyle") || !strcasecmp(key, "cursorshape")) {
			if (!strcasecmp(val, "block"))
				g_st_config.cursor_style = 2;
			else if (!strcasecmp(val, "beam") || !strcasecmp(val, "bar"))
				g_st_config.cursor_style = 6;
			else if (!strcasecmp(val, "underline"))
				g_st_config.cursor_style = 4;
			else
				g_st_config.cursor_style = atoi(val);
		} else if (!strcasecmp(key, "cursor_blink_timeout") || !strcasecmp(key, "blinktimeout")) {
			g_st_config.cursor_blink_timeout = atoi(val);
		} else if (!strcasecmp(key, "cursor_dynamic_color") || !strcasecmp(key, "dynamic_cursor_color")) {
			g_st_config.cursor_dynamic_color = parse_bool(val);
		} else if (!strcasecmp(key, "cursor_thickness") || !strcasecmp(key, "cursorthickness")) {
			g_st_config.cursor_thickness = atoi(val);

		} else if (!strcasecmp(key, "bold_is_not_bright") || !strcasecmp(key, "bold_not_bright")) {
			g_st_config.bold_is_not_bright = parse_bool(val);
		} else if (!strcasecmp(key, "boxdraw")) {
			g_st_config.boxdraw = parse_bool(val);
		} else if (!strcasecmp(key, "boxdraw_bold")) {
			g_st_config.boxdraw_bold = parse_bool(val);
		} else if (!strcasecmp(key, "boxdraw_braille")) {
			g_st_config.boxdraw_braille = parse_bool(val);

		} else if (!strcasecmp(key, "termname")) {
			strncpy(g_st_config.termname, val, sizeof(g_st_config.termname) - 1);
		} else if (!strcasecmp(key, "shell")) {
			strncpy(g_st_config.shell, val, sizeof(g_st_config.shell) - 1);
		} else if (!strcasecmp(key, "tabspaces")) {
			g_st_config.tabspaces = atoi(val);
		} else if (!strcasecmp(key, "bellvolume")) {
			g_st_config.bellvolume = atoi(val);
		} else if (!strcasecmp(key, "minlatency")) {
			g_st_config.minlatency = strtod(val, NULL);
		} else if (!strcasecmp(key, "maxlatency")) {
			g_st_config.maxlatency = strtod(val, NULL);
		} else if (!strcasecmp(key, "icon")) {
			if (val[0] == '~' && (val[1] == '/' || val[1] == '\0')) {
				const char *home = getenv("HOME");
				if (home)
					snprintf(g_st_config.icon, sizeof(g_st_config.icon), "%s%s", home, val + 1);
				else
					strncpy(g_st_config.icon, val, sizeof(g_st_config.icon) - 1);
			} else {
				strncpy(g_st_config.icon, val, sizeof(g_st_config.icon) - 1);
			}
		} else if (!strcasecmp(key, "xdnd_escape_chars") || !strcasecmp(key, "xdndescchar")) {
			strncpy(g_st_config.xdndescchar, val, sizeof(g_st_config.xdndescchar) - 1);
		} else if (!strcasecmp(key, "scrollback_lines") || !strcasecmp(key, "histsize")) {
			g_st_config.histsize = atoi(val);
		} else if (!strcasecmp(key, "mouse_scroll_multiplier") || !strcasecmp(key, "wheel_scroll_multiplier") ||
		           !strcasecmp(key, "scroll_multiplier")) {
			g_st_config.mouse_scroll_multiplier = atoi(val);
		} else if (!strcasecmp(key, "scrollbar")) {
			if (!strcasecmp(val, "overlay") || !strcasecmp(val, "auto"))
				g_st_config.scrollbar = SCROLLBAR_OVERLAY;
			else if (!strcasecmp(val, "always") || !strcasecmp(val, "true") || !strcasecmp(val, "yes") || !strcasecmp(val, "1"))
				g_st_config.scrollbar = SCROLLBAR_ALWAYS;
			else if (!strcasecmp(val, "hide") || !strcasecmp(val, "off") || !strcasecmp(val, "none") || !strcasecmp(val, "false") || !strcasecmp(val, "0"))
				g_st_config.scrollbar = SCROLLBAR_HIDE;
		} else if (!strcasecmp(key, "scrollbar_width") || !strcasecmp(key, "scrollbar_thickness")) {
			g_st_config.scrollbar_width = atoi(val);
		} else if (!strcasecmp(key, "scrollbar_color") || !strcasecmp(key, "scrollbar_fg")) {
			strncpy(g_st_config.scrollbar_color, val, sizeof(g_st_config.scrollbar_color) - 1);
		} else if (!strcasecmp(key, "worddelimiters") || !strcasecmp(key, "word_delimiters") ||
		           !strcasecmp(key, "select_by_word_characters")) {
			strncpy(g_st_config.worddelimiters, val, sizeof(g_st_config.worddelimiters) - 1);
		} else if (!strcasecmp(key, "doubleclicktimeout") || !strcasecmp(key, "double_click_timeout")) {
			g_st_config.doubleclicktimeout = atoi(val);
		} else if (!strcasecmp(key, "tripleclicktimeout") || !strcasecmp(key, "triple_click_timeout")) {
			g_st_config.tripleclicktimeout = atoi(val);
		} else if (!strcasecmp(key, "allowaltscreen") || !strcasecmp(key, "allow_altscreen") ||
		           !strcasecmp(key, "allow_alt_screen")) {
			g_st_config.allowaltscreen = parse_bool(val);
		} else if (!strcasecmp(key, "allowwindowops") || !strcasecmp(key, "allow_window_ops")) {
			g_st_config.allowwindowops = parse_bool(val);
		} else if (!strcasecmp(key, "image_preview_cmd") || !strcasecmp(key, "image_preview_command")) {
			strncpy(g_st_config.image_preview_cmd, val, sizeof(g_st_config.image_preview_cmd) - 1);
		} else if (!strcasecmp(key, "url_launcher") || !strcasecmp(key, "open_url_with") ||
		           !strcasecmp(key, "url_open_command")) {
			strncpy(g_st_config.url_launcher, val, sizeof(g_st_config.url_launcher) - 1);
		} else if (!strcasecmp(key, "url_prefixes") || !strcasecmp(key, "url_prefix")) {
			strncpy(g_st_config.url_prefixes, val, sizeof(g_st_config.url_prefixes) - 1);
		} else if (!strcasecmp(key, "url_style")) {
			if (!strcasecmp(val, "curly") || !strcasecmp(val, "curl"))
				g_st_config.url_style = 3;
			else if (!strcasecmp(val, "straight"))
				g_st_config.url_style = 1;
			else if (!strcasecmp(val, "double"))
				g_st_config.url_style = 2;
			else if (!strcasecmp(val, "dotted"))
				g_st_config.url_style = 4;
			else if (!strcasecmp(val, "dashed"))
				g_st_config.url_style = 5;
		} else if (!strcasecmp(key, "url_color") || !strcasecmp(key, "hyperlink_color")) {
			strncpy(g_st_config.url_color, val, sizeof(g_st_config.url_color) - 1);
		} else if (!strcasecmp(key, "underline_hyperlinks") || !strcasecmp(key, "url_underline")) {
			if (!strcasecmp(val, "hover") || !strcasecmp(val, "on_hover"))
				g_st_config.underline_hyperlinks = URL_UNDERLINE_HOVER;
			else if (!strcasecmp(val, "always") || !strcasecmp(val, "true") || !strcasecmp(val, "yes"))
				g_st_config.underline_hyperlinks = URL_UNDERLINE_ALWAYS;
			else if (!strcasecmp(val, "never") || !strcasecmp(val, "false") || !strcasecmp(val, "no") || !strcasecmp(val, "none"))
				g_st_config.underline_hyperlinks = URL_UNDERLINE_NEVER;
		} else if (!strcasecmp(key, "undercurl_style")) {
			if (strstr(val, "dense"))
				g_st_config.undercurl_style = UNDERCURL_DENSE;
			else
				g_st_config.undercurl_style = UNDERCURL_SPARSE;
		} else if (!strcasecmp(key, "url_click_modifiers") || !strcasecmp(key, "url_click_modifier") ||
		           !strcasecmp(key, "open_url_modifiers") || !strcasecmp(key, "open_url_modifier")) {
			if (!strcasecmp(val, "none") || !strcasecmp(val, "normal") || !strcasecmp(val, "false") || !strcasecmp(val, "no"))
				g_st_config.url_click_modifiers = URL_CLICK_NONE;
			else if (!strcasecmp(val, "ctrl") || !strcasecmp(val, "control"))
				g_st_config.url_click_modifiers = URL_CLICK_CTRL;
			else if (!strcasecmp(val, "shift"))
				g_st_config.url_click_modifiers = URL_CLICK_SHIFT;
			else if (!strcasecmp(val, "alt") || !strcasecmp(val, "mod1"))
				g_st_config.url_click_modifiers = URL_CLICK_ALT;
		} else if (!strcasecmp(key, "graphics_max_file_size_mb")) {
			g_st_config.graphics_max_file_size = (unsigned int)atoi(val) * 1024 * 1024;
			g_st_config.graphics_max_single_file_size = g_st_config.graphics_max_file_size;
		} else if (!strcasecmp(key, "graphics_max_ram_mb")) {
			g_st_config.graphics_max_ram_size = (unsigned int)atoi(val) * 1024 * 1024;
			g_st_config.graphics_max_total_ram_size = g_st_config.graphics_max_ram_size;
		} else if (!strcasecmp(key, "graphics_max_single_file_mb")) {
			g_st_config.graphics_max_single_file_size = (unsigned int)atoi(val) * 1024 * 1024;
		} else if (!strcasecmp(key, "graphics_total_file_mb")) {
			g_st_config.graphics_total_file_size = (unsigned int)atoi(val) * 1024 * 1024;
		} else if (!strcasecmp(key, "graphics_max_single_ram_mb")) {
			g_st_config.graphics_max_single_ram_size = (unsigned int)atoi(val) * 1024 * 1024;
		} else if (!strcasecmp(key, "graphics_max_total_ram_mb")) {
			g_st_config.graphics_max_total_ram_size = (unsigned int)atoi(val) * 1024 * 1024;
		} else if (!strcasecmp(key, "graphics_max_placements")) {
			g_st_config.graphics_max_placements = (unsigned int)atoi(val);
		} else if (!strcasecmp(key, "graphics_tolerance")) {
			g_st_config.graphics_tolerance = strtod(val, NULL);
		} else if (!strcasecmp(key, "graphics_anim_delay")) {
			g_st_config.graphics_anim_delay = (unsigned int)atoi(val);
		} else if (!strcasecmp(key, "kitty_graphics") || !strcasecmp(key, "graphics")) {
			g_st_config.kitty_graphics = !strcasecmp(val, "true") || !strcasecmp(val, "1") || !strcasecmp(val, "yes");
		} else if (!strcasecmp(key, "sixel_graphics") || !strcasecmp(key, "sixel")) {
			g_st_config.sixel_graphics = !strcasecmp(val, "true") || !strcasecmp(val, "1") || !strcasecmp(val, "yes");

		} else if (!strcasecmp(key, "pager_command") || !strcasecmp(key, "pager_cmd") || !strcasecmp(key, "pager")) {
			strncpy(g_st_config.pager_cmd, val, sizeof(g_st_config.pager_cmd) - 1);
		} else if (!strcasecmp(key, "bind") || !strcasecmp(key, "map")) {
			char *eq2 = strchr(val, '=');
			char *combo = val;
			char *act_name = NULL;
			if (eq2) {
				*eq2 = '\0';
				combo = trim(val);
				act_name = trim(eq2 + 1);
			} else {
				char *sp2 = strpbrk(val, " \t");
				if (sp2) {
					*sp2 = '\0';
					combo = trim(val);
					act_name = trim(sp2 + 1);
				}
			}
			if (act_name && *act_name) {
				unsigned int mod = 0;
				KeySym ksym = NoSymbol;
				char custom_cmd[256] = "";
				if (st_config_parse_key_combo(combo, &mod, &ksym)) {
					ShortcutAction act = st_config_parse_action(act_name, custom_cmd, sizeof(custom_cmd));
					if (act != ACT_NONE)
						st_config_add_shortcut(mod, ksym, act, custom_cmd);
				}
			}
		} else if (!strncasecmp(key, "shortcut_", 9)) {
			const char *act_name = key + 9;
			unsigned int mod = 0;
			KeySym ksym = NoSymbol;
			char custom_cmd[256] = "";
			if (st_config_parse_key_combo(val, &mod, &ksym)) {
				ShortcutAction act = st_config_parse_action(act_name, custom_cmd, sizeof(custom_cmd));
				if (act != ACT_NONE)
					st_config_add_shortcut(mod, ksym, act, custom_cmd);
			}
		} else if (!strcasecmp(current_section, "shortcuts")) {
			unsigned int mod = 0;
			KeySym ksym = NoSymbol;
			char custom_cmd[256] = "";
			ShortcutAction act = st_config_parse_action(key, custom_cmd, sizeof(custom_cmd));
			if (act != ACT_NONE && st_config_parse_key_combo(val, &mod, &ksym)) {
				st_config_add_shortcut(mod, ksym, act, custom_cmd);
			}

		} else if (!strcasecmp(key, "foreground") || !strcasecmp(key, "fg")) {
			set_color(258, val);
		} else if (!strcasecmp(key, "background") || !strcasecmp(key, "bg")) {
			set_color(259, val);
		} else if (!strcasecmp(key, "cursor") || !strcasecmp(key, "cursorcolor")) {
			set_color(256, val);
		} else if (!strcasecmp(key, "cursor_reverse") || !strcasecmp(key, "cursorcolorreverse") ||
		           !strcasecmp(key, "cursor_text_color")) {
			set_color(257, val);
		} else if (!strcasecmp(key, "selection_fg") || !strcasecmp(key, "selectionfg") ||
		           !strcasecmp(key, "selection_foreground")) {
			strncpy(g_st_config.selection_fg, val, sizeof(g_st_config.selection_fg) - 1);
		} else if (!strcasecmp(key, "selection_bg") || !strcasecmp(key, "selectionbg") ||
		           !strcasecmp(key, "selection_background")) {
			strncpy(g_st_config.selection_bg, val, sizeof(g_st_config.selection_bg) - 1);
		} else if (!strncasecmp(key, "color", 5)) {
			int idx = atoi(key + 5);
			if (idx >= 0 && idx < 256)
				set_color(idx, val);
		}
	}

	fclose(fp);
	st_config_sync_graphics();
	return 1;
}

int
st_config_reload(void)
{
	if (!g_st_config.is_loaded || !g_st_config.loaded_path[0]) {
		const char *found = st_config_find_file(NULL);
		if (found)
			return st_config_load(found);
		return 0;
	}
	return st_config_load(g_st_config.loaded_path);
}

int
st_config_check_modified(void)
{
	struct stat st;
	size_t i;

	if (!g_st_config.is_loaded || !g_st_config.loaded_path[0])
		return 0;

	if (stat(g_st_config.loaded_path, &st) == 0) {
		if (g_st_config.last_mtime != 0 && (long)st.st_mtime > g_st_config.last_mtime)
			return 1;
	}

	for (i = 0; i < g_st_config.include_count; i++) {
		if (stat(g_st_config.includes[i].path, &st) == 0) {
			if (g_st_config.includes[i].mtime == 0 || (long)st.st_mtime > g_st_config.includes[i].mtime)
				return 1;
		}
	}

	return 0;
}
