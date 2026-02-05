# Managing `user_keywords.db`

This README explains how `snippet_gen.cpp` persists, loads and manages user-defined keywords stored in `user_keywords.db` (a plain text file). The program stores custom keywords as named snippets with optional parameters; use the program's interactive commands to add/update/delete entries, or edit the file by hand if you understand the format.

---

## File location

- Default filename: `user_keywords.db` (relative to the program's current working directory).
- The executable loads this file at start and writes it when you add/update/delete keywords via the program.

## On-disk format

Each user keyword entry uses the following block format:

```
===KEYWORD:<name>===
===PARAMS:name=default,other=val===    # optional
<snippet lines...>
===END===
```

Notes:
- `<name>` should be a single token consisting of letters/digits/underscores (the program normalizes tokens by trimming surrounding punctuation and lowercasing). Use the interactive `:add` command to avoid token mistakes.
- `===PARAMS:...===` is optional. Parameters are comma-separated `name=default` pairs. If `=default` is omitted the default is empty.
- `<snippet lines...>` is the raw multi-line snippet. The program will extract `#include` lines and place them before `main()` when the snippet is used.
- A snippet **must not** contain `int main(`. The program enforces this.

## Parameter placeholders

Inside snippets, reference parameters using `{param_name}`. When a snippet is expanded the program substitutes `{param_name}` with the supplied value or the default from `===PARAMS...===`.

## Recommended workflow (use the program)

Run the program and use the interactive commands (entered at the prompt):

- `:add` or `:define` — define a new custom keyword. The program will prompt for the keyword name, parameters (optional), and the snippet body (enter `.` alone on a line to finish the snippet).
- `:list` — list stored custom keywords.
- `:search <term>` — search names and snippet text for `<term>`.
- `:update <keyword>` — interactively update parameters and/or replace the snippet for `<keyword>`.
- `:delete <keyword>` — delete the stored custom keyword.
- `:help` — show help and the available commands.

Using the program ensures the file stays well-formed and that the keywords are validated (e.g. not colliding with built-in C++ keywords).

## Manual editing (advanced)

If you prefer to edit `user_keywords.db` directly:

1. Make a backup copy before editing.
2. Follow the block format exactly; every `===KEYWORD:...===` block must end with `===END===`.
3. Do not insert `int main(` inside snippets.
4. Use simple ASCII characters for the delimiters `===KEYWORD:`, `===PARAMS:`, `===END===`.
5. After manual edits, run the program. It will read the file at startup; if the file is malformed some entries may be ignored.

## Example

Example entry with two parameters:

```
===KEYWORD:swap===
===PARAMS:var1=x,var2=y===
#include <iostream>
std::swap({var1},{var2});
std::cout << "{var1} = " << {var1} << ", {var2} = " << {var2} << std::endl;
===END===
```

When expanded the placeholders `{var1}` and `{var2}` are replaced with supplied or default values.

## Safety and backup

- Always back up `user_keywords.db` before manual edits or bulk changes.
- Prefer the interactive commands (`:add`, `:update`, `:delete`) to avoid formatting errors.

---

If you want, I can also produce a small example `user_keywords.db` with a couple of sample entries.

