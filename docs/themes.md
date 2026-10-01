# Themes

A theme is a named set of styles, one per role (the roles and what each paints: [settings.md](settings.md#styles-and-themes)). Pick one with `theme = "NAME"` in settings, or `:theme NAME` in a session, which switches live.

## Shipped

| Theme | What |
| :--- | :--- |
| `default` | The built-in look, in effect when no theme is set. `themes/default.lua` lists every role with its default, so it is the file to copy when writing your own. |
| `gruvbox-dark` | The gruvbox palette (morhetz/gruvbox, medium contrast) for a dark terminal: errors red, added lines green, notices yellow, links blue, headings orange and bold, comments grey. |
| `gruvbox-light` | The same mapping with gruvbox's light palette (the faded accents), for a light terminal. |
| `mono` | No colours: bold, dim, underline and inverse only, for a terminal without colour or for screenshots. |

MAIC paints no background of its own, so a theme looks right on a terminal whose background matches its `background` field.

## Writing one

A theme is a Lua file in `~/.config/maic/themes/NAME.lua` (`$XDG_CONFIG_HOME` respected) returning a table:

```lua
return {
  name = "dusk",            -- for the reader; the file name is the theme's name
  background = "dark",      -- "dark" or "light": the terminal background it is meant for
  styles = {
    error = { fg = "#e06c75", bold = true },
    md_heading = { fg = "#c678dd", bold = true, underline = true },
    visual = { bg = "#3e4451" },
    ["mode_auto-read"] = { fg = "#56b6c2", bold = true },
  },
}
```

A style takes `fg`, `bg` (`#rrggbb`, a colour name, or 0-255), `bold`, `dim`, `italic` (shown as dim), `underline` and `inverted`. A theme may set any subset of the roles; a role it sets replaces the default role whole, a role it leaves out keeps the default. A user theme with the name of a shipped one shadows it.

`:theme reload` re-reads the active theme's file, so a theme can be edited while looking at it. A theme that fails to load (a Lua error, an unknown role or key, a colour that is not one) is an error that names the file and line; the session keeps the theme it had. At start, a broken `theme` in settings is reported the same way and the default is used.

Precedence: the built-in default, then the theme, then the `style` entries in settings, which merge over single roles (`style = { user = { fg = "#ff8800" } }` keeps the theme's bold). A settings file written by an older `maic settings init` lists every default under `style`; delete that block, or a theme cannot show through it.

## Importing from neovim

`:theme nvim:NAME` (or `maic themes import NAME [--as FILE_NAME]` outside a session) turns one of your neovim colorschemes into a theme:

1. MAIC runs `nvim --headless -i NONE -n --cmd 'let g:maic_theme_import = 1'` with your own configuration and runtime path, so the colorschemes your plugin manager installed are found. A config can test `g:maic_theme_import` to skip heavy plugins. stdin is `/dev/null` and nvim runs in its own process group, killed with everything it started after 15 s, so a plugin manager that tries to install in headless mode cannot hang it.
2. In nvim: `colorscheme NAME`, then `'background'` and the resolved highlight groups (`nvim_get_hl` with `link = false`) are written as JSON to a temporary file.
3. The groups are mapped onto the roles by the table in [settings.md](settings.md#from-neovim-colorschemes) and written to `~/.config/maic/themes/nvim-NAME.lua` (or `FILE_NAME.lua`), a plain theme file with a header saying where it came from and when. It loads without nvim from then on; re-importing overwrites it, so copy it under another name before editing.
4. The session switches to it.

Tab after `:theme nvim:` lists the colorschemes nvim has (`getcompletion('', 'color')`, asked once in the background). A name nvim does not know is an error saying so.

## Colour depth

Hex colours are sent as they are when the terminal has truecolor (`COLORTERM` is `truecolor` or `24bit`). Otherwise each becomes the nearest xterm-256 colour (the 6x6x6 cube and the grey ramp, by squared distance in sRGB) when `TERM` says 256, else the nearest of the 16 ANSI colours. `colors = "truecolor" | "256" | "16"` in settings overrides the detection.
