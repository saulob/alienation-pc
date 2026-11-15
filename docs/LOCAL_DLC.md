# DLC installation

Install compatible add-on packages from your own legally obtained copy of the game. No game files, DLC packages or licences are distributed with the port.

## Install

Close any running game. On the launcher Play page choose **Install DLC...** and select an add-on `.pkg`. The package must have CATEGORY `ac` and match the active game's title ID and publisher/service prefix. European CUSA00062 DLC requires game update 1.06. Repeat for other packages, then start or restart the game. DLC remains installed but is labelled inactive if the European game is switched back to 1.00.

The optional manual batch tool accepts one package or a folder of packages, including `.PKG` filenames. It honours `ALN_GAME_BUILD` and prints the selected title and version:

```powershell
cmake --build build/cmake/clang64 --target aln-dlc-install
.\build\cmake\clang64\bin\aln-dlc-install.exe '<port folder>' '<DLC package folder>'
.\build\cmake\clang64\bin\aln-dlc-install.exe '<port folder>' --list
```

`--list` performs read-only discovery and prints the recognised labels and count. The batch tool's running-game guard assumes the standard executable name (`alienation.exe` or `alienation`).

The source package is only read. Extraction stages in a new folder before activation under `game/addcont/<TITLE_ID>/<ENTITLEMENT_LABEL>`. Reinstalling an already valid add-on is a no-op. Existing conflicting or incomplete destinations are never overwritten. Base/update files, settings, saves, shader cache and trophies are not changed by the installer.

Limits are 512 MiB per input package, 256 MiB of extracted payload plus metadata, and 256 add-ons per title. Free space is checked before staging. PSN purchases, downloads, separate RIF-only entitlements and AppContent calls not used by this game are outside this implementation.

## Recover an interrupted install

An interrupted install may leave a `.install-*` staging folder or an invalid destination. Errors and runtime logs identify the path. Close the game and other installers, inspect the folder and move it aside before retrying. Staging cleanup retries transient Windows file locks and reports anything left behind. No automatic stale-folder sweep is performed because another installer may still own it.

Symlinks, junctions, cloud Files-On-Demand placeholders and other reparse points inside the managed add-on subtree are unsupported; use normal local folders there. An owner-controlled junction above the add-on subtree is resolved normally.

## Runtime and validation

The runtime exposes completed add-ons using the 24-byte PS4 add-on-info structure, count-only/bounded enumeration, per-label installed status and read-only mounts. Status 4 follows shadPS4's extracted-package AppContent provider, including packages with empty payloads. A best-effort EntitlementUpdate event is queued once per process for the initial local user; a full queue warns without terminating the game. Diagnostics show the resolved root and skipped candidates.

The suites cover identity checks, incomplete destinations, cancellation before activation, idempotence, Windows junctions, runtime discovery/root normalisation, zero/256 add-ons, a full event queue, and read-only file operations. Run the real-package portion with `tools/test.ps1 -Filter aln-addon-install-tests -DlcPackages '<DLC package folder>'`; without that argument it reports the real-package portion as skipped. The runtime suite is `tools/test.ps1 -Filter aln-content-tests`.

Windows CUSA00062 1.06 startup has been checked with 19 packages containing metadata and no inner payload files: 19 are registered and the game receives installed status for its 17 queried labels. The two bundle identities are registered but not separately queried. Cosmetic selection, weapon use and the actual launcher file-dialog click still require manual verification. Linux gameplay and DLC packages for other regions have not been verified.

For a manual in-game check, inspect an unused character slot for a Veteran Hero starting at level 15 / World Level 2, or the character/loadout colour options for Black, Gold and Chrome armour paints and Lava, Amaranth and Topaz bullets. DLC weapons such as the Devastator and Collapser enter the loot pool rather than appearing immediately in an existing inventory. Existing characters are not automatically promoted.

## Private local builds

Official builds retain the upstream updater. A private build can opt out by configuring with `-DALN_LOCAL_DLC_BUILD=ON`; this creates no updater object and labels the launcher as a local DLC build. The option defaults to OFF. Use the option when replacing the executable with local changes that an official update would otherwise remove.
