#!/bin/sh
# Swapdex installer for macOS and Linux.
#
# Downloads the build for this machine, installs it, and registers it to start at
# sign in. Swapdex attaches to the Codex app you already have and never installs
# or replaces Codex itself.

set -eu

repository="ChloeVPin/swapdex"
install_url="https://raw.githubusercontent.com/${repository}/main/install.sh"

say() {
    printf '%s\n' "$*"
}

fail() {
    printf 'swapdex: %s\n' "$*" >&2
    exit 1
}

need() {
    command -v "$1" >/dev/null 2>&1 || fail "$1 is required but was not found"
}

detect_platform() {
    os="$(uname -s)"
    case "$os" in
        Darwin) printf 'macos' ;;
        Linux) printf 'linux' ;;
        *) fail "Swapdex does not support $os yet" ;;
    esac
}

detect_arch() {
    case "$(uname -m)" in
        x86_64 | amd64) printf 'x64' ;;
        arm64 | aarch64) printf 'arm64' ;;
        *) fail "Swapdex does not support the architecture $(uname -m) yet" ;;
    esac
}

existing_swapdex() {
    for candidate in "$HOME/.local/bin/swapdex" "$HOME/bin/swapdex"; do
        if [ -x "$candidate" ]; then
            printf '%s' "$candidate"
            return 0
        fi
    done
    return 1
}

main() {
    need curl
    need tar
    need uname

    platform="$(detect_platform)"
    arch="$(detect_arch)"
    say "Swapdex installer"
    say "  platform: ${platform}"
    say "  arch:     ${arch}"

    if ! command -v codex >/dev/null 2>&1 && [ ! -d "/Applications/Codex.app" ] && [ ! -d "/Applications/ChatGPT.app" ] && [ ! -x "/usr/lib/chatgpt/ChatGPT" ]; then
        say "  note:     Codex was not found. Install Codex first, then run swapdex install."
    fi

    version="${SWAPDEX_VERSION:-latest}"
    if [ "$version" = "latest" ]; then
        asset="swapdex-${platform}-${arch}.tar.gz"
        url="https://github.com/${repository}/releases/latest/download/${asset}"
    else
        asset="swapdex-${platform}-${arch}.tar.gz"
        url="https://github.com/${repository}/releases/download/v${version}/${asset}"
    fi

    workdir="$(mktemp -d)"
    trap 'rm -rf "$workdir"' EXIT INT TERM

    say "  downloading: ${asset}"
    if ! curl -fsSL "$url" -o "$workdir/swapdex.tar.gz"; then
        # A missing asset and a broken network look the same to curl, so say which
        # build is missing rather than leaving the reader to guess.
        if ! curl -fsSL --head "$url" >/dev/null 2>&1; then
            fail "there is no published build for ${platform}-${arch} yet. Releases are at ${repository}/releases"
        fi
        fail "the download failed. Check your network and try again: ${url}"
    fi

    mkdir -p "$workdir/bundle"
    tar -xzf "$workdir/swapdex.tar.gz" -C "$workdir/bundle"
    if [ ! -f "$workdir/bundle/swapdex/swapdex" ]; then
        fail "the downloaded archive did not contain the Swapdex binary"
    fi

    chmod +x "$workdir/bundle/swapdex/swapdex"
    "$workdir/bundle/swapdex/swapdex" install

    say ""
    say "Swapdex is installed and will start with Codex."
    say "Commands: swapdex status, swapdex list, swapdex add, swapdex uninstall"
    # The binary lands in ~/.local/bin, which is not on every user's PATH. The
    # service does not care, but the commands above do.
    case ":${PATH}:" in
        *":${HOME}/.local/bin:"*) ;;
        *) say "  note: ${HOME}/.local/bin is not on your PATH, so the swapdex command will not resolve there. Add it to use the commands above." ;;
    esac
    say "To reinstall a newer version, run this command again."
}

main "$@"
