# WinGet packaging for win3drag

Package ID: **`nobu121.win3drag`**

Manifests mirror [microsoft/winget-pkgs](https://github.com/microsoft/winget-pkgs) layout under `manifests/n/nobu121/win3drag/<version>/`.

## Before submitting

### 1. Publish a GitHub Release

1. Build: `build.bat` → `3drag.exe`
2. On [releases](https://github.com/nobu121/win3drag/releases), create tag **`v1.0.0`**
3. Upload asset named **`3drag.exe`** (name must match `InstallerUrl`)

### 2. Set SHA256

From repo root (PowerShell):

```powershell
.\packaging\winget\scripts\update-sha256.ps1 -Version 1.0.0 -ExePath .\3drag.exe
```

Or download the release file and hash it:

```powershell
.\packaging\winget\scripts\update-sha256.ps1 -Version 1.0.0 -Url "https://github.com/nobu121/win3drag/releases/download/v1.0.0/3drag.exe"
```

Edit `nobu121.win3drag.installer.yaml` if the script is not used — replace `InstallerSha256` and `ReleaseDate`.

### 3. Validate (optional)

```powershell
winget validate --manifest "packaging\winget\manifests\n\nobu121\win3drag\1.0.0"
```

Local install test (admin once: `winget settings --enable LocalManifestFiles`):

```powershell
winget install --manifest "packaging\winget\manifests\n\nobu121\win3drag\1.0.0"
3drag help
```

## Submit to winget-pkgs

1. Fork https://github.com/microsoft/winget-pkgs
2. Copy folder `packaging/winget/manifests/n/nobu121/win3drag/1.0.0/` to your fork at the **same path**
3. Open PR with title: `New package: nobu121.win3drag version 1.0.0`
4. Fill PR checklist (installer URL reachable, SHA256 matches, etc.)

Docs: [Submitting packages](https://github.com/microsoft/winget-pkgs/blob/master/doc/README.md)

## After merge

Users can install with:

```powershell
winget install --id nobu121.win3drag
```

The manifest uses `InstallerType: portable` with `Commands: [3drag]`. WinGet copies the exe and adds a `3drag` shim under `%LOCALAPPDATA%\Microsoft\WinGet\Links` (on PATH). Users must open a new terminal after install.

Upgrade later: publish `v1.0.1` Release, add `manifests/n/nobu121/win3drag/1.0.1/` (copy 1.0.0, bump version, URL, SHA256), new PR.

## New version checklist

| Step | Action |
|------|--------|
| 1 | Git tag + GitHub Release with `3drag.exe` |
| 2 | New folder `.../win3drag/<version>/` with four YAML files |
| 3 | Update `InstallerUrl`, `InstallerSha256`, `PackageVersion`, `ReleaseDate` |
| 4 | PR to winget-pkgs |
