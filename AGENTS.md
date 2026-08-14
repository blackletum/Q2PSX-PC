# AGENTS.md

Instructions for AI agents working in this repository.

## Show the work visually

This project is a visual recreation — the output of most changes is something you can
*look at*. Take every reasonable opportunity to show it.

- **Produce an image whenever the change has a visible result.** Rendering, GPU/GTE
  behaviour, HUD, menus, screen layout, model/texture handling, lighting, sorting
  artifacts — if a change alters pixels, capture them and show them.
- **Show it in the response, not just in a file.** Send the image to the user
  (e.g. `SendUserFile`) so it renders inline. A path in prose is not showing it.
- **Before/after beats after.** For a fix or a visual regression, capture both states
  and present them side by side (or as two images), pointing out what to look at.
- **Mid-task counts.** Don't batch everything to the end. When a milestone renders for
  the first time, show it then — it's the fastest way for the user to catch a wrong turn.
- **Prefer real output over mockups.** Use the actual renderer or the actual tool:
  run the game and capture a frame, or use `q2psx-inspect export` to decode assets to
  PNG. Never hand-draw an approximation of what the code "should" produce and present
  it as a result.
- **Non-visual work still has visuals.** Data layouts, format structures, pipeline
  stages and state machines are often clearer as a diagram than as prose. Use one when
  it genuinely explains something a paragraph would not.
- **Caption it.** One line saying what the image shows and what to notice. An
  uncaptioned screenshot makes the user do the diffing.
- **Capture with `--headless`, always.** `--frames N --shot` alone does NOT imply it,
  and without it the client advances on the WALL CLOCK: `dt` varies per frame and the
  run is not reproducible. Three identical `--frames 500` captures of BASE3 gave 497,
  497 and 4567 projectile lights; with `--headless` they give 499 every time, because
  that path uses a fixed 1/30 s step. Any per-frame number — lights added, sounds
  played, bolts alive, creatures hunting, units moved — is worthless without it. Load-
  time counts (rotators built, models placed, lights in the map) are unaffected, so a
  before/after on those survives; anything cumulative does not.

If a change is genuinely invisible (pure refactor, build system, tests), say so and
skip the image rather than manufacturing a filler one.
