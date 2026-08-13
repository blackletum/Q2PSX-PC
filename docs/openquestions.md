# Open Questions — Quake II PSX (`SLES-01534`) reverse engineering

Prioritised by how much each blocks the native port. Items in **Tier 1** stand between the project and a
level you can walk around in. The tail is cosmetic or archival.

Full structural detail, evidence and confidence markers live in [`FORMATS.md`](./FORMATS.md).

Legend: `[ ]` open · `[~]` partially resolved · `[x]` resolved (move the item, keep the answer)

---

## Tier 1 — Blocking: cannot render or load a level

- [ ] **1. `SNDVRAM.DAT` section A pixel compression codec.**
      Every texture and 2D image on the disc is behind it; nothing textured can be drawn.
      *Attack:* start from a stub map's smallest payload — record count is 1–2 and the packed image-name
      list bounds the data exactly. Remember offsets are relative to `0x0C`, not file-absolute.
- [ ] **2. `CastList` face vertex-index base.**
      Part-relative fails on 34 % of faces (26,237 / 76,320) and is unsolvable for 257 of 965 models;
      whole-model absolute leaves most vertices of large models unreferenced. No model can be drawn correctly.
      *Attack:* the renderer in `SLES_015.34`. `CastList` block C (8 bytes per part) is the likely home of a
      per-part vertex base.
- [ ] **3. World coordinate scale (×5 vs ×10 vs other).**
      Player size, step height, speeds and weapon ranges all depend on it. The authoring grid is visible
      (multiples of 640 are 86× enriched; 320 52×; 160 31×; 80 18×) but the multiplier is not established.
      *Attack:* read the player bounding-box / step-height / max-speed constants from the EXE and divide by
      the PC Quake II values (56 / 18 / 320).
  - [ ] 3a. Sub-question: dominant world-Y values arrive in **±1 pairs one unit below** the grid multiple.
        A `floor(x*S)` exporter would explain both that and the asymmetric `Scene` bbox slop at once.
- [ ] **4. `Events` operand stream.**
      The `size` / `sub` / `cls` framing is exact (walks land on chunk end in 164/164 files) but the body
      bytes are opaque. No doors, lifts, buttons, teleports, zone transitions or level progression without it.
      This is the gate between "static geometry viewer" and "game".
- [ ] **5. Collision plane point encoding — only 95.6 % confirmed.**
      The `uint16[3]` at `+0x00` reads as an unsigned offset from the owning node's `bboxMin`: 46,968 /
      49,148 planes land inside their node, 91 % of nodes are convex-consistent. Not good enough to trust
      player movement to. *Attack:* resolve the residual 4.4 % — different base, sign convention, or a second
      plane class?
- [ ] **6. `LevelBin` / `CreAIBin` module ABI and the `Rel` fixup encoding.**
      Confirmed MIPS R3000; every fixup is a valid in-`Bin` offset, but only 31 % are 4-aligned, so it is not
      a plain word-address list. Level scripting and monster AI both sit behind this.

---

## Tier 2 — Blocking: degrades the level badly, does not prevent loading

- [ ] **7. `SortData` encoding.** Bit-packed, no offset table, no fixed per-node record (4.0…88.6 bytes per
      scene node — a 22× spread). Almost certainly draw-order data; transparency and overdraw will be wrong
      without it. Requires the EXE's bit reader.
- [ ] **8. `AreaConx` 9-byte link payload.** No fixed offset yields a 1.3.12 unit normal in more than 39 % of
      3,494 links. Histograms suggest **unaligned** `int16_t` values that no single struct layout can express
      (links start at `record + 1 + 9*L`, so parity alternates). Byte `+3` is the best neighbour-index
      candidate. Blocks portal-based visibility.
- [ ] **9. `SpaceLights` per-node partition.** Flat `uint16_t` index array, no length prefix, no discoverable
      partition (0.68…7.12 entries per scene node). Blocks correct dynamic/ambient lighting.
- [ ] **10. `Population` `spawn.classId` target table.** 25 distinct values 0…37; 15 of 673 records exceed
      the map's `ModelNames` count and name resolution yields semantically wrong results. Monsters and items
      cannot be mapped to classes until it is found.
- [ ] **11. `MapMod` `Poly.uvIdxFlags` bits 6–7.** Render flags on 11.7 % of all polygons — likely
      semi-transparency / double-sided / no-texture. Guessing will look wrong on a tenth of the world.
- [ ] **12. `Scene` node fields `flags08`, `unk0C`, `unk0D`, `unk0E`.** `unk0E` (0…197, 119 distinct values,
      non-zero on all but 3 of 17,035 nodes) is the highest-value single byte in the zone format.
      *Attack:* find the 52-byte-stride `Scene` reader in the EXE and see what it does with byte 14.

---

## Tier 3 — Behavioural / audio-visual polish

- [ ] **13. Which in-game situation selects which music id.** The id lives in a `$gp` global whose writer was
      not traced; the per-map id is probably in the level `.DAT` chunks. Music is silent or arbitrary
      without it.
- [ ] **14. Does the engine loop XA tracks?** The duration field is converted to 50 Hz ticks and stored to
      two globals with a 30.0 s fallback — that looks like a countdown to a restart or fade — but the
      consuming code was not disassembled. One entry is **1.0 s short** of its measured stream length, hinting
      the value is a deliberate restart point rather than a length.
- [ ] **15. MDEC output depth for the movies (24-bit vs 15-bit).** Blocked on #16.
- [ ] **16. Locate the movie player overlay.** The executable contains **no** `.STX` / `MOVIES` / `STX`
      string at all, so both the player and its filename assembly live elsewhere. Solving this also settles
      the movie filename suffix and #15 in one pass.
- [ ] **17. SPU RAM base / reverb work area,** and whether reverb is disabled — the worst-case map leaves a
      **240-byte** margin against SPU RAM, suspiciously tight if a reverb buffer is also allocated.
- [ ] **18. `VramImageRec.width` / `height`: dimensions, or VRAM placement coordinates?** Both readings are
      legal for every observed pair. Settled the moment one payload is decompressed and its pixels counted —
      i.e. by solving #1.
- [ ] **19. The secondary 512 × 256 display-env init function** (single unrelated caller). Proves the game
      switches display configurations at least once — boot screen? loading screen? FMV? Resolve before the
      port commits to a single display model.
- [ ] **20. The `VSync(3)` path** (16.67 Hz on PAL), reached only through a function pointer. FMV, menu or
      load-screen tick?

---

## Tier 4 — Low-impact unknowns

- [ ] 21. `CastList` blocks A (8-entry directory confirmed, payloads undecoded), B (per-instance, articulated
      models only, 12 distinct sizes), C (per-part 8-byte elements — see #2), D; the animated-model frame
      layout; and the header's 24-bit field at `+0x01` (range 261…333367).
- [ ] 22. `PrimaryRemap` value space. Definitively **not** a scene-node index — the max exceeds the scene
      node count in 100 of 115 files. Probably a polygon or surface id in a shared table.
- [ ] 23. `CollNode` fields `c` (0…65,077,433, non-monotonic — 609 of 23,003 steps decrease) and `d` (0…75).
- [ ] 24. `Resources` `unk0` (−3000…6600, 49 distinct) and `unk4` (40…180, 17 distinct); `unk3` (64, but 80
      in two records).
- [ ] 25. `TrigBounds` trigger `id` (9…75 plus 255, where 255 is "none") and `flags` (14 distinct values up
      to 10240) semantics.
- [ ] 26. The five `Lights` style values (`(n<<3)|7` for n = 0…4) — what each style actually *does*.
- [ ] 27. Pickup `flags` bits beyond 0, 1 and 8; and the pickup `extra` list's meaning (a consumer exists — a
      pointer to it is stored into the spawned entity's sub-structure — but the interpretation is unknown).
- [ ] 28. `Q2Level` `+0x1C` (constant `jr ra` address, no located reader — possibly the high half of an
      8-byte field whose low half is the runtime pointer at `+0x18`); the writers of the per-level
      `runtime[8]` state; and the `music_playlist` field's real meaning, given that **no instruction anywhere
      in the image loads offset `0x22` from a level record**.
- [ ] 29. `SNDVRAM` section A header bytes `0x0E` (17 for front-end maps, 29…86 for gameplay) and `0x0F`
      (1…181, constant within some map families but not others).

---

## Tier 5 — Archival / other-build / process

- [ ] 30. **NTSC build values:** framebuffer height, `video_mode_const`, movie filename suffix, EXE hash,
      PVD fields. All must be **read**, never guessed — PAL turned out to be 512 × **248**, not the widely
      assumed 256, so the folklore 512 × 240 NTSC figure is *less* trustworthy now, not more.
- [ ] 31. Locate real xrefs to the `.DAT` chunk-name literal pool at `0x800AD414`. Would settle its true
      extent and whether a required-vs-optional flag exists per chunk. Blocked on a working disassembler
      session.
- [ ] 32. Why `ModelNames` is present in all 49 `COMMON.DAT` files yet the string appears **zero** times in
      the 634,880-byte executable. Dead tool-only data, positional access, or a runtime-assembled name?
- [ ] 33. Why `TriggerRemap` and `SecondaryRem` exist in the executable but are emitted by no file on the
      disc. Cut features, or read from a source not on this disc — a parser should tolerate them appearing.
- [ ] 34. Why the zone directory order permutes the `SecondaryCol` / `PrimaryRemap` / `AreaConx` trio as a
      function of zone index. Perfect and exceptionless correlation; the build-tool mechanism is unexplained.
      Matters only as further proof that index-based chunk lookup is unsafe.
- [ ] 35. `MAP.ALL`'s purpose; its four header words at `+0x40` (three decode as plausible floats near
      −1.93…−1.97); and whether the 16-entry table length is right (unverifiable — N = 1). Almost certainly
      an editor leftover: the filename appears **zero** times in the EXE and the file's last 44 bytes are
      MSVC uninitialised-heap fill.
- [ ] 36. The unused 20-byte tail of every Form 2 payload, and the always-zero `uint16_t` at `+2` of each
      music table record. Both are zero in 100 % of samples — nothing can be inferred from this disc.
- [ ] 37. `GlintMod` (2608 bytes, one map only, high-entropy after the first few dozen bytes).

---

## ⚠ Security note (carried forward, do not drop)

- [ ] 38. A prior research pass reported that a fan wiki page about this game served content containing
      **instructions addressed to AI agents** (create files, transfer funds, insult the operator, terminate
      operations). That URL was **not** fetched during verification. **Treat it as hostile for any automated
      fetch**; if data from it is wanted, a human should open it in a browser.
      Consequently, all web-sourced claims in the release census — the existence of the NTSC SKU, its
      timestamps and track lengths, barcodes, magazine demo-disc serials, and the absence of a Japanese
      release — remain **unverified** and should be treated as moderate confidence at best.
