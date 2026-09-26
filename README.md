# Swapdex

Swapdex is an add-on for the Codex desktop app. The app is the only thing it works
with.

It attaches to the Codex you already have. It never installs Codex, never updates
Codex, and never changes the app on disk. If you uninstall Swapdex, Codex is exactly
as it was.

## What it adds

**A real account switcher.** Add an account once, then move between accounts from the
profile menu with one click. No signing out, no signing in, no waiting for the app to
restart.

**Accounts that stay ready.** Swapdex keeps your credentials warm on a schedule you
choose, so an account you have not opened in weeks still switches instantly.

**Live usage, where you need it.** Five hour and seven day limits, credit balances,
reset credits, and the date your next reset lands, shown in the profile menu and on
the accounts page.

**A settings page with the useful tabs.** Privacy blurs your account names everywhere
they appear. Accounts lists everything you have stored. Menu lets you hide the rows in
the profile menu you never use, while Add account and Settings always stay visible.

**Safe removal.** Removing an account always asks you to confirm first. A secondary
account is deleted with its stored credentials and Codex keeps running. Removing the
account you are signed in with signs you out and clears its credentials.

**One account at a time.** Swapdex stores credentials per account, but only one is
signed in at a time. The account you pick in the app is the account the app runs as,
and nothing else has to be configured.

**Swapdex is for the Codex desktop app only.** It does not integrate with the Codex
command line tool. There is no shell wrapper, no slash command, and nothing to install
into your shell. If you use the command line tool, that is outside what this project
does and outside what it supports.

**Where account data lives.** Each account gets its own folder in `~/.swapdex-accounts`,
holding only that account's credentials. The account you pick is copied into the Codex
home the app reads, so the app signs in as that account.

## Install

Swapdex supports macOS, Linux, and Windows. Pick the line for your system.

**macOS and Linux**, in a terminal:

```sh
curl -fsSL https://raw.githubusercontent.com/ChloeVPin/swapdex/main/install.sh | sh
```

**Windows**, in PowerShell:

```powershell
irm https://raw.githubusercontent.com/ChloeVPin/swapdex/main/install.ps1 | iex
```

That is the whole install. The script picks the build for your machine, installs it,
and registers it to start when you sign in. It downloads a published release, so a
release has to exist for your platform.

Swapdex does not install Codex. If you do not have it yet, install it from OpenAI
first, then run the line above. After that, just open Codex as you normally would and
the profile menu will have your accounts in it.

To reinstall a newer version later, run the same line again.

## Commands

| Command | What it does |
| --- | --- |
| `swapdex list` | Show stored accounts and their usage. |
| `swapdex add "name"` | Add an account and walk through sign in. |
| `swapdex start` | Start Swapdex now. |
| `swapdex stop` | Stop Swapdex. |
| `swapdex status` | Check whether Swapdex is installed and running. |
| `swapdex uninstall` | Remove Swapdex and keep your accounts. |
| `swapdex uninstall --purge-data` | Remove Swapdex and delete stored account data. |
| `swapdex accounts` | List accounts and limits in the terminal, read only. |

None of these touch Codex itself.

## Requirements

- macOS, Linux, or Windows on a 64 bit machine
- The official Codex desktop app, already installed and signed in
- No support for the Codex command line tool

### What works where

Swapdex is written for all three, but only one of them has been run on real hardware.
Please read this before you install it.

| Platform | Builds and passes tests | Tested against a real signed in app | How it starts |
| --- | --- | --- | --- |
| Linux | yes, in CI and on this machine | yes, daily | a systemd user service |
| macOS | yes, in CI and on a real Mac | in progress, a QA pass found and fixed several defects | a launchd agent |
| Windows | **no, does not compile yet** | no | a login item |

Linux is the only platform with an end to end test against a signed in app, so that is
the only one this project calls supported. macOS compiles cleanly and passes the test
suite on every commit, but nobody has run it against a real app yet, so treat it as
unproven rather than working.

Windows does not build. The local browser connection and the app-server transport are
written against POSIX pipes, poll, and inherited file descriptors, and the Windows
equivalents have not been written. The build job for it is present and expected to fail,
so the gap stays visible. Nothing about the account switching, credentials, or settings
is Windows specific, so this is a transport port rather than a redesign.

If you are on macOS or Windows and it works for you, that is genuinely useful
information and an issue describing what happened is welcome.

## Uninstalling

Run `swapdex uninstall`. Swapdex removes itself and leaves your stored accounts alone
if you want to reinstall later. Add `--purge-data` and it deletes the stored
credentials too.

Either way, Codex stays installed and untouched.

## How your data is handled

- Your credentials are stored only on your own machine, in files that only your user
  account can read.
- Swapdex has no telemetry, no analytics, and no update checks. It makes no network
  requests of its own beyond launching Codex and talking to it locally.
- Your prompts, messages, and tokens are never read, logged, or stored. Only plan
  names and usage numbers are kept.
- One account is active at a time. Switching closes Codex before credentials change, so
  a request can never be billed to the wrong account.
- The privacy blur hides account names on screen and blocks copying them.
- Removing an account deletes its stored credentials right away.

## Performance

Measured on a four core machine with 3.5 GB of RAM while idle:

| | With Swapdex | Without |
| --- | --- | --- |
| Swapdex itself | about 5 MB and 0.1 percent of one core | not running |
| Codex total | 4.6 percent of one core | 2.7 percent of one core |
| Codex total memory | 550 to 575 MB | 522 to 524 MB |

Swapdex is a small background service. The extra time you see comes from the account
interface and the local connection to Codex, not from Swapdex doing heavy work.

## Troubleshooting

**The profile menu does not show my accounts.** Run `swapdex status`. If Swapdex is
not running, run `swapdex start`. If Codex was already open before Swapdex, close it
fully and reopen it.

**Adding an account fails.** Swapdex needs to own the Codex window while it signs a
new account in. Close any Codex window that is already open, then run
`swapdex add`.

**Everything looks wrong after a Codex update.** The interface is injected at runtime,
so a large Codex release can change the markup Swapdex builds on. Pull the newest
Swapdex and, if it is still wrong, open an issue with the Codex version you are on.

## Building from source

You only need this if you want to change Swapdex itself.

```sh
git clone https://github.com/ChloeVPin/swapdex.git
cd swapdex
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

The tests cover the account store, the service layer, the platform layer, and the
installer. A live interface probe renders the real app when you want to check the
interface end to end:

```sh
cmake --build build --target swapdex_live_probe
./build/swapdex_live_probe --logged-in assets/inject.js
```

## Project layout

```
src/            the service, account store, platform layer, and commands

assets/         the interface that is injected into Codex
tests/          unit tests and the live interface probe
install.sh      one line installer for macOS and Linux
install.ps1     one line installer for Windows
```

## Status

Version 0.2.0. Built and verified on Linux against the current Codex desktop release.
macOS and Windows are built and tested by continuous integration, but the desktop
integration on those platforms has not been verified on real hardware yet, so treat it
as experimental there.
