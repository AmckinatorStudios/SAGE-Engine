# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

SAGE Engine is a general-purpose modular 3D game engine (C++17 / OpenGL) with an entt-based ECS, an ImGui editor, Lua scripting, Jolt physics, and a graphics-backend abstraction (RHI). Version lives in the root `VERSION` file (single source of truth, baked into `sage/core/Version.h`). Status: pre-1.0, public API may still change.

**Language convention: comments, commit messages, log strings, and docs are written in Russian.** Follow this for new code and commits.

## Build & test commands

```bash
# Configure + build (Linux). Build type defaults to Release if unset.
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# Build a single target
cmake --build build --target Sandbox        # or SageEditor, TestGame, sage_engine, sage_tests

# Unit tests (fast, no GL context)
ctest --test-dir build --output-on-failure
# or directly (prints per-test pass/fail):
./build/tests/sage_tests

# Headless smoke tests — the same script CI runs; reproduces CI failures locally
./scripts/ci_smoke_test.sh build

# Faster builds / no-network environments: skip the Jolt physics backend
cmake -B build -DSAGE_PHYSICS_JOLT=OFF

# Windows cross-compile check (what CI's second job does)
cmake -B build-windows -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-toolchain.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-windows -j$(nproc)

# Run locally
./build/games/sandbox/Sandbox
./build/editor/SageEditor
```

Linux build deps: `cmake g++ libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl1-mesa-dev` (plus `xvfb` for headless smoke tests). Third-party libs are fetched by CMake (`FetchContent`) or vendored in `external/` — first configure needs network.

Release packaging: `scripts/build_linux.sh [GameName]`, `scripts/build_windows.sh`, `scripts/build_all.sh` → self-contained archives in `dist/`.

### Tests

- `tests/` is a minimal homegrown framework (`tests/TestFramework.h`): a test is `TEST(name) { CHECK_* ... }` in a `tests/test_*.cpp` file, registered statically. There is **no single-test filter** — the whole `sage_tests` binary runs every test (it's fast). New test files must be added to the explicit source list in `tests/CMakeLists.txt`.
- Unit tests never require a GL context. Anything needing rendering belongs in the smoke tests (`scripts/ci_smoke_test.sh`), which run the real binaries headless under `xvfb-run` and grep their logs for markers (editor self-test, TestGame autopilot, editor E2E that builds and runs a game).
- CI (`.github/workflows/ci.yml`) runs on every push/PR: Linux build + ctest + smoke script, and a MinGW cross-compile build-only job.

## Architecture

Three strictly separated layers, orchestrated by the root `CMakeLists.txt` (which declares all third-party deps once):

- **`engine/`** → static library `sage::engine`. The public API is everything included as `<sage/...>` from `engine/src/sage/**`. Consumers cannot reach internals past this prefix.
- **`editor/`** → `SageEditor` executable (ImGui, docking). Links the engine.
- **`games/<name>/`** → independent game executables linking the engine. `games/sandbox` is the minimal reference example; `games/testgame` is a full stress-test game exercising every subsystem (runs in CI autopilot).
- **`runtime/`** → `SagePlayer`, the generic runtime that plays projects built from the editor.

**Hard boundary rule:** `engine/` and `editor/` code never includes anything from `games/*`. If you touched `engine/`, verify before committing:

```bash
grep -rnE '#include "(\.\./)*games/' engine/ editor/   # must be empty
```

Other structural invariants:

- Device-level `#include <glad/...>` exists **only** in `engine/src/rhi/opengl/` — the rest of the engine talks to the `GraphicsDevice` interface (`sage/rhi/GraphicsDevice.h`). A new graphics backend = new implementation of that interface.
- Engine sources are an **explicit list** in `engine/CMakeLists.txt` (no GLOB) — new `.cpp` files must be added there.
- GLFW is built with `GLFW_INCLUDE_NONE`; never include GL headers via GLFW.
- A new game = `games/<name>/CMakeLists.txt` calling `sage_add_game(NAME ... SOURCES ... ASSETS ...)` (see `cmake/SageHelpers.cmake`) + `add_subdirectory` in the root CMakeLists. Entry point: implement `sage::CreateApplication`, push a `sage::Layer` subclass, end with `SAGE_MAIN()`. Copy the structure from `games/sandbox`.

### Engine subsystems (`engine/src/sage/`)

- `core/` — `Application` owns the window/main loop; games and the editor attach as `Layer`s (`OnAttach/OnUpdate/OnRender`). Also `InputSystem`, `Log`, `Config` (`EngineConfig`), `JobSystem`, `Tween`.
- `scene/` + `ecs/` — a scene is an `entt::registry`; entities are component bags (`Transform`, `MeshRenderer`, `Hierarchy`, `Script`, ...), `GameObject` is a cheap handle, world matrices composed via `Scene::WorldMatrix`. Serialization to `.sage` JSON via `SceneSerializer`.
- `render/` — shaders, meshes, models (obj/gltf), skinned models, materials, skybox, shadow mapping, HDR post-processing, particles, `DebugDraw`, fonts.
- `anim/` — skeleton/animator, clip cross-fading.
- `physics/` — `PhysicsWorld` abstraction with pluggable backends: Jolt (default), Simple (always available, dependency-free), Null.
- `scripting/` — Lua via sol2 (`ScriptEngine`); scripts attach to entities via `ScriptComponent`.
- `audio/` — miniaudio-based 2D/3D sound.
- `ui/` — immediate-mode UI + TrueType text (`UIRenderer`, default font copied next to every game binary).

### Threading model (do not break these invariants)

- GL commands are issued **only from the main thread** (single context). Frame prep (frustum culling in `RenderBatch::CollectVisible`) is parallelized via `JobSystem::ParallelFor`, but ECS iteration order and merge are sequential, so frame output is deterministic.
- Lua scripts run sequentially on the main thread by design (one `sol::state`; parallel structural ECS edits are UB). Jolt manages its own thread pool.

### Editor

`editor/src/EditorLayer.*` is the core (scene/project/Play mode/undo); panels live in `editor/src/panels/` (one class each), talking to the editor through the `EditorHost.h` contract. Editor plugins (`editor/plugins/`, built with `sage_add_editor_plugin`) are MODULE libraries that deliberately do **not** link imgui or the engine — ImGui symbols resolve at runtime against the host binary (see `cmake/SageHelpers.cmake` for why; linking `libimgui.a` into a plugin crashes). Plugins are opt-in at runtime via `SAGE_EDITOR_PLUGINS=1`.

### Headless/CI env hooks

Binaries support env-driven automation used by the smoke tests: `SAGE_SCREENSHOT_AT_FRAME` / `SAGE_SCREENSHOT_PATH`, `SAGE_WINDOW_WIDTH` / `SAGE_WINDOW_HEIGHT`, `SAGE_EDITOR_SELFTEST=1`, `SAGE_EDITOR_E2E=1`, `SAGE_TESTGAME_AUTOPILOT=1`. Prefer extending these when adding CI coverage that needs rendering.

## Key references

- `README.md` — extensive per-subsystem documentation (in Russian), including scripting API, physics, animation, plugin API, and roadmap. Consult it before designing changes to a subsystem; update it when behavior changes.
- `cmake/SageHelpers.cmake` — `sage_add_game()` / `sage_add_editor_plugin()` contracts.
- `scripts/ci_smoke_test.sh` — exact CI expectations (log markers that must appear).
