# MkRender work left

Only work that is left. When an item is done, remove it here and describe it in the engine's `docs/CHANGELOG.md` under the item's name (MkRender versions are in `git log`).
Sources: engine `docs/ROADMAP.md` "Scene editing in MkRender, leftovers", "MkRender leftovers" and "Design refresh for the rest of the suite"; engine `docs/TOOL_DESIGN.md` sections 6 and 7.

**Suggested order**: 1 (look, small and visible every day) → 3 → 4; 2 when the engine has flipbook clips.

## 1. Suite look (engine TOOL_DESIGN section 6 checklist)

MkRender has the Spectrum 2 tokens, the icon font, icon buttons, the dark title bar, the app icon, outlined checkboxes and panel tab icons (engine CHANGELOG "Design refresh ... part 6"). Left:

- [ ] **Brush radio buttons in a capture**: the terrain and scatter brush radio buttons use `radioButton` but haven't been seen on screen yet.
- [ ] **A right-click item for each icon action** (TOOL_DESIGN 1 "second way in"): hierarchy rows, viewport toolbar, Environment and Assets buttons.
- [ ] **First screen and empty states** to icon + label buttons with one primary button (`Open Scene`), the empty-state sentence in Body Large (TOOL_DESIGN 1, 4).
- [ ] Section 6 item 7: a screenshot beside the editor at the same DPI, then update the MkRender row of TOOL_DESIGN section 7.

## 2. Sprites, later

- [ ] With the engine's flipbook clips (engine ROADMAP "2D on the 3D engine"): previewing a clip on the selected sprite.

## 3. Render goldens without the engine (engine ROADMAP "MkRender leftovers")

- [ ] A test in `tests/` that opens a hidden window, renders a fixed scene from plain meshes and compares it like `engine/tests/golden`; `--frames N --capture` and `--render-golden` are the start. DX12 and Vulkan both (`build-vulkan\`).

## 4. Later

- [ ] Publishing MkRender as a product of its own: decided after 3 (engine `docs/DECISIONS.md` 2026-11-12).
