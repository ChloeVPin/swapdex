# Swapdex

Swapdex runs the official Codex desktop app as a local service and adds the things
the app is missing: a real account switcher, credentials that stay ready, live usage
details, and a settings page that actually shows what is going on.

It does not patch or modify Codex. The app stays exactly as OpenAI ships it, and
Swapdex drives it the same way a remote debugger would.

## The problem it solves

Using more than one Codex account normally means signing out, signing in, waiting for
the app to restart, and hoping the right account loaded. Swapdex keeps a separate set of
credentials for every account you add, so switching is a single click in the profile
menu. Your conversations, settings, and caches stay per account, and only one account
is ever signed in at a time.

## What you get

**Account switching from the profile menu.** Add an account once, then move between
accounts without signing in again. The current account sits at the top, saved accounts
sit below it, and the row you pick becomes the active account.

**Accounts that stay warm.** Swapdex refreshes stored credentials on a schedule you
choose, so an account you have not opened in a week still switches instantly. You can
also warm a single account on demand. No chats and no model requests are sent while
warming, it only refreshes the stored session.

**Live usage where you need it.** Five hour and seven day limits, general credits,
reset credits, and the date your next reset lands, shown both in the profile menu and on
the accounts settings page.

**A settings page with the useful tabs.**

- *Privacy* turns on a blur for account names everywhere they appear, including the
  account button at the bottom of the sidebar.
- *Accounts* lists every stored account with its plan, avatar, usage, credits, warm up
  control, and a remove button.
- *Menu* lets you hide optional rows in the profile menu. Add account and Settings stay
  visible so you can never get locked out.

**Safe account removal.** Removing an account always asks for confirmation first. A
secondary account is deleted along with its stored credentials and the app is left
alone. Removing the account you are signed in with signs you out, deletes its
credentials, and moves you to another stored account if one is available.

## Requirements

- Linux with systemd user services
- CMake 3.20 or newer and a C++20 compiler
- nlohmann_json 3.11 or newer
- The official ChatGPT or Codex desktop app installed

## Install

```sh
git clone https://github.com/ChloeVPin/swapdex.git
cd swapdex
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
./build/swapdex install
```

The install command copies the binary and the interface asset into place, writes a
systemd user unit, and starts the service. The app then launches under Swapdex the next
time you open it.

## Commands

| Command | What it does |
| --- | --- |
| `swapdex install` | Install the service and start it. Use `--no-start` to skip starting. |
| `swapdex start` | Start the service and the app. |
| `swapdex stop` | Stop the service and the app. |
| `swapdex status` | Show whether the service is running. |
| `swapdex list` | List stored accounts and their usage. |
| `swapdex add "name"` | Add an account and walk through sign in. |
| `swapdex launch` | Launch the app under Swapdex. |
| `swapdex uninstall` | Remove the service. Add `--purge-data` to also delete stored accounts. |
| `swapdex version` | Print the version. |

## How it works

The service starts the Codex desktop app with a DevTools pipe attached, then injects a
small interface layer into the renderer. That layer is what draws the account rows and
the settings page, and it sends a short list of commands back to the service over the
same pipe.

Credentials live in a private state directory, one folder per account, alongside a small
registry that records which account is active. Switching accounts closes the app,
swaps the active credentials, and starts the app again. It is the slowest possible
approach, and that is on purpose: it guarantees the app is never holding two identities
at once and that no request can be billed to the wrong account.

Warming an account spawns the app's own app server against that account's folder, reads
the account and rate limit responses, writes them back to the registry, and exits. It
never sends a chat.

## Privacy and security

- Credentials are written only under your own user account, in files with owner only
  permissions, inside directories with owner only permissions.
- Swapdex has no telemetry, no analytics, and no update check. It makes no network
  requests of its own beyond starting Codex and talking to it locally over a pipe.
- Prompts, tokens, and message content are never read, logged, or stored. Usage
  numbers and plan names are the only account data kept.
- Account switching is a full app restart, so a single account is active at any moment.
- The privacy blur hides account names on screen and blocks copying them.
- Removing an account deletes its stored credentials immediately.

## Performance

Measured on a four core machine with 3.5 GB of RAM, idle, over 25 second samples:

| Measurement | With Swapdex | Without |
| --- | --- | --- |
| Swapdex service CPU | 0.1 percent of one core | not running |
| Swapdex service memory | about 5 MB | not running |
| Codex renderers and app CPU | 4.6 percent of one core | 2.7 percent of one core |
| Total memory | 550 to 575 MB | 522 to 524 MB |

The service itself is a few megabytes and effectively idle. The rest of the difference
is the injected interface and the DevTools connection, which is where nearly all of the
cost sits. For comparison, Codex uses roughly 50 percent of a single core for the first
couple of minutes after launch regardless of Swapdex.

If memory is tight, closing extra Codex windows and tabs saves far more than disabling
Swapdex does.

## Development

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

Sanitizer build:

```sh
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"
cmake --build build-sanitize -j"$(nproc)"
ctest --test-dir build-sanitize --output-on-failure
```

The live interface probe renders the real app and checks the account rows, the settings
tabs, the interval picker, the removal dialog, and the privacy blur against a running
Codex instance:

```sh
cmake --build build --target swapdex_live_probe
./build/swapdex_live_probe --logged-in assets/inject.js
```

## Project layout

```
src/            service, account store, app server client, DevTools pipe, CLI
assets/         the injected renderer interface
tests/          unit tests, fakes, and the live interface probe
packaging/      reserved for distribution files
```

## Status

Version 0.2.0, built and verified against the current Codex desktop release. The
interface is injected at runtime, so a Codex update that changes the profile menu or
settings markup may need the interface layer adjusted.
