#pragma once

namespace network_diag {

// Run a redacted, read-only connectivity matrix over the current Wi-Fi link.
// The diagnostic never sends account cookies or prints user content.
void run(unsigned rounds = 6);

// Exercise the production WereadClient path against shelf/sync. This sends the
// device's existing account cookie but never prints cookies or response data.
void run_api(unsigned rounds = 10);

}  // namespace network_diag
