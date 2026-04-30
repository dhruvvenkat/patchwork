# Patchwork

Patchwork is a terminal-first code editor with AI-assisted explain, fix, refactor, and build-error workflows. AI suggestions are shown in a scratch buffer first, and code changes move into a reviewable patch preview before you apply them.

## Build

```bash
cmake -S . -B build
cmake --build build
```

Run the test suite:

```bash
ctest --test-dir build --output-on-failure
```

## Run

Basic usage:

```bash
./build/patchwork <file>
```

Choose an AI provider:

```bash
./build/patchwork <file> --ai mock
./build/patchwork <file> --ai openai
./build/patchwork <file> --ai codex
```

Attach a build command so `Ctrl+T` and `:build` work:

```bash
./build/patchwork src/main.cpp --build "cmake --build build"
```

You can also set the default provider with:

```bash
export PATCHWORK_AI_MODE=codex
```

## Core Workflow

1. Open a file.
2. Move the cursor with the arrow keys, `Home`, `End`, `PageUp`, and `PageDown`.
3. Press `Ctrl+G` to start or clear a selection.
4. Move the cursor to expand the selection.
5. Press `Ctrl+E` to explain the current line or selection.
6. Press `Ctrl+R` to ask for a fix patch.
7. Review the AI scratch buffer or patch preview.

## Important Shortcuts

| Shortcut | Action |
| --- | --- |
| `Ctrl+S` | Save the current file |
| `Ctrl+Q` | Quit. If the file is dirty, press it twice |
| `Ctrl+G` | Toggle selection on or off |
| `Ctrl+C` | Copy the current selection, or the current line if nothing is selected |
| `Ctrl+X` | Cut the current selection, or the current line if nothing is selected |
| `Ctrl+V` | Paste the internal editor clipboard at the cursor, or replace the current selection |
| `Ctrl+Z` | Undo the last file change |
| `Ctrl+Y` | Redo the last undone file change |
| `Ctrl+F` | Open the find prompt for the current file |
| `Ctrl+E` | Explain the current line or selection |
| `Ctrl+R` | Generate a fix patch |
| `Ctrl+T` | Run the configured build command |
| `Esc` | Leave AI scratch, patch preview, or build output and return to the file buffer |
| `Alt+E` | Reopen the AI scratch buffer |
| `Alt+P` | Reopen patch preview if a patch is ready |
| `Alt+A` | Accept the current hunk in patch preview |
| `Alt+R` | Reject the current hunk in patch preview |
| `:` | Enter command mode |

Notes:

- Only one AI request runs at a time.
- Pressing `Esc` while AI is still running sends you back to the file buffer and leaves the request running in the background.
- Cut, copy, and paste currently use an internal Patchwork clipboard rather than the system clipboard.

## Command Mode

Press `:` to open command mode, type a command, then press `Enter`.

| Command | Action |
| --- | --- |
| `:open <file>` | Open another file. If it does not exist, Patchwork starts with an empty buffer for that path |
| `:write` | Save the current file |
| `:quit` | Quit. If the file is dirty, run it twice |
| `:build` | Run the configured build command |
| `:find <text>` | Find text in the current file, wrapping from the cursor |
| `:goto <line>` | Jump to a 1-based line number |
| `:ai explain` | Explain the current line or selection |
| `:ai fix` | Generate a fix patch |
| `:ai refactor` | Generate a refactor patch |
| `:ai error` | Explain the latest build failure |
| `:patch accept` | Accept the current patch hunk |
| `:patch reject` | Reject the current patch hunk |
| `:patch accept-all` | Accept all pending hunks |
| `:patch reject-all` | Reject all pending hunks |

Press `Esc` in command mode to cancel it.

## AI Views

### AI Scratch

The AI scratch buffer is where streamed AI output appears first.

- While AI is thinking, the top line shows the request label and a rotating spinner.
- For explain and error requests, the final text stays in the scratch buffer.
- For fix and refactor requests, Patchwork attempts to extract a unified diff when the response completes.

### Patch Preview

If AI returns a valid patch for the current file, Patchwork switches to patch preview.

- Use `Alt+A` and `Alt+R` to accept or reject the current hunk.
- Use `:patch accept-all` or `:patch reject-all` for bulk actions.
- After applying or rejecting hunks, the preview refreshes automatically.

### Build Output

Build output opens in its own buffer after `Ctrl+T` or `:build`.

- If no build command was configured, the build output explains that directly.
- `:ai error` uses the latest captured build output.

## AI Provider Setup

### Mock

`mock` is the default provider. It uses fixture responses from `tests/fixtures` and is useful for local testing.

### OpenAI

Required:

```bash
export PATCHWORK_OPENAI_API_KEY=...
```

Optional:

```bash
export PATCHWORK_OPENAI_MODEL=gpt-4.1-mini
export PATCHWORK_OPENAI_BASE_URL=https://api.openai.com/v1/responses
```

Run:

```bash
./build/patchwork <file> --ai openai
```

### Codex

Patchwork’s Codex mode uses a persistent local `codex app-server` session.

Requirements:

- `codex` must be installed and available on `PATH`
- your Codex CLI must already be authenticated

Optional:

```bash
export PATCHWORK_CODEX_MODEL=...
export PATCHWORK_CODEX_PROFILE=...
```

Run:

```bash
./build/patchwork <file> --ai codex
```

## Status Bar

The status bar shows:

- current buffer name
- detected language
- save state
- active AI provider
- active view
- cursor line and column
- AI request state such as `CONNECTING`, `STREAMING`, `PARSING PATCH`, `FAILED`, or `COMPLETE`

## Tips

- Use `Ctrl+G`, move the cursor, then `Ctrl+E` when you want AI to focus on a specific region.
- Use `Ctrl+T` before `:ai error` so Patchwork has fresh build output to analyze.
- If a patch is ready but you are back in the file buffer, press `Alt+P`.
- If an AI request finishes while you are back in the file buffer, press `Alt+E` to reopen the scratch buffer.
