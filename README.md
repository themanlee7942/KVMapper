# KVMapper

[![CI](https://github.com/themanlee7942/KVMapper/actions/workflows/ci.yml/badge.svg)](https://github.com/themanlee7942/KVMapper/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/themanlee7942/KVMapper?include_prereleases&sort=semver)](https://github.com/themanlee7942/KVMapper/releases/latest)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Platform: Windows](https://img.shields.io/badge/platform-Windows%2010%20%7C%2011-blue)](#install)
[![Regression archaeology](https://img.shields.io/github/directory-file-count/themanlee7942/KVMapper/tests/regression?type=file&extension=md&label=regression%20notes&color=blueviolet)](tests/regression/)

**KVM-aware hotkey mapper for Windows.** Remaps keys that AutoHotkey, PowerToys, and SharpKeys can't see — hardware KVM switches, IP-KVMs (PiKVM, IPMI/BMC), Synergy / Barrier / Mouse Without Borders, and RDP sessions.

KVMapper uses a `WH_KEYBOARD` hook injected into every GUI process, so it sees keystrokes *after* they've been delivered to the focused application. That's the only layer where KVM- and remote-injected keys are visible and modifiable. v0.10 adds a `WH_KEYBOARD_LL` classifier running in the controller process that publishes per-event verdicts (LLKHF_INJECTED, session-token tag) into shared memory, letting the DLL distinguish local vs remote vs own-SendInput events with a single 16-byte read.

## What it does

- Define keyboard combos (trigger) → keyboard actions (output) mappings.
- Fires on key-press OR key-release (configurable per rule).
- Action types: press+release (tap), press-only (hold), release-only.
- Source filter per rule: `ANY` (default), `LOCAL` (real HID only), `REMOTE` (KVM/RDP/injected only).
- Mappings live in a human-readable text file next to the exe.
- Single tray app, no service, no installer, no admin rights.

Example mappings:

```
RWin+RAlt        ->  F1                 (on press)       any
RCtrl+RAlt       ->  Ctrl+C             (on release)     remote-only
Pause            ->  ScrollLock         (on press)       any
```

## <a name="install"></a>Install

Pre-built binaries are published automatically on every release tag — grab them from the **[Releases page](https://github.com/themanlee7942/KVMapper/releases)**:

- **64-bit Windows** → `kvmapper-vX.Y-x64.zip`
- **32-bit Windows** → `kvmapper-vX.Y-x86.zip`
- `SHA256SUMS.txt` for checksum verification

Then:

1. Unzip anywhere — the binaries are portable, no installer needed.
2. Run `kvmapper.exe`. It minimises to the tray.

Verify your download against the published hashes:

```
certutil -hashfile kvmapper-v0.9-x64.zip SHA256
```

The first release is **v0.9**. CI/CD pipeline (`.github/workflows/release.yml`) cross-compiles both architectures from a clean Ubuntu runner via zig, smoke-tests the produced PE files, packages per-arch zips with SHA256, and attaches them to the Release. To cut a new release: `git tag v0.X && git push --tags`.

## Use

1. Launch `kvmapper.exe`. The main window shows the active mappings (empty on first run).
2. Click **+ Create Hotkey**.
3. Click **Record Trigger**, press your combo (e.g. RWin + RAlt), release all keys. The combo is captured. Or just type the VK names directly into the trigger field (e.g. `ALT+F1`).
4. Choose **Pressed** or **Released** dispatch.
5. Click **Record Action** or type names directly (e.g. `HANGUL`).
6. Optional: type a **Label** for your own reference.
7. Click **Save**. The rule is live immediately; no restart.

**Test area** at the bottom of the main window: click in, type, and watch your mappings fire in real time — no need to alt-tab to Notepad. Hit **Clear** to wipe it.

Double-click a row in the list to toggle enable/disable. Use **Edit** or **Delete** to modify.

The rule file is `kvmapper_mappings.txt` next to the exe — readable, editable while KVMapper is **stopped** (use the tray menu → Exit, edit, restart). Source filter (column 7) is hand-editable; the UI radio is coming in a future version.

### Supported VK names

All names below come from Windows' official `VK_*` macros (`winuser.h`). They are case-insensitive in the editor.

**Modifiers**

| Name | VK | Notes |
|---|---|---|
| `LWIN` / `RWIN` / `WIN` | 0x5B / 0x5C | left/right/either Win key |
| `LALT` / `RALT` / `ALT` | 0xA4 / 0xA5 / 0x12 | `ALT` matches either L or R |
| `LCTRL` / `RCTRL` / `CTRL` | 0xA2 / 0xA3 / 0x11 | `CTRL` matches either L or R |
| `LSHIFT` / `RSHIFT` / `SHIFT` | 0xA0 / 0xA1 / 0x10 | `SHIFT` matches either L or R |

**Letters, digits, function keys**

`A` … `Z`, `0` … `9`, `F1` … `F24`.

**Navigation / editing**

`ESC` `TAB` `SPACE` `ENTER` (`RETURN`) `BACK` (`BACKSPACE`)
`INS` (`INSERT`) `DEL` (`DELETE`) `HOME` `END` `PGUP` (`PAGEUP`) `PGDN` (`PAGEDOWN`)
`UP` `DOWN` `LEFT` `RIGHT`
`PAUSE` `SCROLL` `CAPS` (`CAPSLOCK`) `NUMLOCK` `PRTSC` (`PRINTSCREEN`) `APPS`

**Numpad** (`NUM0` … `NUM9`, `NUMADD`, `NUMSUB`, `NUMMUL`, `NUMDIV`, `NUMDOT`)

**OEM punctuation** (`SEMICOLON` `SLASH` `BACKTICK` `LBRACKET` `BACKSLASH` `RBRACKET` `QUOTE` `COMMA` `PERIOD` `MINUS` `PLUS`)

**IME / East Asian language keys** — these are official Windows VKs:

| Name | VK | Effect |
|---|---|---|
| `HANGUL` / `KANA` | 0x15 | Toggle the active IME (Korean Hangul ↔ English, Japanese Kana ↔ ASCII). Same VK for both — Windows treats them as aliases. |
| `HANJA` / `KANJI` | 0x19 | IME conversion (Hanja for Korean, Kanji for Japanese). Same VK. |
| `IME_ON` | 0x16 | Force IME on (Windows 10+) |
| `IME_OFF` | 0x1A | Force IME off (Windows 10+) |
| `CONVERT` | 0x1C | Japanese convert |
| `NONCONVERT` | 0x1D | Japanese non-convert |

**Multimedia / system**

| Name | VK |
|---|---|
| `VOLUME_MUTE` / `VOLUME_DOWN` / `VOLUME_UP` | 0xAD / 0xAE / 0xAF |
| `MEDIA_PLAY_PAUSE` / `MEDIA_STOP` / `MEDIA_NEXT_TRACK` / `MEDIA_PREV_TRACK` | 0xB3 / 0xB2 / 0xB0 / 0xB1 |
| `SLEEP` | 0x5F |

**Browser**

| Name | VK |
|---|---|
| `BROWSER_BACK` / `BROWSER_FORWARD` | 0xA6 / 0xA7 |
| `BROWSER_REFRESH` / `BROWSER_STOP` | 0xA8 / 0xA9 |
| `BROWSER_SEARCH` / `BROWSER_FAVORITES` / `BROWSER_HOME` | 0xAA / 0xAB / 0xAC |

**App-launch keys** (the e-mail / media / app keys on multimedia keyboards)

| Name | VK |
|---|---|
| `LAUNCH_MAIL` / `LAUNCH_MEDIA_SELECT` | 0xB4 / 0xB5 |
| `LAUNCH_APP1` / `LAUNCH_APP2` | 0xB6 / 0xB7 |

**Other** (`CLEAR` `SELECT` `PRINT` `EXECUTE` `HELP`)

**Raw hex** — for any VK that isn't named here, use `0xNN` (e.g. `0x70` for F1).

**Combining keys** — use `+` between names, up to 4 keys per trigger/action: `CTRL+SHIFT+A`, `RWIN+RALT`, `LCTRL+VOLUME_UP`.

### Recipe — map Alt → Hangul/IME toggle (for switching languages from any keyboard)

```
Trigger:     ALT          (or RALT if your KVM distinguishes L/R)
Action:      HANGUL       (or KANA - same VK, locale-agnostic)
Dispatch:    Pressed
Action type: Press+Release
```

## Command-line flags

| Flag | Behaviour |
|---|---|
| *(none)* | Open UI, install tray icon. |
| `/tray` | Start minimised to tray. Useful for autostart. |
| `/stop` | Signal a running KVMapper instance to exit. |

For autostart, drop a shortcut to `kvmapper.exe /tray` into `shell:startup`.

## Build from source

KVMapper builds from Linux, macOS, or Windows with the same toolchain:

```sh
pip install ziglang
./build-all.sh
```

This cross-compiles `dist/kvmapper.exe` (x64), `dist/kvmapper_x86.exe` (x86), and both `kvmapper_hook.dll` flavours. Smoke-test the output:

```sh
python3 tests/verify_dist.py        # PE structural checks
tests/native/run.sh                 # 31 unit tests + 10k fuzz under ASan+UBSan
```

On a Windows machine with mingw-w64 installed, `build.bat` does the same thing.

### Project layout

```
KVMapper/
├── .github/
│   ├── PULL_REQUEST_TEMPLATE.md   - regression-aware PR checklist
│   └── workflows/
│       ├── ci.yml                  - CI on push/PR
│       ├── release.yml             - tag-triggered Release with zips + SHA256
│       ├── regression-label.yml    - auto-label PRs touching tests/regression/
│       └── stop-sign-scanner.yml   - warn on removal of guarded patterns
├── src/
│   ├── main.c                      - WinMain, UI, tray, mapping editor
│   ├── capture.c                   - WH_KEYBOARD_LL key-capture for the editor
│   ├── config.c                    - parse/write kvmapper_mappings.txt
│   ├── shared_mem.c                - named file mapping IPC, session token
│   ├── classifier.c                - LL classifier thread, verdict ring writer
│   └── hook/
│       ├── hook.c                  - DllMain, install_hook, KeyboardProc
│       └── inject.c                - SendInput helpers, token stamp
├── include/
│   ├── mapping_defs.h              - shared structs (exe <-> DLL)
│   └── icon_data.h                 - embedded fallback icon
├── tests/
│   ├── verify_dist.py              - PE smoke test (gate before release)
│   ├── native/
│   │   ├── run.sh                  - unit + fuzz runner
│   │   ├── test_config.c           - 31 unit tests
│   │   ├── fuzz_config.c           - 10k seed fuzzer (ASan + UBSan)
│   │   └── win32_shim.h            - minimal Win32 types for Linux native build
│   └── regression/                 - bug archaeology (see badge above)
├── skills/                         - design references used to build this
│   ├── windows-dll-input-hook/SKILL.md
│   └── windows-native-cicd/SKILL.md
├── assets/tray.ico
├── app.rc / app.manifest
├── build-all.sh                    - cross-compile via zig
├── build.bat                       - native Windows build (mingw or zig)
├── Makefile                        - native mingw build
└── plan.md                         - original design doc
```

## Architecture

```
[User presses RWin + RAlt via KVM]
        |
        v
[WH_KEYBOARD_LL classifier (kvmapper.exe)]
   - LLKHF_INJECTED?  -> mark "remote"
   - dwExtraInfo == our session token? -> mark "ours"
   - Write 16-byte verdict to ring[vk] in shared memory
        |
        v
[WH_KEYBOARD in focused process (kvmapper_hook.dll)]
   - Read ring[vk] verdict (single 16-byte aligned read)
   - If isOurs: pass through (re-entrancy)
   - Check sourceFilter: skip rule if mismatch
   - Match combo, SendInput action keys (tagged with token)
        |
        v
[Target window receives remapped keystroke]
```

The exe creates a named file mapping (`kvmapper_shared_v1`) holding the mapping table, a per-launch session token (PID + rand32), and a 256-slot per-VK verdict table. Each injected DLL instance opens it read-only. Writers and readers are unsynchronised by a release fence + fingerprint check, not by locks. No locks, no I/O on the hot path. See `plan.md` for the original design and `tests/regression/` for the v0.10 fixes that made it correct.

## Limitations (v0.10)

- UI radio for `sourceFilter` is hand-editable text only; the radio button comes in v0.11.
- 32-bit and 64-bit DLLs are shipped, but only the matching exe installs the hook for its own bitness. To cover BOTH 32-bit and 64-bit GUI apps you run both `kvmapper.exe` and `kvmapper_x86.exe`. v1 may launch the x86 helper automatically.
- No macro sequences — output is a single combo, not a typed phrase.
- No per-process rules (next on the roadmap; `GetForegroundWindow` is the hook).
- No installer; ship as a zip.

## Why KVMapper exists

If you've tried AutoHotkey or PowerToys Keyboard Manager through a KVM and watched `RWin` get eaten while `LWin` works fine, this is for you. The full story is in [`plan.md`](plan.md) — what every other tool does, why it breaks on KVM/RDP, and the one Windows API surface (`WH_KEYBOARD` DLL injection) that survives those layers. The bugs we hit and fixed along the way are catalogued in [`tests/regression/`](tests/regression/).

## License

MIT — see [LICENSE](LICENSE).
