# pexninja on Windows — native MSVC build

This folder is a thin **Visual Studio (MSVC) wrapper** that builds the
[`pexninja`](../) reference client on Windows from the same
[`../pexninja.cpp`](../pexninja.cpp) the CMake build uses.

It is the answer to "can I build pexninja for Windows using the Pulse NuGet?".
The Pulse runtime comes straight from the **`Pexip.Pulse` NuGet package** that
ships in this repo under [`../../../sdk/windows`](../../../sdk/windows) — the
exact same package the managed [`windows`](../../windows/) WinForms demo uses,
but consumed by a *native* C++ target here. The package provides:

* the C headers (`build/native/include/pulse*.h`),
* the `pexpulse.lib` import library, and
* the `win-x64` runtime DLLs (`pexpulse.dll`, `pexlgpl.dll`, `tbb12.dll`, …)
  plus the ONNX `share/models/…` assets.

Everything else pexninja needs (Dear ImGui *docking*, ImPlot, gl3w,
ImGui-Addons and GLFW) is fetched into `third_party/` by `fetch-deps.ps1` at
the **same pinned versions** the [CMakeLists.txt](../CMakeLists.txt) uses, so
the two builds stay in lock-step.

## Prerequisites

* **Visual Studio 2022** with the *Desktop development with C++* workload
  (toolset `v143`), or the Build Tools + `msbuild`.
* **git** and **python** on `PATH` (python is only needed once, to run gl3w's
  generator).
* **NuGet** — either the `nuget.exe` CLI, or `dotnet`/Visual Studio's built-in
  restore.
* Internet access for the one-time dependency fetch + NuGet restore.

## Build

From a *Developer PowerShell for VS 2022* in this directory:

```powershell
# 1. Fetch ImGui / ImPlot / gl3w / ImGui-Addons / GLFW into third_party\
powershell -ExecutionPolicy Bypass -File .\fetch-deps.ps1

# 2. Restore the Pexip.Pulse NuGet package from ..\..\..\sdk\windows
nuget restore pexninja.sln

# 3. Build
msbuild pexninja.sln /p:Configuration=Release /p:Platform=x64
```

Or just open **`pexninja.sln`** in Visual Studio and build — but run steps 1–2
first (the project fails fast with a clear message if either is missing).

The produced executable and all of the Pulse runtime DLLs + models are copied
to:

```
build\Release\pexninja.exe
```

## Run

```powershell
.\build\Release\pexninja.exe
```

pexninja opens its ImGui window; point it at a Pexip Infinity node and join a
conference exactly as on Linux/macOS. See the
[parent README](../README.md) for what the client can do.

## How it fits together

| Piece | Source | Wired up by |
| ----- | ------ | ----------- |
| Pulse headers + `pexpulse.lib` | `Pexip.Pulse` NuGet | `Pexip.Pulse.props` (auto-imported) |
| Pulse runtime DLLs + models | `Pexip.Pulse` NuGet | `CopyPulseRuntime` target → next to the `.exe` |
| ImGui (docking), ImPlot, gl3w, ImGui-Addons | git, pinned | `fetch-deps.ps1` → `third_party\` |
| GLFW (win-x64) | prebuilt release zip | `fetch-deps.ps1` → `third_party\glfw\` |

Because the NuGet lays its headers out *flat* (`pulse.h`, not
`pexpulse/pulse.h`), `pexninja.cpp` includes the Pulse header by its bare name
under `HOST_WINDOWS`; on Linux/macOS it keeps using `<pexpulse/pulse.h>`.

> Only **x64** is configured — `Pexip.Pulse` ships native binaries for
> `win-x64` only.
