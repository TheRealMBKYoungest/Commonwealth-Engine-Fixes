# Commonwealth Engine Fixes (CEF): source code

This is the source for everything in the CEF 1.0.5a download on Nexus Mods, published so anyone can review it. Download the
mod from Nexus. This repository holds source only, with no compiled DLLs.

## AI disclosure
**This mod was made with generative AI.** CEF was co-developed with Claude (Anthropic), an AI model. Claude did the reverse
engineering and wrote the C++ plugin code, the crash logger, the diagnostic tools and the automated test harness. Claude
also wrote the documentation, including this page. I did not write the code by hand: I directed the project, chose what to
build, approved each design and playtested the results.

The download contains no AI-generated art, audio, voices or other in-game assets, but the Nexus page thumbnail is
AI-generated. CEF contains no AI model and doesn't connect to any AI service. Test results come from one PC.

## What's here
| Folder | What it is |
|---|---|
| `plugin/fcstruct2` | `FCStruct2.dll`: FaceGen optimizer. Patches game code. |
| `plugin/fcshadow2` | `FCShadow2.dll`: multi-threaded sun-shadow culling. Patches game code. |
| `plugin/cefcrash` | `CEFCrashLog.dll`: crash logger. Patches no game code. |
| `plugin/common` | `cef_version.h`: the exact-game-build check used by FCStruct2 and FCShadow2. |
| `tools` | `analyze_logs.py` and `check_mods.py` (the diagnostic tools), their `.bat` launchers, and `compare_dll.py`. |

**What the version check covers:** FCStruct2 and FCShadow2 patch nothing unless the game is exactly Fallout 4 1.11.240 (F4SE
runtime version, exe build stamp and image size). CEFCrashLog has no such check, because it patches no game code. It installs
an exception handler, and it stands down if Buffout 4 is loaded.

**What's not here:** decompiled game code (it belongs to Bethesda), test captures and logs, the benchmark scripts, and
experiments that never shipped.

## Check that this source matches the Nexus DLLs
SHA-256 of the DLLs in the CEF 1.0.5a download (the same DLLs as 1.0.5):

```text
CEFCrashLog.dll  95b768e913fd4ee0cba7a9a6bbe4aeab3212e69b26556fa97a81111f17e71182
FCShadow2.dll    840a525ad35ef76514a374e221000c4d8284762d1462763b843f8533d649a873
FCStruct2.dll    a102ee6d57ccd028f46ae2734b0e90ecc008285b8abcd8728048cb83354d2136
```

To build, install Visual Studio Build Tools with the MSVC x64 compiler, then run `build.bat` in each plugin folder. The
`.bat` files expect the Build Tools 18 install path, so edit that line if yours differs. The output goes to `build\`.

The Nexus DLLs were built with **MSVC 19.51.36260**. With the same compiler, a rebuild matches them byte for byte except the
build timestamp, which is stored in two places in each file. To check that:

```text
python tools\compare_dll.py <your built DLL> <the DLL from the Nexus download>
```

A different compiler version will produce different bytes without that meaning anything is wrong.

## Credits
- F4SE team (ianpatt, behippo, purple pigeon). The plugin interface declarations follow F4SE's `PluginAPI.h`.
- meh321 for Address Library. CEF reads its database directly.
- CommonLibF4 and Buffout 4 contributors, whose public source helped identify engine structures and showed how to keep a
  crash handler armed. CEF doesn't link or include their libraries.
- Tools: Ghidra, RenderDoc, Intel PresentMon.

## Licence
This source is published for review. It is not open source: see [LICENSE.txt](LICENSE.txt).
