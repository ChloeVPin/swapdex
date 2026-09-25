# Swapdex

Swapdex is an add-on for the Codex desktop app.

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

**The same accounts in your terminal.** Swapdex stores credentials per account, so the
Codex command line tool can use any of them too:

```sh
swapdex shell --list                  # accounts you can run
swapdex shell work                    # start Codex as the work account
swapdex usage                         # limits and credits for the signed in account
swapdex whoami                        # who the terminal is set to
```

`swapdex shell` hands the rest of the command straight to Codex, so the usual flags
still work. Each account keeps its own session history, because the CLI stores that
under the account it was started with.

To have plain `codex` follow the account signed in to the app, run this once:

```sh
swapdex shell-init
```

It adds a small wrapper to your shell startup file and never touches anything else in
it. Bypass it for a single command with `SWAPDEX_FOLLOW=0 codex`, or remove it with
`swapdex shell-init --remove`.

**A menu in the terminal.** For a visual picker instead of commands:

```sh
swapdex tui
```

It draws your accounts with plan and limits, and you press one to switch. Swapdex asks
the running service to do the swap so there is only ever one writer of your
credentials, waits for it to finish, and then starts Codex on the same screen as that
account, so it feels like the terminal just became the other account. `a` adds an
account, `r` removes one behind a confirmation, and `q` leaves without changing
anything. Add `--no-relaunch` if you would rather go back to your shell.

**Limits inside the terminal.** Codex draws its own status line, and Swapdex never
renders into it. But because the CLI is authenticated as whichever account you picked,
Codex's built in rate limit items show that account's real limits. To turn them on, run
`/statusline` inside Codex, or let Swapdex add them for you with
`swapdex statusline --apply`, which keeps a backup of your config.

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
and registers it to start when you sign in.

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
| `swapdex shell [account]` | Start the Codex command line tool as that account. |
| `swapdex shell --list` | List accounts the terminal can use. |
| `swapdex usage` | Show limits and credits for the signed in account. |
| `swapdex whoami` | Show which account the terminal would use. |
| `swapdex shell-init` | Make plain `codex` follow the app account. |
| `swapdex statusline` | Explain and optionally enable the rate limit items. |
| `swapdex tui` | Open the interactive account picker. |

None of these touch Codex itself.

## Requirements

- macOS, Linux, or Windows on a 64 bit machine
- The official Codex desktop app, already installed and signed in

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
src/            the service, account store, platform layer, and CLI

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
