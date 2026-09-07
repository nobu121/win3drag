# WinGet packaging for win3drag

Package ID: **`nobu121.win3drag`**

Publishing a GitHub Release whose asset is named **`3drag.exe`** runs `.github/workflows/winget.yml`, which opens a PR on [microsoft/winget-pkgs](https://github.com/microsoft/winget-pkgs). Users get the new version after that PR merges (`winget upgrade --id nobu121.win3drag`).

`manifests/` in this folder is a local copy / reference. The action builds the next version from the last published manifest in winget-pkgs, so you do **not** need to add a new YAML folder for each release.

## One-time setup

1. Fork https://github.com/microsoft/winget-pkgs as **`nobu121/winget-pkgs`** (already done).
2. Create a **classic** PAT with **`public_repo`** (fine-grained tokens are not supported):  
   https://github.com/settings/tokens/new?scopes=public_repo&description=win3drag-winget
3. Add it as repo secret **`WINGET_TOKEN`**:  
   https://github.com/nobu121/win3drag/settings/secrets/actions

## Release

```bat
build.bat
```

```powershell
gh release create v1.0.2 --title v1.0.2 --notes "..." .\3drag.exe
```

Tag `v1.0.2` becomes WinGet version `1.0.2` (the `v` is stripped). Retry from **Actions → Publish to WinGet → Run workflow** if the first run failed (for example the secret was missing).

## Manual fallback

Copy `manifests/n/nobu121/win3drag/<version>/` into the winget-pkgs fork at the same path, then open a PR titled `New version: nobu121.win3drag version <version>`.

```powershell
winget validate --manifest "packaging\winget\manifests\n\nobu121\win3drag\1.0.1"
```
