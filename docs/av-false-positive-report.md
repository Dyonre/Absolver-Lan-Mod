# Antivirus false-positive report: LanNative `version.dll`

For the vendors that flagged it (ClamAV, Microsoft, Symantec, McAfee, Cynet). Send it yourself (each vendor has a "submit a false positive" form); nothing here has been submitted.

**File:** `version.dll` (LanNative loader), SHA-256 `8C8BEF72B00FF27D3B12485FA5166D7338F58E8762E5CB399FA1575FD89585D5`, 202,240 bytes (M3.7.1).
Earlier build that VirusTotal scanned: SHA-256 `FB2FCA9E9778F5B6BAE9D4E64F5916B6204BC4B34E7FCBEB5D226BA281C95130`, 199,680 bytes (M3.7), zip `02D606937C7A396EC7EC6BC1FA2DBA9766C72AF217B96A46564BCC76C3229C6C`.
Detections seen on VirusTotal for the M3.7 DLL (5/71): ClamAV `Win.Backdoor.Sagerunex-10041857-1`, Microsoft `Trojan:Win32/Wacatac.C!ml`, Symantec `ML.Attribute.HighConfidence`, Cynet `Malicious (score: 100)`, McAfee `Ti!FB2FCA9E9778`. Four of the five are machine-learning/reputation verdicts, not named signatures.

**What it is.** A hobby mod for the 2018 game Absolver (Sloclap, Steam build 1.31). The game loads a `version.dll` from its own folder; LanNative is installed under that name, forwards every export to `C:\Windows\System32\version.dll`, and adds LAN multiplayer fixes by patching the game's own functions in memory. It does nothing unless the game is started with `-NoEAC` (offline play) and does nothing outside `Absolver-Win64-Shipping.exe`.

**Why it can look suspicious (all by design):** it is a proxy DLL (the same shape DLL side-loading uses); it patches code in the game process (`VirtualAlloc`, `VirtualProtect`, `FlushInstructionCache`); it polls hotkeys (`GetAsyncKeyState`, `GetForegroundWindow`, only to act on F3/F5/F6/F7/F8 while the game window is focused); it is new, rare and unsigned.

**What it does not do (checkable):** the import table has only `USER32.dll` (3 functions) and `KERNEL32.dll`. No network DLL (no ws2_32/wininet/winhttp), no registry/advapi32, no `CreateProcess`, no `OpenProcess`/`WriteProcessMemory`/`CreateRemoteThread`. Its only file output is a log and CSV files in a `LanNative` folder next to it. Windows Defender on the author's PC has never flagged it. The VirusTotal scan of the whole zip (nine files) was 1/66 (ClamAV only).

**Request:** please review and whitelist/remove the detection for the hashes above. The author can provide the complete source code and a build on request.