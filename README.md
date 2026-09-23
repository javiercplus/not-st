# not-st

An `st` fork with Kitty Graphics Protocol support and external runtime configuration (`st.conf`).

---

## Features & Included Capabilities

1. **Kitty Graphics Protocol** (based on `sergei-grechanik/st-graphics`)
   - Full support for image preview and inline rendering: `kitten icat`, `yazi`, `ranger`, `chafa`, `timg`, `snacks.nvim`, `image.nvim`, `fzf` previews.
   - Supports PNG, RGB, RGBA, JPEG, zlib compression, shared memory (`t=s`), direct uploads (`t=d`), Unicode placeholders.
2. **Font Ligatures** (HarfBuzz text shaping)
   - Renders programming ligatures (`!=`, `==`, `===`, `<=`, `>=`, `->`, `=>`, `/*`, `*/`) seamlessly.
3. **Boxdraw**
   - Crisp rendering of box-drawing and block characters (U+2500 to U+259F) without gaps or font-dependent misalignment.
4. **Scrollback & Mouse Wheel**
   - History scrollback using `Shift+PageUp` / `Shift+PageDown`.
   - Mouse wheel scrolling: automatically scrolls terminal buffer at prompt, and passes through scroll keys when in alternate screen applications (`vim`, `less`, `tmux`, `htop`).
5. **Alpha & True Transparency (ARGB 32-bit)**
   - True window background opacity without dimming text glyphs.
   - Configurable focused and unfocused opacity (`alpha` and `alpha_unfocused`).
   - Dynamic opacity adjustments via shortcuts (`Alt+a`, `Alt+s`, `Alt+m`).
6. **External Configuration File (`st.conf`)**
   - Configure fonts, fallback fonts, colors (Tokyo Night, Catppuccin, Gruvbox, etc.), padding, opacity, cursor styles, latency, and window settings in `~/.config/st/st.conf` without recompiling.
   - Live configuration reload without restarting terminal (`Ctrl+Shift+F5` or `kill -USR1 $(pidof st)`).
7. **Blinking Cursor & Dynamic Cursor Color & Cursor Shapes**
   - Shapes: Blinking Block, Steady Block, Blinking Underline, Steady Underline, Blinking Bar, Steady Bar.
   - DECSCUSR escape sequence support: cursor dynamically changes when switching between insert and normal mode in Neovim/Vim.
   - Dynamic cursor color: reverses color of the character underneath for maximum visibility.
8. **Native Fullscreen Toggle (EWMH)**
   - Press `F11` to toggle borderless fullscreen seamlessly via `_NET_WM_STATE_FULLSCREEN`.
9. **Spare Fonts / Fallback Fonts**
   - Automatic fallback to Nerd Fonts, Symbola, or Emoji fonts for unmapped characters.
10. **Bold is not bright**
    - Bold font styling does not distort or wash out ANSI colors 0-7.
11. **Modern Underline / Undercurl Support**
    - Curly underlines (undercurl), double underlines, dotted, dashed, and colored underlines for LSP diagnostics and spelling errors.
12. **Synchronized Updates (appsync / \033[?2026h])**
    - Tear-free screen updates during fast output.
13. **Anysize Window Centering**
    - Smooth window resizing to arbitrary pixel dimensions with configurable centering alignment.
14. **Clipboard Integration**
    - `Ctrl+Shift+C` (copy) and `Ctrl+Shift+V` (paste) with X11 PRIMARY and CLIPBOARD synchronization.
15. **URL Handler**
    - `Alt+u` to select and open URLs in default browser.
    - `Alt+y` to copy URLs to clipboard.
    - Generic URL detection: any `scheme://...` (http, https, ftp, git, gemini, gopher, ...) is detected by shape; additional no-`://` schemes (`magnet`, `mailto`, ...) are configured via `url_prefixes` in `st.conf`.
16. **Zoom Keybindings**
    - `Ctrl + Plus` / `Ctrl + =` (Zoom In)
    - `Ctrl + Minus` (Zoom Out)
    - `Ctrl + 0` (Zoom Reset)
17. **Desktop Integration & Window Icon**
    - `st.desktop` entry and native `_NET_WM_ICON` PNG loading via Imlib2.

---

## Configuration

Configuration file search order:
1. `-C /path/to/config.conf` command line argument
2. `$ST_CONFIG` environment variable
3. `$XDG_CONFIG_HOME/st/st.conf` or `~/.config/st/st.conf`
4. `$XDG_CONFIG_HOME/st/config.toml` or `~/.config/st/config.toml`
5. `~/.st.conf`
6. `/etc/st/st.conf`
7. Internal defaults (`config.def.h`)

To get started, copy the provided example configuration:

```sh
mkdir -p ~/.config/st
cp examples/st.conf.example ~/.config/st/st.conf
```

---

## Keybindings Cheatsheet

| Action | Keybinding |
|---|---|
| Toggle Fullscreen | `F11` |
| Zoom in | `Ctrl + Plus` / `Ctrl + =` / `Ctrl + Shift + PageUp` |
| Zoom out | `Ctrl + Minus` / `Ctrl + Shift + PageDown` |
| Zoom reset | `Ctrl + 0` / `Ctrl + Shift + Home` |
| Copy selection | `Ctrl + Shift + C` |
| Paste clipboard | `Ctrl + Shift + V` / `Shift + Insert` |
| New terminal (CWD) | `Ctrl + Shift + Return` |
| Scrollback pager | `Ctrl + Shift + H` |
| Scrollback Up / Down | `Shift + PageUp` / `Shift + PageDown` |
| Mouse Scroll | Mouse Wheel (buffer scroll at prompt; passthrough in vim/less) |
| Open URL | `Alt + u` (via `st-urlhandler`) |
| Copy URL | `Alt + y` (via `st-urlhandler`) |
| Increase Opacity | `Alt + a` (+5%) |
| Decrease Opacity | `Alt + s` (-5%) |
| Toggle Full Opacity | `Alt + m` (100% opaque) |
| Reload Configuration | `Ctrl + Shift + F5` (or `kill -USR1 $(pidof st)`) |
| Kitty Image Preview | `Ctrl + Shift + Right Click` on placeholder |
| Kitty Image Debug Info | `Ctrl + Shift + Middle Click` |

---

## Building and Installation

### Dependencies
- `libX11`, `libXft`, `libXrender`
- `fontconfig`, `freetype2`
- `harfbuzz`
- `imlib2`, `zlib`

### Compile & Install
```sh
make clean
make
sudo make install
```

### Install Terminfo (if needed)
```sh
tic -sx st.info
```

---

## Credits & Upstream Projects

This project is built upon the work of the suckless community and several modern `st` forks:

- **[st upstream](https://git.suckless.org/st/)** — The original simple terminal emulator by the suckless.org team.
- **[st-graphics](https://github.com/sergei-grechanik/st-graphics)** by Sergei Grechanik — Kitty Graphics Protocol implementation and inline image rendering engine.
- **[st-sx](https://github.com/veltza/st-sx)** by veltza — Modern X11 features, patches, and enhancements.
- **[st](https://github.com/mohkale/st)** by mohkale — Scrollback reflow architecture and standalone buffer concepts.
- **[st](https://github.com/zootedb0t/st)** by zootedb0t — Dynamic cursor styling, themes, and modern TUI integrations.
- **[khash / kvec](https://github.com/attractivechaos/klib)** by Attractive Chaos — Lightweight hash map and dynamic vector headers.
- **HarfBuzz & Boxdraw** — Font ligatures shaping and crisp box-drawing patch contributors.

## License

Distributed under the MIT/X Consortium License. See [LICENSE](LICENSE) for details.
