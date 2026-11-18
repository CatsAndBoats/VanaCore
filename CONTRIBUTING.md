# Contributing to VanaCore

Thanks for helping. Bug reports, fixes and new features are all welcome, and so is code written with AI tools. What
matters is that a pull request is understood, tested and clearly explained by the person sending it.

## Before you start

- **Something big?** Open an issue first and say what you'd like to do. A new feature, a new setting or a change to how
  something works is easier to agree on before the code is written.
- **One change per pull request.** A fix and an unrelated tidy-up are two pull requests.
- **Keep to what the change needs.** Don't reformat, rename or reorganise code you aren't changing.

## AI-assisted contributions

AI tools (Claude, Copilot, ChatGPT, Cursor and the like) are fine to use. You are still the author, so:

1. **Understand every line you send.** You should be able to answer a reviewer's question about any part of it
   without asking the AI. A pull request whose author can't explain it will be closed.
2. **Test it in the game yourself.** Build it, run it, and check the change does what it says. Say how you tested it
   in the pull request. "The AI says it works" is not testing.
3. **Write the pull request yourself, or check it line by line.** An AI-written description often claims things the
   code doesn't do, or lists files and checks that never happened. Everything in yours must be true.
4. **Watch for what AI tends to add:** code nobody calls, duplicates of functions VanaCore already has, made-up APIs,
   needless comments, wide rewrites for a small fix, and changed tests that should have stayed the same. Remove it
   before sending.
5. **Don't commit the AI's own files**, such as `CLAUDE.md`, `AGENTS.md`, `.claude/`, `.cursor/`, chat logs or
   scratch scripts.

## Pull requests

### The title

Short: a few words naming the change, for example `Party list shows the new zone at once`. The cause, the details and
the testing go in the description, not the title.

### The description

The [template](.github/pull_request_template.md) fills in when you open a pull request. Answer every part:

- **What changed:** what a player or addon maker will notice, in plain words.
- **Why:** the bug or the need. Link the issue if there is one.
- **How:** the approach, and the files that matter, for a reviewer to start from.
- **How it was tested:** what you ran and what you saw, with the renderer (Direct3D 12 or 11), the GPU, and Windows,
  Wine or Proton. Screenshots or a clip for anything you can see in game.

### Commits

- Write the message as a sentence saying what changed and why: `Party list: show the new zone as soon as the zone
  loads, not after the next party update`, not `fix bug` or `update files`.
- Squash "fix typo" and "try again" commits before asking for a review.
- Don't change the version number (`src/shared/vanacore/version.hpp`): it's set when a release is made.

## Code

- **Build it:** Release | x86 must build with no new warnings. See [Building](README.md#building).
- **Lint it:** for any change to `src/`, run `lint.bat lizard filelen` (seconds), and the whole `lint.bat` for a bigger
  change. It fails for a function or file grown past its allowance in `tools/lint/baseline.json`: split the function
  or move code into its own file rather than raising the baseline.
- **Match what's there.** Follow the style of the file you're in.
- **Document what you add.** A new setting or feature goes in [docs/reference.md](docs/reference.md), with its
  default in `vanacore.example.ini`. A new addon API goes in [docs/addons/](docs/addons/README.md).
- **Ashita and Windower addons are the spec.** If one misbehaves under VanaCore, fix VanaCore's compatibility layer
  (`src/core/compat/`), never the addon.
- **Leave `third_party/` as it is**, unless the change is updating that library.
- **No secrets or personal files:** no passwords, API keys, `vanacore.ini` or account `vanacore.<hash>.ini` files,
  logs or crash dumps.

## Bug reports

Say what happened, what you expected, and how to make it happen again. Add the VanaCore version (the /vana window
shows it), the renderer, your GPU, and Windows, Wine or Proton. Attach the log from `logs\`, and the crash dump for a
crash.

## Licence

VanaCore is under the [GPL v3 or later](LICENSE), with the additional permissions in [LICENSING.md](LICENSING.md). By
sending a pull request you agree that your contribution is under the same terms, and that you have the right to give
it: code copied from somewhere else must have a licence that allows it, and must say where it came from.
