// Loads the cluster config JSON into dkv::wire::ClusterConfig, applies defaults, validates (LLD §3).
#pragma once

#include <string>

#include "common/status.h"
#include "config.pb.h"

namespace dkv {

StatusOr<wire::ClusterConfig> LoadClusterConfig(const std::string& path);
StatusOr<wire::ClusterConfig> ParseClusterConfig(const std::string& json);
void ApplyConfigDefaults(wire::ClusterConfig* cfg);
Status ValidateClusterConfig(const wire::ClusterConfig& cfg);

// "<host>:<port>"
std::string EndpointAddress(const wire::Endpoint& e);
// Storage endpoint by id, or nullptr.
const wire::Endpoint* FindStorageNode(const wire::ClusterConfig& cfg, const std::string& id);
// <data_dir>/<id> with ${USER}/${ID} expanded.
std::string NodeDataDir(const wire::ClusterConfig& cfg, const std::string& id);

}  // namespace dkv
