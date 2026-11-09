// Updates from GitHub releases, verified and with automatic rollback.
//
// Each release carries htram-fw-ota.json: version, size, SHA-256 and an
// Ed25519 signature over them, made in CI with a key only the repository's
// secrets hold. The firmware has the public key (update_key.h) and installs
// nothing whose signature or hash does not match, so the download itself
// needs no trusted TLS. A newly installed image stays provisional until it has
// shown it works (main board answering, network up); if it reboots or hangs
// first, the bootloader goes back to the previous image, and that version is
// not installed automatically again.
#pragma once

#include <Arduino.h>

namespace updater {

// Starts the background task once the network is up. Checks daily, a few
// minutes after boot first; installs by itself if automatic updates are on.
void begin();

// Call regularly. `healthy` says whether this image works; a provisional
// image is confirmed once it is true, and rolled back if it stays false.
void loop(bool healthy);

// Asks the task to check now, and to install a newer version if `install`.
void request(bool install);

bool auto_install();
void set_auto_install(bool on);

// {"state":..., "current":..., "latest":..., "available":..., ...}
String status_json();

// One line for the status page, HTML.
String status_html();

}  // namespace updater
