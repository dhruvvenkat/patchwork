# PRD: AI-Native Terminal Code Editor

## 1. Product Summary

### Product Name

Working name: **flowstate**

### One-liner

A terminal-first code editor where AI suggestions are delivered as reviewable patches, keeping the developer in control.

### Product Vision

Most AI coding tools either live inside large GUI editors or behave like chatbots bolted onto the side of an existing workflow. flowstate is a lightweight terminal editor built around a stricter workflow: **select code, ask for help, review a diff, accept or reject changes**.

The goal is not to build another Vim clone or another tmux-like terminal environment. The goal is to build a small, focused editor for the AI era where code editing, compiler output, project context, and AI-assisted reasoning all happen in one controlled loop.

### Core Philosophy

AI should never silently mutate source code. It should propose changes, explain them, and let the user accept or reject them.

flowstate should feel like this:

> Old-school terminal editor discipline + modern AI-assisted development.

---

## 2. Problem Statement

Developers increasingly use AI to explain bugs, generate tests, refactor code, and understand unfamiliar codebases. However, the common workflow is clunky:

1. Copy code from terminal/editor.
2. Paste into an AI chat.
3. Ask a question.
4. Copy the answer back.
5. Manually apply edits.
6. Hope the change did not break anything.

This flow has several issues:

* It is slow and context-switch heavy.
* The AI often lacks useful local context.
* Suggestions are not naturally represented as patches.
* There is no clean accept/reject workflow.
* The user may accidentally overwrite good code with bad AI output.
* Learning is separated from editing.

Existing full IDEs solve some of this, but they are heavyweight. Terminal-first developers need a focused, hackable tool that integrates AI without giving up control.

---

## 3. Target Users

### Primary User

A developer or student working in the terminal who wants AI assistance without leaving their coding environment.

This user likely:

* Works on C, C++, Rust, Python, or systems-style projects.
* Uses Linux/macOS terminal frequently.
* Wants lightweight tooling.
* Values understanding code, not blindly accepting AI output.
* Is comfortable with CLI workflows.
* Wants a project that demonstrates real systems/programming skill.

### Secondary Users

* Students debugging assignments.
* Developers onboarding into unfamiliar codebases.
* Terminal users who dislike heavy IDEs.
* People learning low-level programming who want explanations tied directly to code.

---

## 4. Goals

### Product Goals

* Build a usable terminal code editor with basic editing functionality.
* Add AI assistance as a first-class workflow, not a sidebar gimmick.
* Make AI edits reviewable through unified diffs.
* Support accept/reject hunk-level patch application.
* Provide compiler/build error explanations.
* Keep the project small enough to realistically build as a serious portfolio project.

### Technical Goals

* Implement raw terminal input handling.
* Implement efficient text buffer operations.
* Implement rendering with ANSI escape codes.
* Support file open/save.
* Support text selection.
* Support AI requests using selected code and local context.
* Parse and display unified diffs.
* Apply accepted hunks safely to the in-memory buffer.
* Keep a modular architecture that can grow into plugins, syntax highlighting, project indexing, or LSP later.

### Learning Goals

This project should teach:

* Terminal raw mode.
* Keyboard input handling.
* Buffer data structures.
* Incremental rendering.
* File I/O.
* Process execution.
* Diff parsing.
* API integration.
* UX design for developer tools.

---

## 5. Non-Goals

flowstate should not initially try to be:

* A full Vim replacement.
* A full VS Code replacement.
* A tmux replacement.
* A complete IDE.
* A full LSP client.
* A multiplayer collaborative editor.
* A plugin marketplace.
* A graphical editor.
* A perfect AI agent that edits entire codebases autonomously.

The MVP should avoid feature creep. The product lives or dies on one loop:

> Ask AI about code → receive diff/explanation → review → accept/reject.

---

## 6. Core User Stories

### Basic Editing

As a user, I want to open a file so that I can edit source code.

As a user, I want to move the cursor with arrow keys so that I can navigate naturally.

As a user, I want to insert and delete text so that I can modify code.

As a user, I want to save the file with a familiar shortcut so that I do not lose work.

As a user, I want to quit safely and be warned about unsaved changes.

### Selection-Aware AI

As a user, I want to select a block of code and ask AI to explain it.

As a user, I want to select a function and ask AI to find bugs.

As a user, I want the AI to know the file name, language, and nearby lines so that its response is relevant.

### Patch-Based AI Editing

As a user, I want to ask AI to modify selected code.

As a user, I want AI changes to appear as a diff instead of directly changing my file.

As a user, I want to accept or reject each diff hunk.

As a user, I want to preview the final result before saving.

### Compiler Error Assistance

As a user, I want to run a build command from inside the editor.

As a user, I want compiler errors to appear in a build output buffer.

As a user, I want to ask AI to explain a compiler error.

As a user, I want the editor to jump to the relevant file and line when possible.

### Learning Mode

As a user, I want AI explanations to include the underlying programming concept, not just the answer.

As a user, I want to understand why a fix works so that I become a better programmer.

---

## 7. MVP Scope

The MVP should prove the product idea without becoming a giant IDE.

### MVP Features

#### 1. Basic Terminal Editor

* Launch from terminal with a file path.
* Open existing text files.
* Create new files.
* Render file contents.
* Cursor movement.
* Insert characters.
* Delete/backspace.
* Newline insertion.
* Vertical scrolling.
* Save file.
* Quit file.
* Unsaved changes warning.

#### 2. Status Bar

Display:

* File name.
* Modified indicator.
* Current line and column.
* File type/language guess.
* Mode/status message.

Example:

```text
main.cpp | C++ | modified | Ln 42, Col 7
```

#### 3. Text Selection

Support keyboard-based selection using Shift + Arrow or a custom shortcut.

Minimum viable behavior:

* Start selection.
* Extend selection.
* Clear selection.
* Extract selected text.

Mouse selection can be deferred.

#### 4. AI Explain Command

Command:

```text
:ai explain
```

Behavior:

* Uses selected text if available.
* Otherwise uses current line or current function approximation.
* Sends prompt to AI provider.
* Displays response in an AI scratch buffer.

#### 5. AI Fix/Refactor Command With Diff Output

Commands:

```text
:ai fix
:ai refactor
```

Behavior:

* Sends selected code plus context.
* Requests a unified diff.
* Shows the diff in a patch preview buffer.
* Does not mutate the file automatically.

#### 6. Diff Preview

Display AI-proposed changes in a readable format.

Example:

```diff
- int totalBefore;
+ long long totalBefore = 0;
```

Must show:

* Added lines.
* Removed lines.
* Context lines.
* Hunk boundaries.

#### 7. Accept/Reject Patch

Support:

* Accept current hunk.
* Reject current hunk.
* Accept all hunks.
* Reject all hunks.

Accepted hunks are applied to the in-memory buffer.

#### 8. Build Command Integration

Command:

```text
:build
```

Behavior:

* Runs a configured build command.
* Captures stdout/stderr.
* Shows output in build buffer.

Initial build command can be passed through config or CLI flag.

Example:

```text
flowstate main.cpp --build "g++ main.cpp -o main"
```

#### 9. AI Explain Build Error

Command:

```text
:ai error
```

Behavior:

* Uses latest build output.
* Includes current file context.
* Asks AI to explain the error and suggest a minimal fix.
* Displays explanation in AI scratch buffer.
* Optionally requests patch output.

---

## 8. Post-MVP Features

### High-Priority Post-MVP

* Syntax highlighting for C/C++.
* Search within file.
* Undo/redo.
* Config file.
* Fuzzy file finder.
* Multiple buffers.
* Project-wide search.
* Git diff integration.
* AI-generated commit messages.
* AI code review on Git diff.

### Medium-Priority Post-MVP

* Project context indexing.
* Function/symbol detection.
* Test generation.
* Crash recovery.
* Themes.
* Split panes.
* Mouse support.
* Markdown rendering/preview.

### Long-Term / Advanced

* Tree-sitter support.
* LSP support.
* Plugin system.
* Lua scripting.
* Local embedding-based project search.
* Multi-file AI patches.
* Debugger integration.
* Collaborative editing.

---

## 9. Key Workflows

### Workflow 1: Explain Selected Code

1. User opens `main.cpp`.
2. User selects a function.
3. User runs `:ai explain`.
4. Editor sends selected function, file path, language, and surrounding context.
5. AI response appears in scratch buffer.
6. User returns to code.

Success criteria:

* User does not need to copy/paste code manually.
* Response refers directly to the selected code.
* Original file remains unchanged.

### Workflow 2: Fix Bug With AI Patch

1. User selects buggy code.
2. User runs `:ai fix`.
3. Editor sends selected code and nearby context.
4. AI returns a unified diff.
5. Editor displays the diff.
6. User accepts/rejects hunks.
7. Accepted changes update the buffer.
8. User saves file.

Success criteria:

* AI does not directly edit file without approval.
* User can inspect exact changes.
* Patch application does not corrupt the buffer.

### Workflow 3: Explain Compiler Error

1. User runs `:build`.
2. Build fails.
3. Output appears in build buffer.
4. User runs `:ai error`.
5. AI explains likely cause.
6. AI optionally proposes a minimal diff.
7. User reviews and accepts/rejects.

Success criteria:

* Build output is captured correctly.
* Error context is sent to AI.
* Suggested fixes are patch-based.

### Workflow 4: AI Code Review

Post-MVP workflow:

1. User edits code.
2. User runs `:ai review`.
3. Editor gets current Git diff.
4. AI reviews only changed code.
5. Editor shows issues grouped by severity.

Success criteria:

* AI does not waste time reviewing unchanged files.
* Feedback is tied to specific line ranges.

---

## 10. UX Requirements

### General UX Principles

* Default to safety.
* Never apply AI edits silently.
* Keep keyboard flow fast.
* Make every AI action inspectable.
* Prefer small targeted context over huge prompts.
* Avoid hiding file mutations.
* Make failure states obvious.

### Modes/Buffers

flowstate should support several buffer types:

1. **File Buffer**

   * Editable source file.

2. **AI Scratch Buffer**

   * Readable AI explanation output.
   * May be copyable/editable later.

3. **Patch Preview Buffer**

   * Shows proposed diff.
   * Supports accept/reject commands.

4. **Build Output Buffer**

   * Shows command output.
   * Read-only initially.

### Suggested Commands

```text
:open <file>
:write
:quit
:ai explain
:ai fix
:ai refactor
:ai error
:build
:patch accept
:patch reject
:patch accept-all
:patch reject-all
```

### Suggested Shortcuts

```text
Ctrl+S        Save
Ctrl+Q        Quit
Ctrl+F        Search, post-MVP
Ctrl+E        AI explain selection
Ctrl+R        AI refactor/fix selection
Ctrl+B        Build
Alt+A         Accept current patch hunk
Alt+R         Reject current patch hunk
Esc           Cancel selection / close panel
```

Terminal caveat: Ctrl+C and Ctrl+V may conflict with terminal behavior. Prefer Ctrl+Shift+C/Ctrl+Shift+V through the terminal emulator for clipboard, or implement explicit editor commands.

---

## 11. AI Behavior Requirements

### AI Input Context

When making an AI request, include:

* User instruction.
* Selected code.
* File path.
* Detected language.
* Current cursor line/column.
* Nearby context lines.
* Build output, if relevant.
* Project metadata, if available.

### AI Output Types

The AI should return one of two main response types:

#### Explanation Response

Plain text or Markdown explanation.

Should include:

* What the code does.
* Potential bug or issue.
* Why it matters.
* Suggested next step.

#### Patch Response

Unified diff only, optionally followed by a short explanation.

Preferred format:

```diff
--- a/main.cpp
+++ b/main.cpp
@@ -10,7 +10,7 @@
- int totalBefore;
+ long long totalBefore = 0;
```

The editor should attempt to parse only the diff portion.

### AI Safety Rules

* AI must not automatically save files.
* AI must not automatically run destructive shell commands.
* AI patches must be previewed.
* User must explicitly accept changes.
* The editor should reject malformed patches or show them as text only.

---

## 12. Technical Architecture

### Recommended Language

C++ is the recommended implementation language for the project because it aligns with systems programming goals and exposes useful low-level details.

### Major Modules

```text
src/
  main.cpp
  terminal.cpp        raw mode, key input, terminal resize
  screen.cpp          rendering, status bar, panels
  buffer.cpp          text storage and editing operations
  cursor.cpp          cursor movement and viewport logic
  selection.cpp       selection state and selected text extraction
  command.cpp         command parsing and dispatch
  ai_client.cpp       API calls to AI provider
  prompt.cpp          prompt construction
  diff.cpp            unified diff parser
  patch.cpp           hunk application/rejection
  build.cpp           shell command execution and output capture
  config.cpp          config loading, post-MVP
  syntax.cpp          syntax highlighting, post-MVP
  git.cpp             git integration, post-MVP
```

### Core Data Structures

#### Text Buffer

MVP can use a simple line vector:

```cpp
std::vector<std::string> lines;
```

This is simple and good enough for small/medium files.

Later upgrades:

* Gap buffer.
* Piece table.
* Rope.

#### Cursor

```cpp
struct Cursor {
    size_t row;
    size_t col;
};
```

#### Selection

```cpp
struct Selection {
    bool active;
    Cursor start;
    Cursor end;
};
```

#### Patch/Hunk

```cpp
struct DiffLine {
    enum class Type { Context, Add, Remove };
    Type type;
    std::string text;
};

struct Hunk {
    size_t old_start;
    size_t old_count;
    size_t new_start;
    size_t new_count;
    std::vector<DiffLine> lines;
    bool accepted;
};
```

---

## 13. Patch Application Requirements

Patch application is the most important technical feature.

### Requirements

* Parse unified diff hunks.
* Validate hunk line numbers where possible.
* Match context lines before applying.
* Reject hunks that do not match current buffer state.
* Apply accepted hunks in a safe order.
* Preserve undo history once undo is implemented.

### Hunk Application Strategy

For MVP:

1. Parse diff.
2. For each hunk, locate target line.
3. Verify context and removed lines match buffer.
4. Remove deleted lines.
5. Insert added lines.
6. Mark hunk as applied.

Apply hunks from bottom to top to avoid line number shifts.

### Failure Cases

If patch cannot apply:

* Show message: `Patch hunk does not match current buffer.`
* Keep hunk visible.
* Allow user to manually copy/edit.
* Do not partially corrupt buffer.

---

## 14. Build System Integration

### MVP Behavior

The user can define a build command.

Examples:

```bash
flowstate main.cpp --build "g++ main.cpp -o main"
```

or later in config:

```toml
build = "g++ main.cpp -o main"
run = "./main"
```

### Build Output Capture

* Run command as child process.
* Capture stdout.
* Capture stderr.
* Store output in build buffer.
* Preserve exit code.

### Error Parsing

Post-MVP:

Parse common compiler error formats:

```text
main.cpp:42:13: error: ...
```

Use parsed location to jump to relevant line.

---

## 15. Configuration

MVP can avoid config initially, but post-MVP should support a simple config file.

Example path:

```text
~/.config/flowstate/config.toml
```

Example config:

```toml
tab_size = 4
line_numbers = true
theme = "default"
ai_provider = "openai"
model = "gpt-4.1-mini"
build = "g++ main.cpp -o main"
```

Potential configurable items:

* Keybindings.
* Build command.
* AI provider.
* Model.
* Theme.
* Tab size.
* Line numbers.
* Prompt templates.

---

## 16. Milestones

### Milestone 1: Raw Terminal Editor Skeleton

Deliverables:

* Enter raw terminal mode.
* Read keypresses.
* Render screen.
* Open file.
* Move cursor.
* Insert/delete characters.
* Save file.
* Quit safely.

Definition of done:

* User can edit and save a normal text file from terminal.

### Milestone 2: Editor Usability

Deliverables:

* Status bar.
* Scrolling.
* Unsaved changes warning.
* Basic command mode.
* Line/column display.

Definition of done:

* Editor feels usable for small files.

### Milestone 3: Selection + AI Explain

Deliverables:

* Text selection.
* Extract selected text.
* AI client.
* Prompt construction.
* AI scratch buffer.
* `:ai explain` command.

Definition of done:

* User can select code and receive an explanation inside the editor.

### Milestone 4: AI Diff Generation

Deliverables:

* `:ai fix` command.
* Prompt asks for unified diff.
* Diff response displayed in patch preview buffer.
* Basic diff parser.

Definition of done:

* AI can propose changes as a diff without modifying the file.

### Milestone 5: Patch Accept/Reject

Deliverables:

* Parse hunks.
* Accept/reject current hunk.
* Accept/reject all hunks.
* Apply accepted hunks to buffer.
* Detect failed hunk application.

Definition of done:

* User can safely apply AI-proposed changes.

### Milestone 6: Build/Error AI Loop

Deliverables:

* Run build command.
* Capture output.
* Build output buffer.
* `:ai error` command.
* AI explains latest build failure.

Definition of done:

* User can build, see failure, and ask AI for explanation without leaving editor.

---

## 17. Success Metrics

Because this is likely a portfolio project, success is not measured by revenue or active users at first. It is measured by technical completeness and demo quality.

### MVP Success Criteria

* Can edit a file end-to-end.
* Can select code and ask AI to explain it.
* Can ask AI for a fix and receive a diff.
* Can preview the diff.
* Can accept/reject hunks.
* Can run a build command and ask AI about errors.

### Portfolio Demo Success Criteria

A demo video should be able to show:

1. Opening a C++ file.
2. Selecting buggy code.
3. Asking AI to fix it.
4. Reviewing the diff.
5. Accepting one hunk and rejecting another.
6. Running build.
7. Asking AI to explain any compiler error.
8. Saving final code.

If that demo works, the project is genuinely impressive.

---

## 18. Risks and Mitigations

### Risk: Editor Scope Gets Too Big

Mitigation:

* Do not build splits, tabs, plugins, or LSP before the AI patch workflow works.

### Risk: AI Returns Bad or Malformed Diffs

Mitigation:

* Validate patches.
* Show malformed output as plain text.
* Never apply automatically.

### Risk: Patch Application Corrupts Buffer

Mitigation:

* Verify context lines before applying.
* Apply hunks bottom-up.
* Keep original buffer snapshot before patch.

### Risk: Terminal Input Is Painful

Mitigation:

* Start with minimal key handling.
* Add shortcuts gradually.
* Avoid mouse support in MVP.

### Risk: API Integration Distracts From Editor Core

Mitigation:

* Build AI client behind an interface.
* Allow a mock AI provider that reads canned responses from files.

### Risk: Project Becomes Just Another Editor

Mitigation:

* Prioritize diff-based AI workflow above traditional editor bells and whistles.

---

## 19. Suggested MVP Tech Stack

### Language

* C++17 or C++20.

### Terminal Control

* `termios` for raw mode.
* ANSI escape sequences for rendering.
* `read()` and `write()` for input/output.

### HTTP/API

Options:

* Use `libcurl` from C++.
* Or spawn a helper script for API calls initially.
* Or abstract AI client and mock it first.

### JSON

Options:

* `nlohmann/json`.
* Or minimal manual string construction for early prototype.

### Build

* CMake.

### Testing

* Unit tests for buffer operations.
* Unit tests for diff parsing.
* Unit tests for patch application.

---

## 20. Testing Plan

### Unit Tests

Test:

* Cursor movement.
* Insert/delete behavior.
* Newline splitting.
* Line joining.
* Selection extraction.
* Diff parser.
* Patch application.
* Failed patch detection.
* Build command output capture.

### Manual Tests

Scenarios:

* Open empty file.
* Open large-ish file.
* Edit and save.
* Quit with unsaved changes.
* Select multi-line code.
* Ask AI to explain selection.
* Apply simple one-hunk patch.
* Apply multi-hunk patch.
* Reject hunk.
* Handle malformed diff.
* Run failing build command.

### Mock AI Tests

Use fixture files:

```text
tests/fixtures/ai_explain_response.txt
tests/fixtures/simple_patch.diff
tests/fixtures/malformed_patch.txt
```

This prevents development from depending on live API calls.

---

## 21. Example Prompt Templates

### Explain Prompt

````text
You are helping explain code inside a terminal editor.

File: {{file_path}}
Language: {{language}}
Cursor: line {{line}}, column {{column}}

Selected code:
```{{language}}
{{selected_code}}
````

Explain what this code does, identify any likely issues, and explain the underlying programming concepts clearly.

````

### Fix Prompt
```text
You are helping modify code inside a terminal editor.

Return your proposed change as a unified diff only. Do not rewrite the entire file unless necessary. Keep the patch minimal.

File: {{file_path}}
Language: {{language}}

Context before selection:
```{{language}}
{{context_before}}
````

Selected code:

```{{language}}
{{selected_code}}
```

Context after selection:

```{{language}}
{{context_after}}
```

User request:
{{user_instruction}}

````

### Compiler Error Prompt
```text
You are helping debug a compiler/build error inside a terminal editor.

File: {{file_path}}
Language: {{language}}

Build command:
{{build_command}}

Build output:
```text
{{build_output}}
````

Relevant code:

```{{language}}
{{relevant_code}}
```

Explain the error, identify the likely cause, and suggest a minimal fix. If a code change is needed, provide it as a unified diff.

````

---

## 22. Recommended Build Order

Do not build this in the order that feels coolest. Build it in the order that reduces risk.

Recommended order:

1. Raw terminal mode.
2. File rendering.
3. Cursor movement.
4. Text insertion/deletion.
5. Save/quit.
6. Status bar.
7. Command mode.
8. Selection.
9. AI scratch buffer with mock response.
10. Real AI client.
11. Diff preview with mock diff.
12. Diff parser.
13. Patch application.
14. Build command.
15. AI error explanation.

This avoids getting stuck on AI or fancy UI before the editor actually edits.

---

## 23. Demo Script

A strong demo could use a buggy C++ file:

```cpp
float calculateCpuUsage(const std::vector<long long>& before,
                        const std::vector<long long>& after) {
    long long idleBefore = before[3] + before[4];
    long long idleAfter = after[3] + after[4];

    long long totalBefore;
    long long totalAfter;

    for (int i = 0; i < before.size(); i++) totalBefore += before[i];
    for (int i = 0; i < after.size(); i++) totalAfter += after[i];

    return 100.0 * (1.0 - (idleAfter - idleBefore) / (totalAfter - totalBefore));
}
````

Demo flow:

1. Open file in flowstate.
2. Select function.
3. Run `:ai explain`.
4. AI points out uninitialized totals and integer division.
5. Run `:ai fix`.
6. AI returns diff initializing totals and casting to double.
7. Accept patch.
8. Run `:build`.
9. Build succeeds.

This demo is compact, relatable, and shows the product clearly.

---

## 24. Final MVP Definition

The MVP is complete when flowstate can do this reliably:

> Open a source file, allow basic edits, let the user select code, ask AI for an explanation or fix, display AI fixes as diffs, and let the user accept or reject those diffs before saving.

Everything else is optional until that loop works.

The project should be judged by how well it protects the user from bad AI edits while making good AI assistance fast and natural.
