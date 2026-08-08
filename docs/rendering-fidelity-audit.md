# Rendering fidelity audit

**Question.** Does `termrenderer` ever replace the way an input character is
drawn — i.e. render a codepoint *not* according to its intended meaning,
substituting a different glyph or a different drawing method? This audit was
prompted by a case where inner table borders looked like broken dashed lines,
and by the instruction *not* to "force" dashed box characters into solid ones.

**Scope.** `src/render.c` (`render_screen`, `draw_glyph`) and `src/main.c`
(font fallback chain). Build: `fb8ae92` ("Fix box-drawing border alignment").

**Method.** Enumerate every decision point that chooses *how* a character is
rasterised, then judge whether it changes the character's semantic identity
(which codepoint, which glyph design) or only its *presentation* (size,
position, colour, weight).

---

## Decision points

### 1. Codepoint → font face (`render.c:364-373`)
```c
for (int f = 0; f < c.n_faces; f++) {
    if (FT_Get_Char_Index(c.faces[f], ch) != 0) { pick = c.faces[f]; break; }
}
```
The first face in the fallback chain that *actually contains* the codepoint is
used. This can change **which font** draws a glyph (e.g. CJK falls through from
Noto Sans Mono to Noto Sans SC), but it never changes **which glyph** — the
glyph is always the font's own design for that exact codepoint. **Not a
semantic substitution.**

### 2. Box-drawing branch (`render.c:85-88`, `render.c:114-124`)
`is_box_drawing(ch)` selects a positioning mode, **not a different glyph**.
Inside the branch the real box glyph is drawn at its natural size, positioned
with the font's own `bitmap_left` / `bitmap_top` metrics:
```c
dw = w; dh = h;
pen_x = cell_x + bitmap_left;
pen_y = cell_y + baseline - bitmap_top;
sxf = syf = 1.0;
```
A box font (Noto Sans Mono) places every stroke on the cell grid, so adjacent
cells connect with no stretching. Crucially this preserves the glyph's design:
**U+2506 (BOX DRAWINGS LIGHT TRIPLE DASH VERTICAL) renders as a 3-segment
dashed line — exactly as Unicode defines it.** **Not a substitution; faithful.**

> The header comment above `render_screen` previously described the *opposite*
> approach ("a horizontal bar is stretched across the cell width … corners/crosses
> fill the cell"). That text described a stretching workaround the user had
> already rejected, and did not match the code. It has been corrected.

### 3. Non-box glyph fit-to-cell scaling (`render.c:126-142`)
```c
double s = 1.0;
if (w > avail_w) s = (double)avail_w / (double)w;
if (h > avail_h) s = (double)avail_h / (double)h;
if (s > 1.0) s = 1.0;
```
Glyphs are scaled **down only** when they exceed the cell, never upscaled, and
centred. This is standard terminal fit-to-cell behaviour: the outline is
preserved, only its size is clamped. **Not a semantic change.**

### 4. Synthetic bold (`render.c:189-219`)
```c
/* crude bold: redraw one pixel to the right for non-colour glyphs */
```
Bold text is faked by re-blitting the same glyph shifted one pixel right; no
bold font face is selected. This alters *presentation weight*, not the
character. The codepoint and its glyph design are unchanged. **A known
limitation (style workaround), not a character-substitution.** Documented here
so it is not mistaken for fidelity loss.

### 5. Colour (BGRA) glyphs (`render.c:145-166`)
Colour bitmap glyphs (Noto Color Emoji) use their own pixel colours; every
other glyph is tinted with the cell's foreground colour. This follows the
glyph's own nature — an emoji *is* coloured. **Faithful.**

### 6. Wide-character trailing half (`render.c:341-343`)
The second half of a wide character (`cell.width <= 0`) is skipped because its
area was already painted by the leading cell. Standard. **Not a change.**

### 7. Cell width from `M` advance (`render.c:287-292`)
`font_width` is taken from the advance of the representative `M` glyph, **not**
`max_advance`. A proportional face's `max_advance` can be inflated by a single
wide/fullwidth glyph, which made cells too wide and stopped box glyphs from
filling them. This was a bug fix, not a workaround, and keeps the cell size
faithful to the primary monospace face.

> `README.md` previously claimed `font_width` came from `max_advance`; that
> sentence has been corrected.

---

## Conclusion

**The renderer does not replace any input character's intended rendering.**
For every codepoint it draws the exact glyph the font provides for that
codepoint, using the font's own metrics. The only deliberate deviations from a
"naive" paint are:

- **Synthetic bold** (decision 4) — a *style* workaround; the character
  identity is preserved.
- **Fit-to-cell downscaling** (decision 3) — standard terminal behaviour; the
  glyph outline is preserved.

Neither changes a character's meaning or its designed glyph. In particular, the
earlier "broken dashed lines" were caused by the **source input using U+2506**
(a dashed box character), not by the renderer. The renderer drew it faithfully
as a dashed line.

## Guards against regressions

`tests/run_tests.py` adds pixel-level regression checks (run with `make test`
or `python3 tests/run_tests.py`). The case most relevant to this audit is
`dashed_not_solidified`: it renders a column of `│` (U+2502, solid) next to a
column of `┆` (U+2506, dashed) and asserts the solid line is *one* contiguous
segment while the dashed line is *many* segments. **Any future change that
"forces" dashed box characters into solid ones will fail this test**, which is
exactly the behaviour the user asked us not to introduce.

## Recommendation

- Keep box-drawing characters at natural size with font metrics. **Never
  stretch or re-centre them** — stretching moves junction arms off the bar
  positions and silently alters the glyph's design (e.g. would turn U+2506 into
  a solid bar).
- If a future requirement truly needs "all borders solid", do it by
  *documenting the input expectation* or by an explicit, opt-in flag — not by
  silently remapping codepoints in the blitter.
