# mcd

A mouse-driven TUI tool for quick `cd` in the terminal.

## Features

- Two views: **custom path list** and **directory browser**, switch with `Tab`
- Mouse support: click / double-click / scroll wheel / right-click to quit
- Click the path bar to jump to any parent directory
- Fuzzy filter while typing (case-insensitive)
- Browse list always shows `.` (current) and `..` (parent)
- Symlink handling:
  - default: keep logical path (no `realpath`)
  - `-P`: resolve symlinks to physical path
- Auto redraw on terminal resize (`SIGWINCH`)
- Subshell mode (`-s`) for shells that can't be wrapped
- Configurable custom paths via file or environment variable

## Build

```sh
make
```
Or directly:

```sh
cc -O2 -Wall -Wextra -o mcd mcd.c
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
mcd -h         # show help
```

## Shell integration

External program cannot change the parent shell's cwd. Use a shell
function so that `mcd` really `cd`s you:

### Bash / Zsh

```sh
mcd() {
    for arg in "$@"; do
        case $arg in
            -h|--help|-s)
                command mcd "$@"
                return
                ;;
        esac
    done

    local dir

    dir="$(command mcd "$@" < /dev/tty)" || return 0

    if [ -n "$dir" ]; then
        cd -- "$dir"
    fi
}
```

### Fish
```fish
function mcd
    for arg in $argv
        switch $arg
            case -h --help -s
                command mcd $argv
                return
        end
    end

    set -l dir (command mcd $argv </dev/tty)
    if test $status -eq 0 -a -n "$dir"
        cd $dir
    end
end
```

Then bind it to a key, e.g. `Ctrl-g`:

### Bash
```bash
bind '"\C-g":"\C-a\C-kmcd\C-m"'
```

### Zsh
```zsh
mcd-widget() {
    BUFFER="mcd"
    zle .accept-line
    zle reset-prompt
}
zle -N mcd-widget
bindkey '^G' mcd-widget
```

### Fish
```fish
bind \cg 'mcd; commandline -f repaint'
```

## Keys

- `Tab` switch between custom / browse view
- `Left` parent directory in browse view
- `Right` enter selected directory
- `Ctrl+P` change resolve mode
- `Enter` confirm and output path
- `Esc` / `Ctrl+C` quit
- Type to filter, `Backspace` to delete

## Mouse

- Left click: select
- Double click: enter / confirm
- Wheel: scroll
- Right click: quit
- Click the path bar: jump to that parent
- Click buttons: `[C]` `[B]` `[..]` `[.]` `[OK]` `[X]`

## Configuration

Custom paths are read from (in order):

- Command-line arguments
- `$MCD_FILE` (a text file, one path per line, default: `~/.mcd_dirs`)
- `$MCD_PATHS` (colon-separated list)

Defaults: `$HOME`, `/`, `/tmp`, `/usr`, `/etc`

Lines starting with `#` and blank lines are ignored. `~` is expanded to
`$HOME`.

Example `~/.mcd_dirs`:

```text
~
~/proj
~/downloads
/etc
```

## License

MIT
