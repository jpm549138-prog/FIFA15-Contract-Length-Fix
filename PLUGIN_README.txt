FIFA 15 Contract Fix v1.1 - Calendar Contract

Purpose
-------
Maintenance update for the v1.0 contract-duration fix.

Why v1.1 exists
---------------
v1.0 fixed the bug in normal testing, but a high-volume / fast-user signing
session could let one or two players escape. The v1.0 log showed the plugin
loaded and armed correctly and applied several corrections, but the reduced
release log did not contain enough diagnostic detail to prove exactly which
filter missed the escaped players.

v1.1 keeps the same correction mechanism but relaxes the timing assumptions
that were most likely too strict under high-volume market operation.

Changes from v1.0
-----------------
- Maximum sequence window increased from 250 ms to 2000 ms.
- Same-thread requirement removed from the matching rule.
- State table increased from 256 to 1024 entries.
- Thread breakpoint re-arming interval reduced from 1000 ms to 100 ms.
- Log still remains compact, but correction lines include whether the
  first and second writes occurred on the same thread.

Core safety filters retained
----------------------------
All must still match:
- players.contractvaliduntil, offset 156, depth 11;
- writer instruction RVA 0x2F8CE2E;
- dispatcher return RVA 0x2F8382C;
- packet-caller return RVA 0x2F8D170;
- same compact player record;
- same decoded player ID;
- first packet mode non-zero;
- second packet mode zero;
- second year = first year - 1.

The plugin does not patch executable bytes. It neutralizes only the second,
incorrect packed-field write.

Build
-----
Visual Studio 2022
Release | x64

Expected output
---------------
bin\Release\FIFA15ContractFix_v1.1_CalendarContract.asi

Installation
------------
Remove v1.0 and all older ContractProbe / ContractFix ASI files.
Install only FIFA15ContractFix_v1.1_CalendarContract.asi.

Log
---
%TEMP%\FIFA15_ContractFix_v1.1_CalendarContract.log

Recommended validation
----------------------
Run one high-volume transfer-window test, similar to the scenario that exposed
the v1.0 slip:
- several paid transfers and/or Free Agents in one career session;
- complete them at normal or fast pace;
- check final contract durations.

If any player still slips, preserve the log and the names/player IDs.
