# Streets of Rogue Offsets Dumper

A dynamic offsets dumper for *Streets of Rogue*, designed to extract field offsets from the game's Mono (Unity) runtime.

## Overview

This project provides a DLL-based dumper that scans the `Assembly-CSharp` module using Mono runtime functions and extracts class field offsets.

The dumper generates output in multiple formats for convenient use in different programming environments.

## Features

* Dumps offsets directly from the game's Mono runtime
* Supports multiple output formats:

  * `.txt`
  * `.json`
  * `.hpp`
  * `.zig`
  * `.rs`
  * `.cs`
* Includes inherited fields (base classes)
* Automatically resolves nested classes
* Generates timestamped output

## Usage

1. Download or compile `payload.dll` from the source code.
2. Launch *Streets of Rogue* (`StreetsOfRogue.exe`).
3. Inject the DLL into the game process using a standard injection method:

   * Example: `CreateRemoteThread + LoadLibraryW`
   * Or use any existing DLL injector
4. Ensure the game is fully initialized (main menu or in-game - both are fine).

After successful injection:

* The dumper will automatically execute
* Output files will be generated in:

```
%USERPROFILE%\Downloads\SOR Output
```

* A Windows message box will appear confirming completion

## How It Works

The dumper attaches to the Mono runtime used by the Unity engine and:

* Iterates through loaded assemblies
* Targets `Assembly-CSharp`
* Enumerates all classes and fields
* Extracts:

  * Field names
  * Types
  * Offsets
  * Static flags
  * Inheritance information

## Output Formats

The dumper generates the following files:

* `sor_dump.txt` - human-readable dump
* `sor_dump.json` - structured data
* `sor_offsets.hpp` - C++ header
* `sor_offsets.zig` - Zig definitions
* `sor_offsets.rs` - Rust module
* `SorOffsets.cs` - C# class

## Requirements (Runtime)

* Windows 10 / 11 (x64)

## Requirements (Build)

* C++20 compatible compiler
  (e.g. MSVC 19.3x / Visual Studio 2022 or newer)
* Windows SDK
* Standard Windows libraries (`Windows.h`, `ShlObj.h`)

## Notes

* The dumper is implemented as a DLL and must be injected into the target process
* Injection must be performed after the game has initialized Mono
* The dumper waits for the Mono module (`mono-2.0-bdwgc.dll` or `mono.dll`) before execution

## License

Licensed under the MIT License (see `LICENSE`).
