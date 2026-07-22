# Keithley 6487 Picoammeter GUI

Cross-platform (Windows / Linux) Qt GUI for real-time current readout from a
Keithley 6487 picoammeter over **GPIB (IEEE-488)** or the instrument's
built-in **RS-232 serial port** — selectable in the GUI. Live scrolling plot
and text-file logging. The instrument driver (SCPI) is included; the RS-232
path needs no third-party software at all, the GPIB path needs only the GPIB
driver stack (NI-488.2 / linux-gpib).

## Features

- **Fast burst mode** — the 6487's fastest readout: fixed range, NPLC 0.01,
  autozero off, display off, readings streamed into the instrument's internal
  buffer (up to 3000 points) and fetched in blocks. Reaches roughly
  **1000 readings/s**, with short gaps (~buffer transfer time) between bursts.
- **Continuous mode** — gap-free `READ?` polling at roughly 50–200 readings/s.
- Live scrolling current-vs-time plot with auto-scaling and pA/nA/µA/mA labels,
  adjustable time window, min/max decimation so high rates stay smooth.
- Logging to a text file: header with start time and settings, then one
  `elapsed_seconds<TAB>current_amps` line per reading, flushed every second.
- Selectable interface: **GPIB** (board index + address, 6487 factory
  default 22) or **RS-232** (port + baud rate). RS-232 needs no GPIB
  hardware or vendor software. Over GPIB, readings transfer in binary
  (SREAL, 4 bytes each; ~1000 rdg/s). Over RS-232 the instrument only
  supports ASCII (the program detects this automatically), sustaining
  roughly 280 rdg/s at 57600 baud / ~50 rdg/s at 9600.
- Timestamps are true sample times: at maximum rate the instrument
  alternates measuring and transferring, and the plot honestly shows
  those acquisition windows (the trace is not drawn across the dead
  time between them). For gap-free operation choose a total rate below
  ~500 rdg/s, e.g. NPLC 1.
- Selectable current range, NPLC, and display refresh interval (default
  0.05 s, adjustable live; in burst mode the burst size is derived
  automatically from the measured reading rate to match it).

## Building

Requirements: CMake ≥ 3.16, a C++17 compiler, Qt 6 (or Qt 5.12+) including
the **Qt SerialPort** module.

Using **RS-232 only**? That's everything — build and go. GPIB support is
detected automatically at configure time: if no GPIB stack is installed, the
program still builds with full RS-232 support and simply reports an error if
the GPIB interface is selected. For GPIB you additionally need:

- **Windows**: [NI-488.2](https://www.ni.com/en/support/downloads/drivers/download.ni-488-2.html)
  (provides `ni4882.h` / `ni4882.lib`).
- **Linux**: [linux-gpib](https://linux-gpib.sourceforge.io/)
  (`libgpib` + `gpib/ib.h`; e.g. build from source or distro packages, and
  configure `/etc/gpib.conf` for your interface board).

### Windows (primary platform)

1. Install **NI-488.2** (free download from ni.com). After installation,
   verify the instrument is visible in **NI MAX** (it should appear under the
   GPIB board, address 22 by default) — if NI MAX can't talk to the 6487,
   nothing else will.
2. Install **Qt** (Qt 6.x, MSVC 64-bit component) via the Qt Online Installer,
   and **Visual Studio** (Community is fine) with the C++ workload, plus CMake.
3. Build from a *x64 Native Tools Command Prompt*:

   ```bat
   cmake -B build -DCMAKE_BUILD_TYPE=Release ^
         -DCMAKE_PREFIX_PATH=C:\Qt\6.7.0\msvc2019_64
   cmake --build build --config Release
   ```

   CMake looks for `ni4882.lib` / `ni4882.h` in NI's standard
   `External Compiler Support` directories automatically; if your NI install
   is somewhere unusual, pass
   `-DGPIB_LIBRARY=<path\to\ni4882.lib> -DGPIB_INCLUDE_DIR=<NI include dir>`.

4. **Deploying to other lab PCs**: copy `pico6487_gui.exe` into a folder and
   run Qt's deployment tool to pull in the needed DLLs:

   ```bat
   C:\Qt\6.7.0\msvc2019_64\bin\windeployqt.exe --release pico6487_gui.exe
   ```

   The target PC only needs NI-488.2 installed (the runtime is enough);
   `ni4882.dll` is provided system-wide by that installer.

### Linux

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/pico6487_gui
```

### Mock build (no hardware / no GPIB stack)

To test the GUI, plotting, and logging without an instrument:

```sh
cmake -B build -DPICO_MOCK_GPIB=ON
cmake --build build
```

The mock backend simulates a 6487 producing a noisy ~1 nA sine signal in both
modes.

## Usage

1. Pick the interface in the Connection box:
   - **GPIB**: set the board index (usually 0) and the instrument address
     (check `MENU` → `GPIB` on the 6487 front panel; factory default 22).
   - **RS-232**: pick the COM port and baud rate. On the 6487 front panel,
     `MENU` → `RS-232`: set the same baud (57600 recommended), 8 data bits,
     no parity, **flow control OFF**. Use a straight-through (not null-modem)
     DB-9 cable — check the manual if unsure; a USB-serial adapter works fine.
2. Pick the mode:
   - *Fast burst* for maximum rate (small gaps between buffer dumps),
   - *Continuous* for gap-free monitoring.
3. Pick a **fixed current range** that covers your signal — autorange is much
   slower and defeats fast mode.
4. Optionally choose the log directory and press **Start**. Each run creates
   a new `pico6487_<date>_<time>.txt` file there (never overwriting previous
   runs); the file currently being written is shown in the Logging box.

Notes:

- The 6487 display is turned off during fast acquisition and restored on stop.
- In burst mode, per-reading timestamps are reconstructed by spreading the
  measured buffer-fill interval evenly across the burst.
- NPLC 0.01 is noisy by nature; increase NPLC if you need lower noise and can
  accept a lower rate.

## Layout

| Path | Purpose |
|---|---|
| `src/GpibInterface.*` | Thin RAII wrapper over NI-488.2 / linux-gpib `ib*` API (+ mock backend) |
| `src/Pico6487Driver.*` | 6487 SCPI driver: configuration, burst and single readout |
| `src/AcquisitionThread.*` | Worker thread running the acquisition loop |
| `src/PlotWidget.*` | Dependency-free scrolling plot widget |
| `src/MainWindow.*` | GUI, controls, and file logging |
