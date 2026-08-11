# KUAL Native

This is a Java-free replacement for KUAL's menu and action engine.  The old
installer in the original `KUAL.sh` registers `KUALBooklet.jar` with `appmgrd`; that
registration cannot work when the Java/Kindlet runtime is absent, so it is not
used by this implementation.

`kual-native` recursively discovers `/mnt/us/extensions/**/menu.json`, understands KUAL's
`name`, `action`, `params`, `priority`, and nested `items` fields, then executes an action through
`/bin/sh` with the extension directory as its working directory.  It never
uses Java or a JAR.

## Build

For a host parser/action test:

```sh
cmake -S . -B build
cmake --build build
./build/kual-native --root /path/to/extensions --list
```

For Kindle ARM (provide the matching Kindle sysroot):

```sh
cmake -S . -B build-kindle -DCMAKE_TOOLCHAIN_FILE=toolchains/kindle-arm.cmake \
  -DKINDLE_SYSROOT=/path/to/kindle-sysroot -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build build-kindle
```

## Build in GitHub Actions

If your local computer cannot cross-compile, push this repository to GitHub and
open the **Actions** tab. The included
[`Build Kindle Native Launcher`](.github/workflows/build-kindle.yml) workflow
tests the host code, cross-compiles a static ARM EABI binary, and uploads
`KUAL-Native-armel.tar.gz`. Download that artifact and unpack it at the root of
the Kindle USB storage. It supplies both:

```
extensions/KUAL/bin/kual-native
documents/KUAL-Native.sh
```

Publishing a GitHub Release attaches the same tarball to that release. The
workflow deliberately compiles for ARMv5TE soft-float EABI, the conservative
baseline for the practical post-K5 Kindle range.

Install the resulting executable as
`/mnt/us/extensions/KUAL/bin/kual-native`; `scripts/kual-native` is the small
stable launcher intended for the device-native UI integration. Running the
binary without arguments on a Kindle opens the original KUAL-style dark UI:
breadcrumb, ten rounded menu buttons, left/right paging rails, submenu
indicator, final `× Quit`/`/` button, and status line. It renders through
FBInk (not `eips`) and uses Linux evdev for touch and key input.

FBInk must be installed on the Kindle. The launcher automatically tries
`/mnt/us/extensions/KUAL/bin/fbink`, `/mnt/us/libkh/bin/fbink`, KOReader's
copy, and MRInstaller's copy; set `FBINK=/path/to/fbink` to use another one.
The UI uses the stock Futura fonts and batches a complete screen into one
FBInk refresh to avoid per-label flashing.

For current firmware, copy [KUAL-Native.sh](scripts/KUAL-Native.sh) to the
documents area through the same scriptlet mechanism used by your jailbreak,
then select its document entry. It starts the native binary in the background;
the binary keeps control of the e-ink display until the user exits it.

## Important platform boundary

This repository did not include a Kindle 5.19.4 native application entry point
or Amazon's current app-registration ABI. This launcher deliberately avoids
that ABI: a compatible post-jailbreak document scriptlet starts the binary,
and it owns its e-ink screen while open. `--list` and `--run 'Menu/Item'` are
also available for automated launchers. The supplied JAR's original `parse.awk`
presentation flags (`checked`, `refresh`, `status`, `date`, `hidden`, and
`exitmenu`) are represented in the native UI.

## Target scope

The target is post-K5, jailbroken Kindles with a working document-scriptlet
entry point, FBInk, `/dev/fb0`, and Linux evdev input nodes. This covers the
practical newer Kindle family without claiming support for firmware where
Amazon has also removed the scriptlet entry point. The program safely exits if
it cannot open an input device.
