# mcd

A mouse-driven TUI tool for quick `cd` in the terminal.

## Features

- Mouse click / double-click / scroll wheel support
- Two views: custom path list and directory browser
- Click the path bar to jump to any parent directory
- Fuzzy filter while typing
- Symlink handling: logical path by default, `-P` for physical path
- Auto redraw on terminal resize (SIGWINCH)
- No third-party dependencies, single C file

## Build

```sh
make
```

## Install

```sh
sudo make install
```

Default install path is `/usr/local/bin/mcd`. Override with `PREFIX`:

```sh
sudo make install PREFIX=/usr
```

## Usage

```sh
mcd            # default: keep symlink path
mcd -P         # physical mode: resolve symlinks
mcd -s         # choose a dir and start a subshell there
mcd ~/proj     # only use given custom dirs
```

## Shell integration

External programs cannot change the parent shell's cwd. Use a shell
function to make `mcd` really `cd`:

### Bash / Zsh

```sh
mcd() {
    local run=0
    local skip=0
    for arg in "$@"; do
       case $arg in
            -h|-P|--help|--physical) [ $skip -eq 0 ] && run=1 ;;
            -s|--) skip=1 ;;
        esac
    done

    if [ $run -eq 1 ]; then
        command mcd "$@"
        return
    fi

    local dir

    dir="$(command mcd "$@" < /dev/tty)" || return 0

    if [ -n "$dir" ]; then
        cd -- "$dir"
    fi
}
```

Then bind it to a key, e.g. `Ctrl-g`:

```sh
# Bash
bind -x '"\C-g":"mcd"'

# Zsh
mcd-widget() {
    zle -I
    mcd
    zle reset-prompt
}
zle -N mcd-widget
bindkey '^G' mcd-widget
```

## Keys

- `Tab` switch between custom / browse view
- `Left` parent directory in browse view
- `Right` enter selected directory
- `Enter` confirm and output path
- `Esc` / `Ctrl-C` quit
- Type to filter, `Backspace` to delete

## Mouse

- Left click: select
- Double click: enter / confirm
- Wheel: scroll
- Right click: quit
- Click the path bar: jump to that parent

## Config

Custom paths are read from `~/.mcd_dirs`, or the file in `$MCD_FILE`,
or the colon-separated list in `$MCD_PATHS`.

Example `~/.mcd_dirs`:

```text
~
~/proj
~/downloads
/etc
```

## License

MIT
