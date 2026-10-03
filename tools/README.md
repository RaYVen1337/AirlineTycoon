# Multiplayer test tools (Windows)

Headless host + client on 127.0.0.1, both players auto-played. Used to find and verify the fix for issue #29
(crash when a damaged network message was applied).

| Script | Purpose |
|---|---|
| `build.ps1 [-Asan]` | Configure + build with MSVC/Ninja into `build` or `build-asan` |
| `nettest.ps1` | Run a host/client pair, logs in `<BuildDir>\nettest` |
| `dbgrun.ps1` | Same under cdb (WinDbg), prints the crash stack of each side |
| `nettest-round.ps1` | Build, run `dbgrun.ps1`, watch for crash/stall, print the stacks |
| `nettest-watch.ps1` | Used by `nettest-round.ps1` |

Quick check (one game day takes ~5 minutes):

```powershell
powershell -ExecutionPolicy Bypass -File tools\nettest-round.ps1 -Fuzz 50 -Turbo 4 -Days 1
```

Command line switches of `AT.exe`:

- `/nettest host <port>` / `/nettest client <host> <port>`: drive the menus and start a Direct-IP game without a human
- `/nettestdays N`: exit with code 0 after N days
- `/nettestmonkey N`: random walking/clicking of the human player (seed N). Negative: host stays in the office,
  client hovers over the Last Minute notes (the scenario of issue #29)
- `/nettestturbo N`: game time runs N times faster
- `/nettestfuzz N`: every received game message is replayed N times with random damage.
  N > 0 damages the frame including the checksum trailer (all copies must be dropped),
  N < 0 damages the message behind the checksum test (tests the message handlers)

Only one pair can run at a time (fixed port). `dbgrun.ps1` needs `cdbX64.exe` in PATH.
