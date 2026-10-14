# puddle

Easiest self hosted chat application conceivable.

## Building

Two programs, built from one CMake project:

- `puddled` — headless service (C, OpenSSL)
- `puddle-gui` — desktop app (C core + Qt 6 Widgets)

### Requirements

CMake 3.21+, a C11/C++17 compiler, OpenSSL 3.0+, and (for the GUI) Qt 6 Widgets.

macOS (Homebrew):

    brew install cmake openssl@3 qt

Debian/Ubuntu:

    sudo apt install build-essential cmake libssl-dev qt6-base-dev

Fedora:

    sudo dnf install gcc gcc-c++ cmake openssl-devel qt6-qtbase-devel

### Build and test

    cmake -S . -B build
    cmake --build build
    ctest --test-dir build

Headless host with no Qt: add `-DPUDDLE_BUILD_GUI=OFF` to the first command.
If CMake picks the wrong OpenSSL (macOS ships LibreSSL), pass
`-DOPENSSL_ROOT_DIR=/path/to/openssl@3`. If Qt isn't found, add its prefix with
`-DCMAKE_PREFIX_PATH=/path/to/qt`.

Outputs: `build/puddled` and `build/puddle-gui` (`build/puddle-gui.app` on macOS).

### Run

    ./build/puddled                  # or ./build/puddle-gui

On first run `~/.config/puddle/puddle.conf` is created (Windows: `%APPDATA%\puddle\`)
with `address = 127.0.0.1` and `port = 8443`, along with a self-signed `cert.pem` /
`key.pem` in the same folder. Edit the config and restart to change the listener
(`address = ::` listens on all IPv4 and IPv6 interfaces).

    curl --cacert ~/.config/puddle/cert.pem https://localhost:8443/health

### Run as a systemd user service

    install -Dm755 build/puddled ~/.local/bin/puddled
    install -Dm644 dist/puddled.service ~/.config/systemd/user/puddled.service
    systemctl --user daemon-reload
    systemctl --user enable --now puddled
    journalctl --user -u puddled -f

To keep it running when you're logged out: `loginctl enable-linger $USER`.

The server code is POSIX-only for now; Windows builds are not yet supported.

## Releases

Pushing a tag like `0.0.0` (no `v`) builds Linux amd64/arm64 (`puddled`) and
macOS arm64/amd64 (`puddled` + `puddle-gui.app`) and publishes them, with `SHA256SUMS`,
to the GitHub release page. A tag containing `-` (e.g. `0.1.0-rc1`) is marked a pre-release.

    git tag 0.0.0 && git push origin 0.0.0

Pushes to `main` and pull requests run the same builds without publishing.
The macOS app is not notarized; after downloading, clear the quarantine flag
with `xattr -dr com.apple.quarantine puddle-gui.app`.
