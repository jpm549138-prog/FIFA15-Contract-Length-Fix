# FIFA 15 Contract Length Fix

ASI plugin for FIFA 15 PC that corrects an incorrect contract-duration write affecting players signed by clubs operating on calendar-year seasons.

## Current version

**v1.1 — Calendar Contract**

This is a maintenance update to the original v1.0 contract-duration fix.

## The problem

The FIFA 15 career-mode contract logic can incorrectly reduce the contract duration of newly signed players by one year in calendar-year leagues.

The original v1.0 plugin corrected the issue during normal testing, but high-volume or very fast transfer activity could occasionally allow one or two players to escape the correction.

v1.1 keeps the same correction mechanism while relaxing timing assumptions that were considered too strict under heavy transfer-market activity.

## Changes in v1.1

- Maximum sequence window increased from 250 ms to 2000 ms.
- Same-thread requirement removed from the matching rule.
- Internal state table increased from 256 to 1024 entries.
- Thread breakpoint re-arming interval reduced from 1000 ms to 100 ms.
- Correction log entries now indicate whether the first and second writes occurred on the same thread.

## Safety filters

The correction is applied only when all expected conditions match:

- `players.contractvaliduntil`
- Field offset: `156`
- Depth: `11`
- Writer instruction RVA: `0x2F8CE2E`
- Dispatcher return RVA: `0x2F8382C`
- Packet-caller return RVA: `0x2F8D170`
- Same compact player record
- Same decoded player ID
- First packet mode is non-zero
- Second packet mode is zero
- Second year equals first year minus one

The plugin does **not** patch FIFA 15 executable bytes.

It neutralizes only the second incorrect packed-field write when the expected sequence has been identified.

## Build

Built with:

- Visual Studio 2022
- Configuration: `Release`
- Platform: `x64`

Expected output:

```text
bin\Release\FIFA15ContractFix_v1.1_CalendarContract.asi
```

## Installation

Remove v1.0 and any older `ContractProbe` or `ContractFix` ASI files.

Install only:

```text
FIFA15ContractFix_v1.1_CalendarContract.asi
```

A compatible FIFA 15 ASI/plugin loader is required.

## Log

Runtime log:

```text
%TEMP%\FIFA15_ContractFix_v1.1_CalendarContract.log
```

## Validation

Recommended validation is a high-volume transfer-window test similar to the scenario that exposed the occasional v1.0 slip:

- complete several paid transfers and/or Free Agent signings in one career session;
- perform transfers at normal or fast pace;
- verify the final contract durations.

If a player still escapes the correction, preserve the log together with the player's name and player ID.

## Repository contents

- `FIFA15ContractProbe.cpp` — plugin source
- `FIFA15ContractProbe.vcxproj` — Visual Studio project
- `FIFA15ContractProbe.sln` — Visual Studio solution
- `PLUGIN_README.txt` — original technical notes for v1.1

## Status

Runtime-tested maintenance release.

The source is preserved here for documentation, reproducibility and future development.

## Credits

Special thanks to **Dmitri** for creating and documenting the FIFA 15 Plugin Loader. This project relies on that loader to inject and run the custom `.asi` plugin in FIFA 15.

Original Plugin Loader thread:
https://soccergaming.com/forums/threads/fifa-15-plugin-loader.6475283/
