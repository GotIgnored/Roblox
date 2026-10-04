# Roblox Multi-Instance Unlocker

A small Windows command-line tool that lets you run **multiple Roblox clients at
once** on one PC, so alt accounts don't each need a separate device.

Roblox only allows one client at a time by holding a named Event object
(`ROBLOX_singletonEvent`). This tool finds every handle to that object and closes
it, which destroys it. The next launch creates a fresh one, so clients can run
side by side.

It only closes Roblox's own single-instance guard object. It does not read or
write Roblox memory, inject code, or touch the game.

## Usage (no C++ tools needed)

1. Download `roblox_multi.exe` from this repo (prebuilt, no dependencies).
2. Launch Roblox normally.
3. Right-click `roblox_multi.exe` → **Run as administrator** (needs admin to
   access the handle; you'll get a UAC prompt automatically).
4. When it says so, press any key. On success:

   ```
   You can now launch multiple Roblox instances.
   ```

5. Launch your next client. Run the tool again before each extra launch.

**Antivirus note:** tools that close system handles are sometimes flagged by AV.
The full source is here so you can read it or build it yourself. If Defender
removes the exe, add an exclusion for it.

## Building it yourself (optional)

Double-click `build.bat` (or run it in a terminal). It auto-detects MSVC or
MinGW, builds, embeds the admin manifest, and stays open so you can read the
result.

Manual (MinGW / MSYS2 ucrt64):

```bash
echo '1 24 "app.manifest"' > manifest.rc
windres manifest.rc -O coff -o manifest.res
g++ -std=c++17 -O2 -static roblox_multi.cpp manifest.res -o roblox_multi.exe -lntdll
```

`-static` gives a single portable exe with no DLL dependencies.

## Files

| File | What it is |
|------|------------|
| `roblox_multi.exe` | Prebuilt, ready-to-run binary |
| `roblox_multi.cpp` | Source |
| `app.manifest` | Requests admin elevation (UAC) |
| `build.bat` | Build script (optional) |

## License

MIT — see [LICENSE](LICENSE).
