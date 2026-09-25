---
name: swapdex
description: Show the Swapdex accounts on this machine, which one the Codex app is signed in to, and each account's plan, five hour and weekly limits, and credits. Use when someone asks which account is active, what accounts are stored, how much quota an account has left, or how to switch Codex accounts. Also use for any request to change, add, or remove a Codex account.
---

# Swapdex accounts

Swapdex keeps several Codex accounts on this machine and shares them between the Codex
desktop app and the Codex command line tool. One account is signed in at a time.

## Show the accounts

Run this single command and report its output. It is read only and changes nothing:

```sh
swapdex accounts
```

It prints one line per account with the plan, the five hour and weekly limits, and a
marker on whichever account the desktop app is currently signed in to. Present it as a
short list and say plainly which account is active right now.

## Switching accounts

Switching cannot be done from inside a chat. It needs a real terminal, because Swapdex
has to restart the Codex app and hand your terminal back to the command line tool.

Tell the user to run this in their terminal:

```sh
swapdex tui
```

That draws a menu of their accounts. They press one, Swapdex switches the desktop app
to that same account, and Codex starts again on the chosen account. There is no way to
do this from inside a Codex session, so do not try to run it yourself, and do not run
any command that changes which account is active.

## Adding and removing

Both need a terminal as well, for the same reason:

```sh
swapdex add          # add an account and walk through sign in
swapdex tui           # press r on an account to remove it, with a confirmation
```

## Rules

- Only ever run `swapdex accounts` unless the user has explicitly asked for something
  else and you have explained that it must be done in their terminal.
- Never read, print, copy, or ask for credentials, tokens, or the contents of any
  `auth.json`.
- If Swapdex is not installed, say so and point at the install command in the README.
