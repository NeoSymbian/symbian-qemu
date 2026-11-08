# symbian-qemu

The Symbian Foundation's QEMU package (`oss.FCL.sf.adapt.Qemu`) brought to a current QEMU. It runs the 2010
Symbian user side on the **Syborg** board, the virtual phone the Foundation made for QEMU, and it is where
NeoSymbian tests its Rust user side and UI.

| Path | What |
|---|---|
| `baseport/syborg/` | The Symbian board support package for Syborg (EPL). The ROM build reads it from here. |
| `qemu/` | QEMU (a pinned release, imported with `git subtree`) plus the Syborg board in C: `hw/arm/syborg*.c`, `hw/display/syborg_fb.c`, `tests/qtest/syborg-test.c`. |
| `run-syborg` | Boots a ROM and drives it from a script, watching the serial log instead of sleeping. |
| `syborg-view.py` | A live window (Python + Tk only) for a `LIVE=1` run: shows the screen, sends keys and touches. |
| `container/Dockerfile` | `ghcr.io/neosymbian/symbian-qemu`: QEMU (`qemu-system-arm`, `qemu-system-aarch64`) and `run-syborg` on Debian. |
| `docs/`, `tools/`, `applications/` | The Foundation's documentation, test driver and ELF4ROM, as released. |

The Foundation's own QEMU 0.9.1 fork (`symbian-qemu-0.9.1-12/`) is in the history and on the `master` branch.

## Running a ROM

```sh
docker run --rm -v "$PWD:/work" -e C_DRIVE=/work/c_drive.img -e RUN=neohello,neotest \
    ghcr.io/neosymbian/symbian-qemu run-syborg /work/minigui.rom 300
```

`run-syborg` waits until ESHELL (or the NeoSymbian UI) is up, types the `RUN` commands, sends `MONITOR` commands
(QEMU monitor lines such as `mouse_move 16384 16384;mouse_button 1;mouse_button 0`), and stops when the script is
done. The serial output goes to `serial.log` and the screen to `fb.ppm`. `SCREEN=1080x2160` sets the screen size
and `SNAPSHOT=/work/snap` keeps the booted machine, so later runs of the same ROM start in about a second.
`run-syborg --help` lists everything.

With `LIVE=1` it runs in the background: VNC on port 5900 (any VNC viewer shows the screen and takes keys and
touches), the QEMU monitor on port 4444. Without a VNC viewer, `python3 syborg-view.py fb.ppm` opens a window on the
screen dump and sends keys and touches through the monitor.

Directly with QEMU: `qemu-system-arm -M syborg,width=768,height=640 -kernel minigui.rom
-drive if=mtd,format=raw,file=c_drive.img -serial stdio`. Add `hostfs=DIR` to the `-M` options to give the
guest a host folder as its E: drive (the ROM needs `svphostfsdriver.ldd`, `svphostfsy.fsy` and `svphostfs.exe`,
which mount it at start-up); the guest cannot reach outside DIR.

## Building QEMU

```sh
mkdir build && cd build
../qemu/configure --target-list=arm-softmmu,aarch64-softmmu
ninja qemu-system-arm tests/qtest/syborg-test
QTEST_QEMU_BINARY=./qemu-system-arm tests/qtest/syborg-test
```

This works on Linux and on macOS (Apple Silicon included; `brew install glib pixman ninja pkgconf dtc`). On macOS
add `--enable-cocoa` for a window.

## Moving to a newer QEMU

```sh
git fetch --depth 1 https://gitlab.com/qemu-project/qemu.git tag vX.Y.Z
git subtree pull --prefix qemu --squash FETCH_HEAD -m "qemu/: QEMU vX.Y.Z"
```

Our changes to QEMU are ordinary commits under `qemu/`, so conflicts are fixed in place. Then build, run the qtest,
and boot a ROM.

## Licences

The baseport and the Foundation's files: EPL-1.0 (the files we change: EPL-2.0). QEMU: GPL-2.0. The Syborg board
files carry CodeSourcery's MIT licence, which they started under. `run-syborg`, the container and the workflows:
Apache-2.0. `syborg-view.py`: GPL-2.0, as published in NeoSymbian/qemu-syborg.
