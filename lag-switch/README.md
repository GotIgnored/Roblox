# Lag Switch

A lightweight Windows lag switch that cuts your connection on a key press by
toggling a **Windows Firewall outbound block rule**.

## Why not `ipconfig /release` / `/renew`?

That tears down the whole DHCP lease: it's slow (2–5s), drops *every*
connection, hits the router's DHCP server, and can even hand you a new IP.

A firewall rule is the clean way — flipping it on/off is **instant and purely
local**, so the router never sees a thing. This tool creates one disabled
block-outbound rule and just enables/disables it.

## Usage (no C++ tools needed)

1. Download `lagswitch.exe` from this repo (prebuilt, no dependencies).
2. Right-click → **Run as administrator** (creating firewall rules needs admin).
3. At the menu, **pick a key** (single character) and an **auto-restore timeout**
   in seconds (recommended 9).
4. **Hold** your key → outbound traffic is cut. It restores on whichever comes
   first: you **release** the key, or the **timer** expires. (After an
   auto-restore while you keep holding, it won't cut again until you let go and
   press once more.)
5. Quit with **Ctrl+C** or by closing the window — it auto-restores your
   connection and removes the rule.

The bound key is captured globally while the tool runs, so it won't type into
other apps.

## Safety

While active, **all outbound traffic is blocked**. The tool removes its rule
when you quit and also clears any leftover rule on startup, so a crash can't
leave you offline for good — just relaunch to recover. Manual recovery if ever
needed:

```
netsh advfirewall firewall delete rule name="LagSwitch_BlockOutbound"
```

**Antivirus note:** network/firewall tools are sometimes flagged by AV. The
source is here so you can read it or build it yourself; add an exclusion if the
exe gets removed.

## Building it yourself (optional)

Double-click `build.bat`. It auto-detects MSVC or MinGW, builds, embeds the
admin manifest, and stays open so you can read the result.

Manual (MinGW / MSYS2 ucrt64):

```bash
echo '1 24 "app.manifest"' > manifest.rc
windres manifest.rc -O coff -o manifest.res
g++ -std=c++17 -O2 -static lagswitch.cpp manifest.res -o lagswitch.exe -lole32 -loleaut32 -luuid
```

## Files

| File | What it is |
|------|------------|
| `lagswitch.exe` | Prebuilt, ready-to-run binary |
| `lagswitch.cpp` | Source |
| `app.manifest` | Requests admin elevation (UAC) |
| `build.bat` | Build script (optional) |

## License

MIT — see [LICENSE](LICENSE).
