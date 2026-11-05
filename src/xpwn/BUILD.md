# Building xpwn

## Dependencies

### Linux (Ubuntu/Debian)
```bash
sudo apt update
sudo apt install -y cmake build-essential pkg-config \
    libssl-dev libpng-dev zlib1g-dev libbz2-dev \
    libusb-1.0-0-dev
```

### macOS (using MacPorts)
```bash
sudo port install cmake openssl libpng bzip2
```

### Windows (MSYS2)
```bash
pacman -S cmake gcc make pkg-config openssl-devel libpng-devel zlib-devel bzip2-devel
```

## Build Instructions

```bash
cd src/xpwn
mkdir build && cd build
cmake ..
make
```

## Output Binaries

The following binaries will be built in the `build/` directory:

| Binary | Description |
|--------|-------------|
| `ipsw` | IPSW tool for firmware manipulation |
| `xpwntool` | Core xpwn tool for patching/exploiting |
| `imagetool` | Image manipulation tool |
| `validate` | Ticket/validation tool |
| `ticket` | Ticket generation tool |
| `dmg` | Apple DMG utility |
| `hdutil` | Apple disk image utility |
| `hfsplus` | Apple HFS+ utility |

## Static Build (Linux/Windows)

For fully static binaries (no runtime dependencies):

```bash
cmake -DBUILD_STATIC=ON ..
make
```

## Cross-compilation for Windows (from Linux)

```bash
cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/Toolchain-mingw64.cmake ..
make
```

*(Requires mingw-w64 toolchain installed)*

## Notes

- Minimum CMake version: 3.5
- The `xpwntool` and `ipsw` binaries are the primary tools for iOS firmware exploitation
- Firmware bundles are installed to `FirmwareBundles/` and `bundles/` directories