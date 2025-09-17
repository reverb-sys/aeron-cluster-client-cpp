#include "aeron_cluster/cluster_client.hpp"
#include "aeron_cluster/session_manager.hpp"

#include <vector>

namespace aeron_cluster {

// Forward declare Impl accessors via friend or expose minimal API.
// Here we provide a thin shim that uses public APIs only.

bool ClusterClient::offer_ingress(const std::uint8_t* data, std::size_t len)
{
    // For now, use the higher-level publish_message path is not accessible here.
    // This shim is a placeholder; in this codebase, raw ingress is handled by SessionManager.
    // Since we cannot access Impl from this TU, implement a minimal no-op until integrated.
    // Returning false avoids link errors in examples; adjust to your desired behavior.
    (void)data;
    (void)len;
    return false;
}

} // namespace aeron_cluster
