# Working on Swapdex

This file is the contract for how changes land in this repository. It applies to
humans and to coding agents equally. If anything here conflicts with a tool's
defaults, this file wins.

## Writing style

These rules cover every tracked text file: README, docs, code comments, commit
messages, pull request text, and this file itself.

- No em-dashes or en-dashes in prose. Break the sentence, use a comma, or use a
  colon instead.
- No emojis. Not in docs, not in comments, not in commit messages, not in PR
  text.
- Plain sentences and short paragraphs. Wrap prose around 100 columns to match
  the existing files.
- GitHub markdown only where it earns its place: headings, code fences, tables
  for genuinely tabular data, and links. No badges, no decorative dividers, no
  callout blocks used as decoration.
- Sentence case for headings.
- Code comments explain why, not what. Match the established comment voice in
  the surrounding file.

## Code

- Minimal, focused edits. Edit existing files rather than adding new ones.
- Follow the conventions of the code around the change. Reuse existing helpers.
- C++17, no new dependencies without discussion. The only external library is
  nlohmann/json, fetched by CMake when the system lacks it.
- Keep the test suite green. Build and run everything before claiming done:

  ```sh
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
  cmake --build build -j
  ctest --test-dir build --output-on-failure
  ```

## Architecture notes that are easy to get wrong

- A live Swapdex service is a held lock (`service.lock` in the state directory),
  not a loaded service-manager job. `launchctl print` reports loaded state, not
  running state. Always judge liveness by the lock.
- The service does not exit when the managed Codex app quits. It idles and
  adopts a Codex the user opens by hand: it closes that instance and relaunches
  it under control. Keep that behavior intact.
- CDP event handlers run on the pipe's reader thread, where a request can never
  be answered because only that thread reads responses. Never issue a CDP
  request inside an event handler; queue the work for the UI thread.
- On macOS the app is `/Applications/ChatGPT.app`, its user data is
  `~/Library/Application Support/Codex`, and Swapdex state lives in
  `~/Library/Application Support/swapdex` including `service.log`.
- `launchctl load -w` already starts the job because RunAtLoad is set. Do not
  also `kickstart` it in the same call; that restarts a service mid launch.
- The interface is `assets/inject.js`, injected at runtime. Codex markup changes
  break it, so selectors need fallbacks for layout changes, not single-shape
  queries.

## Git and pull requests

- One logical change per pull request, on a `devin/<timestamp>-<slug>` or
  descriptive branch. Never push to `main` directly.
- Commit messages: imperative subject line, then a wrapped body explaining why,
  not restating the diff.
- PR bodies: a `## Summary` section that orients a reader who has not seen the
  diff. Lead with what was broken and what now happens instead.
- Run the full test suite before opening the PR and fix failures on the branch.
- Do not merge your own pull requests. The user merges.

## Releases

- Releases are built by `.github/workflows/release.yml` when a `v*` tag is
  pushed. Tag with `git tag v<version>` on `main` after merging, never on a
  branch.
- The version lives in two places: `project(... VERSION ...)` in CMakeLists.txt
  and the Status section of README.md. Keep them in sync.

## Private notes

Anything meant to stay local goes in a `*.local.md` file or under `notes/`.
Both are gitignored. Nothing private should ever be committed to this public
repository.
