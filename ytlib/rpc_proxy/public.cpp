#include "public.h"

namespace NYT {
namespace NRpcProxy {

////////////////////////////////////////////////////////////////////////////////

const TString RpcProxiesPath = "//sys/rpc_proxies";
const TString GrpcProxiesPath = "//sys/grpc_proxies";
const TString AliveNodeName = "alive";
const TString BannedAttributeName = "banned";
const TString RoleAttributeName = "role";
const TString DefaultProxyRole = "default";
const TString ApiServiceName = "ApiService";
const TString DiscoveryServiceName = "DiscoveryService";

////////////////////////////////////////////////////////////////////////////////

} // namespace NRpcProxy
} // namespace NYT
