# Stylus Touch-Mirror Parallel Refactor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. Each code-writing agent must work in a dedicated git worktree and must not revert or rewrite edits made by other agents.

**Goal:** 将 stylus solver 重构成与 `TouchPipeline` 同级的 full touch-mirror pipeline：单一 `Process(HeatmapFrame&)` 入口、统一大帧数据流、删除 `StylusFrameState` / `PenStateMachine` / centralized diagnostics / solver packet 责任，并把 touch interop 与 VHF packet 责任边界显式化。

**Architecture:** 先由一个 core contract worktree 冻结 `frame.stylus` 新数据模型和 stylus phase API，再让 VHF packet、TouchTracker interop、diagnostics/UI/test 三条支线从 core contract 后并行实施。最终用一个 integration worktree 合并所有支线，做编译修复、旧语义扫除和 spec-compliance review。

**Tech Stack:** C++23, CMake project, header-only stylus phase modules, `HeatmapFrame`, `TouchPipeline`, `VhfReporter`, focused native tests under `Tools/tests`.

---

## 1. Current State

- `main` 当前为干净工作树，`HEAD=92c0d53`，提交信息为 `wip: snapshot stylus touch-mirror baseline`。这是后续新 worktree 的唯一推荐基线。
- `main` 比 `origin/main` ahead 2，包含 `37a7ab3 docs: add stylus touch-mirror rebuild design` 和 `92c0d53` baseline snapshot。
- 仓库没有 `.worktrees/` 或 `worktrees/`，但已有 `.claude/worktrees/`，且 `.gitignore` 已忽略 `.claude/`。后续沿用 `.claude/worktrees/<branch>`，不再引入新的 worktree 根目录。
- 现有 `.claude/worktrees/stylus-touch-mirror-plan` 位于旧分支 `worktree-stylus-touch-mirror-plan@7f0c179`，包含旧 implementation plan，但基线早于 `main@92c0d53`。
- 现有 `.claude/worktrees/stylus-touch-mirror-core` 是 detached worktree。当前沙箱用户读 git 状态会触发 `dubious ownership`，且文件哈希抽样显示它与 `main` 不一致。不要把它当作新的执行基线；如需保留，只作历史参考。
- 当前用户已明确要求并允许我直接执行编译验证。本计划从现在开始允许并要求 agent 运行**聚焦的** `cmake --build build --target ... -j 8` 与对应 `ctest -R ...` 验证命令；只有在确实需要全量链接回归时才跑 `cmake --build build -j 8`。

## 2. Code Gap Against Spec

- `EGoTouchService/Solvers/StylusSolver/StylusPipeline.cpp` 仍在 `Process()` 内创建 `StylusFrameState`，并通过 `OutputState` 执行 begin/commit/finalize/reuse。
- `StylusPipeline` 仍持有 `PenStateMachine`、`StylusStateController`、`BtPressBuffer`、`NoiseGate`、`StylusDiagnosticsWriter` 等旧状态链对象。
- `EGoTouchService/Solvers/SolverTypes.h` 的 `StylusFrameData` 仍是 flat struct，release 字段中包含 `packet`、`packetRoute`、`animState`、`noPressInkActive`、`sustainOutput`、`fastLiftOutput` 和完整 `StylusDiagnostics diag`。
- `VhfReporter` 当前仍通过 solver 侧 `PacketBuilder` 回填 `frame.stylus.packet`，并写 `frame.stylus.diag.vhfPenState`。
- `TouchTracker` 当前仍把 `frame.stylus.animState >= 2` 当成 writing-like 判断，和设计稿的 interop contract 冲突。
- `Tools/tests/StylusPipelineModulesTest.cpp`、`Tools/tests/StylusPipelineFastInkTest.cpp`、`Tools/tests/TouchTrackerStylusSuppressTest.cpp`、`Tools/tests/VhfReporterStylusPacketTest.cpp` 均仍包含旧字段或旧 packet/state-machine 期望。

## 3. Worktree Topology

所有新 worktree 都从 `main@92c0d53` 或其后继 integration 分支创建，命名固定如下：

```powershell
git worktree add .claude/worktrees/stylus-touch-mirror-contract -b worktree/stylus-touch-mirror-contract 92c0d53
git worktree add .claude/worktrees/stylus-touch-mirror-vhf -b worktree/stylus-touch-mirror-vhf worktree/stylus-touch-mirror-contract
git worktree add .claude/worktrees/stylus-touch-mirror-touch-interop -b worktree/stylus-touch-mirror-touch-interop worktree/stylus-touch-mirror-contract
git worktree add .claude/worktrees/stylus-touch-mirror-diag-tests -b worktree/stylus-touch-mirror-diag-tests worktree/stylus-touch-mirror-contract
git worktree add .claude/worktrees/stylus-touch-mirror-integration -b worktree/stylus-touch-mirror-integration worktree/stylus-touch-mirror-contract
```

如果 `worktree/stylus-touch-mirror-contract` 还没有提交，后四个命令不要执行。先让 core contract agent 产出可编译的 contract commit，再从该提交 fork 支线。

## 4. Agent Assignment

| Agent | Worktree | Write Ownership | Depends On | Output Commit |
|---|---|---|---|---|
| A0 Coordinator | original `main` | plan only, no production code | none | this plan doc |
| A1 Core Contract | `.claude/worktrees/stylus-touch-mirror-contract` | `SolverTypes.h`, `StylusPipeline.*`, stylus phase API, `StylusFrameParser`, delete old state files | `92c0d53` | `refactor: freeze stylus heatmap-frame contract` |
| A2 VHF Packet | `.claude/worktrees/stylus-touch-mirror-vhf` | `VhfReporter.*`, reporter-private stylus packet helper, `VhfReporterStylusPacketTest.cpp` | A1 | `refactor: move stylus hid packet ownership to vhf` |
| A3 Touch Interop | `.claude/worktrees/stylus-touch-mirror-touch-interop` | `TouchTracker.hpp`, `TouchTrackerStylusSuppressTest.cpp`, touch-side references to stylus fields | A1 | `refactor: use stylus touch interop contract` |
| A4 Diagnostics And Tests | `.claude/worktrees/stylus-touch-mirror-diag-tests` | `StylusPipelineModulesTest.cpp`, `StylusPipelineFastInkTest.cpp`, debug-only fields, IPC/UI compile seams if required | A1 | `test: align stylus tests with touch-mirror contract` |
| A5 Integration | `.claude/worktrees/stylus-touch-mirror-integration` | merge seams, CMake target list, stale include/reference cleanup | A1+A2+A3+A4 | `refactor: integrate stylus touch-mirror rebuild` |
| A6 Review | read-only | spec compliance and code-quality review | A5 | review report only |

## 5. Execution Waves

### Wave 0: Baseline And Safety

- [ ] Confirm `main` is clean.

```powershell
git status --short
```

Expected: no output.

- [ ] Confirm `.claude/` is ignored.

```powershell
git check-ignore -q .claude
```

Expected: exit code `0`.

- [ ] Treat `.claude/worktrees/stylus-touch-mirror-plan` and `.claude/worktrees/stylus-touch-mirror-core` as stale/historical unless manually rescued. Do not let agents edit them.

### Wave 1: A1 Core Contract, Single Blocking Worktree

This is the only non-parallel blocker. It must finish before fan-out because every other agent needs the new `frame.stylus` contract.

**Files owned by A1:**

- Modify: `EGoTouchService/Solvers/SolverTypes.h`
- Modify: `EGoTouchService/Solvers/StylusSolver/StylusPipeline.h`
- Modify: `EGoTouchService/Solvers/StylusSolver/StylusPipeline.cpp`
- Rename: `EGoTouchService/Solvers/StylusSolver/StylusInputParser.hpp` to `EGoTouchService/Solvers/StylusSolver/StylusFrameParser.hpp`
- Modify: `EGoTouchService/Solvers/StylusSolver/CommonModeFilter.hpp`
- Modify: `EGoTouchService/Solvers/StylusSolver/GridPeakDetector.hpp`
- Modify: `EGoTouchService/Solvers/StylusSolver/CoordinateSolver.hpp`
- Modify: `EGoTouchService/Solvers/StylusSolver/StylusSignalAnalyzer.hpp`
- Modify: `EGoTouchService/Solvers/StylusSolver/PressureSolver.hpp`
- Modify: `EGoTouchService/Solvers/StylusSolver/NoiseGate.hpp`
- Modify: `EGoTouchService/Solvers/StylusSolver/CoorPostProcessor.hpp`
- Modify: `EGoTouchService/Solvers/StylusSolver/CoorReviser.hpp`
- Modify: `EGoTouchService/Solvers/StylusSolver/EdgeLiftCorrector.hpp`
- Create: `EGoTouchService/Solvers/StylusSolver/StylusOutputGate.hpp`
- Create: `EGoTouchService/Solvers/StylusSolver/StylusCoordinateFilter.hpp`
- Delete: `EGoTouchService/Solvers/StylusSolver/StylusFrameState.hpp`
- Delete: `EGoTouchService/Solvers/StylusSolver/PenStateMachine.hpp`
- Delete: `EGoTouchService/Solvers/StylusSolver/StylusDiagnosticsWriter.hpp`

**Contract to implement:**

- `StylusFrameData` becomes `input + output + interop + debug-only` rather than a flat legacy mirror.
- `StylusInputSnapshot` contains slave validity, checksum, block validity, status, and BT sample snapshot.
- `StylusOutputState` contains final `inRange`, `tipDown`, `pressure`, `StylusSolvePoint`, and final validity/confidence.
- `StylusTouchInterop` contains only touch suppression/AFT signals: `signalX`, `signalY`, `maxRawPeak`, `recheckPassed`, `recheckThreshold`, `recheckThresholdMulti`, `touchSuppressActive`, `touchSuppressFrames`, `touchNullLike`.
- `StylusDebugFrame` exists only under `#if EGOTOUCH_DIAG`.
- `StylusPipeline::Process(HeatmapFrame&)` snapshots BT pressure once, then linearly calls parser, conditioning, peak, coordinate, signal, pressure, output gate, coordinate filter, reviser, edge correction, debug cache.
- No `StylusFrameState`, no `OutputState`, no `CommitFinal`, no `ReuseCommittedFrame`, no `PenStateMachine`, no solver packet build, no centralized diagnostics writer.

**A1 acceptance gates:**

```powershell
rg -n "StylusFrameState|PenStateMachine|StylusDiagnosticsWriter|OutputState|ReuseCommittedFrame|CommitFinal|FinalizeFinal|FinalizeTerminal" EGoTouchService\Solvers\StylusSolver EGoTouchService\Solvers\SolverTypes.h
```

Expected: no references except deleted-file tombstones in git diff.

```powershell
rg -n "packetRoute|animState|noPressInkActive|sustainOutput|fastLiftOutput|StylusDiagnostics diag" EGoTouchService\Solvers\SolverTypes.h EGoTouchService\Solvers\StylusSolver
```

Expected: no release-path references. If some debug-only migration remains, it must be inside `#if EGOTOUCH_DIAG`.

```powershell
cmake --build build --target StylusPipelineModulesTest StylusPipelineFastInkTest -j 8
ctest --test-dir build --output-on-failure -R "StylusPipelineModulesTest|StylusPipelineFastInkTest"
```

Expected: A1 改造后至少这两个 stylus 核心测试可以重新编译并运行。

### Wave 2: Parallel Fan-Out From A1 Commit

After A1 commits, create A2/A3/A4 worktrees from A1 commit. These agents can run concurrently.

#### A2 VHF Packet Agent

**Files owned by A2:**

- Modify: `EGoTouchService/Device/vhf/VhfReporter.h`
- Modify: `EGoTouchService/Device/vhf/VhfReporter.cpp`
- Create or move: reporter-private stylus packet helper under `EGoTouchService/Device/vhf/`
- Modify: `Tools/tests/VhfReporterStylusPacketTest.cpp`
- Modify: `CMakeLists.txt` only if test target/source list changes

**Implementation contract:**

- `VhfReporter` builds stylus HID bytes from `frame.stylus.output`.
- Solver no longer owns or backfills `frame.stylus.packet`.
- Invalid/parse-failure zero-state emission policy belongs to reporter.
- Eraser post-transform stays reporter-local and must not mutate `frame.stylus.output`.

**A2 acceptance gates:**

```powershell
rg -n "PacketBuilder|frame\.stylus\.packet|packetRoute|diag\.vhfPenState" EGoTouchService\Device\vhf Tools\tests\VhfReporterStylusPacketTest.cpp
```

Expected: no solver-owned packet dependency. Any VHF-local packet bytes should be local variables or reporter-private types, not `StylusFrameData` fields.

```powershell
cmake --build build --target VhfReporterStylusPacketTest -j 8
ctest --test-dir build --output-on-failure -R "VhfReporterStylusPacketTest"
```

Expected: reporter 侧 packet 责任迁移后，VHF 相关测试重新编译并运行。

#### A3 Touch Interop Agent

**Files owned by A3:**

- Modify: `EGoTouchService/Solvers/TouchSolver/TouchTracker.hpp`
- Modify: `Tools/tests/TouchTrackerStylusSuppressTest.cpp`

**Implementation contract:**

- `TouchTracker` reads `frame.stylus.output` and `frame.stylus.interop` only.
- Writing-like logic must use `output.tipDown`, `output.inRange`, `output.pressure`, and interop signals, never `animState`.
- Suppression still respects signal/recheck/overlap behavior, but old state-machine fields are gone.

**A3 acceptance gates:**

```powershell
rg -n "animState|packetRoute|packet|noPressInkActive|sustainOutput|fastLiftOutput" EGoTouchService\Solvers\TouchSolver\TouchTracker.hpp Tools\tests\TouchTrackerStylusSuppressTest.cpp
```

Expected: no matches.

```powershell
cmake --build build --target TouchTrackerStylusSuppressTest -j 8
ctest --test-dir build --output-on-failure -R "TouchTrackerStylusSuppressTest"
```

Expected: touch interop 改造后，TouchTracker 抑制测试重新编译并运行。

#### A4 Diagnostics And Test Agent

**Files owned by A4:**

- Modify: `Tools/tests/StylusPipelineModulesTest.cpp`
- Modify: `Tools/tests/StylusPipelineFastInkTest.cpp`
- Modify: app/shared-frame diagnostic consumers only if compile diagnostics require it
- Modify: `CMakeLists.txt` only if target names or source lists change

**Implementation contract:**

- Replace parser/state-machine tests with frame-contract tests.
- Keep pressure, peak, coordinate, BT snapshot, and touch interop assertions.
- Remove fast-ink/lift/animation expectations because the spec intentionally deletes those semantics.
- If debug fields are asserted, guard test access with `#if EGOTOUCH_DIAG`.

**A4 acceptance gates:**

```powershell
rg -n "StylusFrameState|PenStateMachine|animState|packetRoute|packet|noPressInkActive|sustainOutput|fastLiftOutput" Tools\tests\StylusPipelineModulesTest.cpp Tools\tests\StylusPipelineFastInkTest.cpp
```

Expected: no matches except intentional comments explaining removed legacy semantics.

```powershell
cmake --build build --target StylusPipelineModulesTest StylusPipelineFastInkTest -j 8
ctest --test-dir build --output-on-failure -R "StylusPipelineModulesTest|StylusPipelineFastInkTest"
```

Expected: 测试改造完成后，这两个目标与其用例均可运行。

### Wave 3: A5 Integration Worktree

**Files owned by A5:**

- Modify: any merge-conflict seam among A1/A2/A3/A4
- Modify: `CMakeLists.txt`
- Modify: includes and stale references across `EGoTouchService`, `Common`, `Tools`

**Integration steps:**

- [ ] Start from A1 contract commit.
- [ ] Cherry-pick A2, A3, A4 in that order.
- [ ] Resolve CMake/includes/type-name seams.
- [ ] Run global stale-reference scans.
- [ ] Run focused compile/test targets, then optional full build if focused验证已稳定。

```powershell
rg -n "StylusFrameState|PenStateMachine|StylusDiagnosticsWriter|StylusInputParser|PacketBuilder|packetRoute|animState|sustainOutput|fastLiftOutput|noPressInkActive" EGoTouchService Common Tools
```

Expected: no production references to deleted concepts. If `PacketBuilder` remains, it must be reporter-private and not under `Solvers/StylusSolver`.

- [ ] Provide the user with build targets to run locally.

Recommended user-side targets:

```powershell
cmake --build build --target StylusPipelineModulesTest -j 8
cmake --build build --target StylusPipelineFastInkTest -j 8
cmake --build build --target VhfReporterStylusPacketTest -j 8
cmake --build build --target TouchTrackerStylusSuppressTest -j 8
```

Integration-side validation:

```powershell
ctest --test-dir build --output-on-failure -R "StylusPipelineModulesTest|StylusPipelineFastInkTest|VhfReporterStylusPacketTest|TouchTrackerStylusSuppressTest"
cmake --build build -j 8
```

Expected: 相关聚焦测试先通过，再确认全量默认 build 仍可链接成功。

### Wave 4: A6 Review

The review agent is read-only and should check the integrated branch against `docs/superpowers/specs/2026-04-23-stylus-touch-mirror-rebuild-design.md`.

**Review checklist:**

- `StylusPipeline::Process()` is structurally comparable to `TouchPipeline::Process()`: direct phase calls on `HeatmapFrame`, no hidden scratch owner.
- `StylusFrameState.hpp`, `PenStateMachine.hpp`, and `StylusDiagnosticsWriter.hpp` are deleted.
- `StylusFrameData` release fields are narrowed to final output plus touch interop input.
- VHF packet construction is outside solver.
- `TouchTracker` does not depend on old stylus lifecycle or packet fields.
- Diagnostics are debug-only and phase-owned.
- Tests no longer assert deleted fast-ink/lift/animation behavior.

## 6. Merge Policy

- Do not merge A2/A3/A4 directly into `main`; merge them only into A5 integration after A1 freezes the contract.
- Do not reuse stale `stylus-touch-mirror-plan` or `stylus-touch-mirror-core` as write bases.
- Keep each agent commit focused. If an agent needs to touch another agent's file, it must report `BLOCKED` instead of widening scope.
- Do not run destructive cleanup of existing worktrees. If old worktrees need removal, ask explicitly first.

## 7. Practical Agent Prompts

Use these as controller prompts after creating each worktree.

**A1 prompt:**

```text
你不是唯一在代码库中工作的 agent。不要回退他人的改动。你负责 core stylus touch-mirror contract，写入范围仅限本计划 A1 文件集。从 main@92c0d53 创建的独立 worktree 开始。严格实现 docs/superpowers/specs/2026-04-23-stylus-touch-mirror-rebuild-design.md：删除 StylusFrameState/PenStateMachine/StylusDiagnosticsWriter，重写 StylusFrameData 为 input/output/interop/debug-only，重写 StylusPipeline::Process(HeatmapFrame&) 为 TouchPipeline 风格顺排。不要修改 VhfReporter 或 TouchTracker。完成后运行计划中的 rg 扫描与聚焦 `cmake --build` / `ctest` 验证，列出改动文件和验证结果。
```

**A2 prompt:**

```text
你不是唯一在代码库中工作的 agent。不要回退他人的改动。你负责 VHF packet 迁移，写入范围仅限 VhfReporter.*、reporter-private stylus packet helper、VhfReporterStylusPacketTest.cpp 和必要 CMake。基线必须是 A1 contract commit。把 stylus HID packet 从 frame.stylus.output 组装，solver 不再写 frame.stylus.packet。完成后运行计划中的 rg 扫描与 `VhfReporterStylusPacketTest` 编译/测试验证，列出改动文件和验证结果。
```

**A3 prompt:**

```text
你不是唯一在代码库中工作的 agent。不要回退他人的改动。你负责 TouchTracker interop，写入范围仅限 TouchTracker.hpp 和 TouchTrackerStylusSuppressTest.cpp。基线必须是 A1 contract commit。TouchTracker 只能依赖 frame.stylus.output 与 frame.stylus.interop，不允许 animState、packetRoute、packet 或旧 lifecycle 字段。完成后运行计划中的 rg 扫描与 `TouchTrackerStylusSuppressTest` 编译/测试验证，列出改动文件和验证结果。
```

**A4 prompt:**

```text
你不是唯一在代码库中工作的 agent。不要回退他人的改动。你负责 diagnostics/test alignment，写入范围仅限 StylusPipelineModulesTest.cpp、StylusPipelineFastInkTest.cpp、必要 debug-only 消费端和必要 CMake。基线必须是 A1 contract commit。移除旧 StylusFrameState/PenStateMachine/animState/packetRoute 期望，保留 parser、peak、coordinate、pressure、BT snapshot、touch interop 的新 contract 测试。完成后运行计划中的 rg 扫描与聚焦 `cmake --build` / `ctest` 验证，列出改动文件和验证结果。
```

**A5 prompt:**

```text
你不是唯一在代码库中工作的 agent。不要回退他人的改动。你负责 integration worktree，从 A1 contract commit 开始依次 cherry-pick A2/A3/A4，解决类型、include、CMake、测试编译缝隙。不要重新引入 StylusFrameState、PenStateMachine、StylusDiagnosticsWriter、solver packet build 或 animState。完成后运行全局 rg stale-reference 扫描、相关 `ctest` 和一次 `cmake --build build -j 8`，整理验证结果与任何 compiler diagnostics。
```
