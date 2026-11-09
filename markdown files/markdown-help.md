# MegaMarkdown Help

This viewer renders a practical subset of Markdown at build time. The result is plain text wrapped to 18 columns for the radio's small font.

## Supported syntax

- `#` headings become uppercase text.
- `-` and `1.` lists keep their marker.
- `>` quotes start with a `>` marker.
- `**bold**`, `*italic*` and `` `code` `` markers are removed.
- `[links](target)` keep only the visible text.
- Code fences are shown as plain lines.

## Example

```c
int main(void) {
    return 0;
}
```

A horizontal rule becomes a line of dashes:

---

Tables keep their cells, separated by a pipe:

| a | b | c |
| --- | --- | --- |
| 1 | 2 | 3 |
| 4 | 5 | 6 |
