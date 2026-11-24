# MkRender work left

Only work that is left. When an item is done, remove it here and describe it in the engine's `docs/CHANGELOG.md` under the item's name (MkRender versions are in `git log`).
Sources: engine `docs/ROADMAP.md` "Scene editing in MkRender, leftovers", "MkRender leftovers" and "Design refresh for the rest of the suite"; engine `docs/TOOL_DESIGN.md` sections 6 and 7; the scene tree, inspector and viewport items of the engine's "Editor usability" list (moved here 2026-11-22, sections 4 and 5; numbers are those of the 2026-11-04 list).

Only work an agent can do without a person is listed here. MkRender's hand checks (the screenshot beside the editor at the same DPI for TOOL_DESIGN section 6 item 7, dragging a `gizmo=radius` handle) and the decision whether to publish MkRender as a product of its own are in the engine's `docs/ROADMAP_HUMAN.md` (moved 2026-11-24).

**Suggested order**: 4 one piece at a time when scenes grow (FuseTag maps).

## 4. Usability items moved from the editor (2026-11-22)

The editor's scene editing left in MkRender step 10, so these items of the engine's "Editor usability" list apply to MkRender's hierarchy, inspector and view. Not scheduled: pick before work starts, and put open design questions in the engine's `docs/DECISIONS.md` first. MkRender has a plain name/kind text search in the hierarchy, a single "Markers and areas" switch, names of the selected objects (all with Alt) in the view, and "Select all"; the rest below is missing (checked in `tool/App.cpp`).

- [ ] **Scene tree**: 19. color tags (in the existing `.view` file), connecting lines and row stripes. 21. filter words such as `t:spawn`, `is:hidden`, `has:problem` (the `!` badges exist) and "select all of this kind". 22. selection history (`Alt+Left/Right`) and favorite objects. 23. hover link between hierarchy rows and the view. Left from done items: separators (18), a prefab badge (20; objects keep no prefab link yet).
- [ ] **Inspector**: 10. pinned and multiple inspectors for comparing objects, and remembering which sections are collapsed. Left from done items: tabs in field groups (2), drag and drop and a preview button in asset/event pickers (4), `validate` rules (6).
- [ ] **View**: 30. icons and labels for spawns, cameras and probes, toggled per kind (today one switch for all markers). 31. a small preview window for the selected virtual camera (today "Look through selected camera" goes through a running editor). Left from done items: `gizmo=point` (29; `gizmo=radius` exists).

## 5. What MkRender does not cover (2026-11-22)

By design it stays with the MealKit editor, which MkRender reaches over the live link (checked on Linux with `--editor-play`): playing (solo, from here, multi-player, game code hot reload, sim debug), the look-through and cutscene previews drawn by the running game's camera brain, the feedback, tables and tuning panels, event light previews, and building and exporting.
Not covered yet and not scheduled: viewing the scene through a virtual camera or a cutscene with no editor running (item 31 would cover the camera), and the items of section 4.
