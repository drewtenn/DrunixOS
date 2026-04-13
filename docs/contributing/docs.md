# Documentation Workflow

Whenever a source file in `boot/` or `kernel/` is created or modified, update the corresponding chapter in `docs/` to reflect the change.

All new or revised prose must follow `docs/style.md`.

## New Features

If a new source file introduces a feature that does not fit an existing chapter:

1. Assign the next sequential chapter number after the current highest chapter in `docs/`. Use that number both as the filename prefix and as the `## Chapter N` heading inside the file.
2. Create `docs/chNN-<topic>.md` starting with `\newpage` on the first line, followed by a blank line, so the chapter starts on a fresh page in the PDF. Follow `docs/style.md`: narrate the runtime flow, introduce jargon on first use, prefer prose over code blocks except for C struct definitions and short data tables, and end with a "Where the Machine Is by the End of Chapter N" section.
3. Add the new chapter to `DOCS_SRC` in `Makefile` at the point that matches its pedagogical dependency order. A chapter must appear after all chapters whose concepts it relies on and before any chapters that depend on it. Chapter headings are `##`; sections within a chapter are `###`.
4. Update any relevant docs index or chapter list that still exists in the repo.
