# Security

## Reporting

If you find a vulnerability, please report it privately through GitHub's security
advisory form for this repository rather than opening a public issue.

## How Swapdex handles credentials

Swapdex stores Codex credentials so that switching accounts does not require signing in
again. Those files are sensitive. The design rules are:

- Credentials are written only inside the state directory under your own user account.
- Credential files are created with owner read and write permissions only, and the
  directories holding them are owner only as well.
- Credentials are never logged, printed, or sent anywhere. The service reports status
  and errors in the interface, never file contents.
- Swapdex makes no network requests of its own. It starts Codex, and it talks to Codex
  locally over a DevTools pipe.
- At most one account is active at a time. Switching closes the app before credentials
  change, so a request can never be billed to the wrong account.
- Removing an account deletes its stored credentials and prunes the account folder.
  If you want to be certain nothing is left behind, run `swapdex uninstall --purge-data`
  and then confirm the state directory is gone.

## Trust boundary

Swapdex injects an interface layer into the Codex renderer at runtime. It does not
modify the installed application on disk. Any code with access to your user account can
already read your credentials, so this does not widen the trust boundary, but it is worth
knowing that the interface layer runs with full renderer privileges.

## Platforms

Swapdex supports macOS, Linux, and Windows. Each platform stores credentials the same
way, with owner only permissions, and each registers itself to start at sign in through
its own mechanism: systemd on Linux, a LaunchAgent on macOS, and the per user Run key
on Windows. Credentials, prompts, and tokens are never written anywhere else on any
platform.
