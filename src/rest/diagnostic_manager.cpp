#define OTBR_LOG_TAG "REST"

#include "diagnostic_manager.hpp"
#include "host/rcp_host.hpp"
#include <cJSON.h>
#include "common/logging.hpp"

namespace otbr {
namespace rest {

DiagnosticManager::DiagnosticManager(otbr::Host::RcpHost &aHost)
    : mHost(aHost)
{
    // 初期化が必要な場合はここに記述
}

std::string DiagnosticManager::GetTopologyJson(void)
{
    cJSON *root  = cJSON_CreateObject();
    cJSON *nodes = cJSON_AddArrayToObject(root, "nodes");

    // テスト用のダミーデータ
    cJSON *node = cJSON_CreateObject();
    cJSON_AddStringToObject(node, "rloc16", "0xabcd");
    cJSON_AddStringToObject(node, "role", "Leader");
    cJSON_AddItemToArray(nodes, node);

    char *jsonStr = cJSON_Print(root);
    std::string result = (jsonStr != NULL) ? jsonStr : "{}";

    if (jsonStr != NULL)
    {
        cJSON_free(jsonStr);
    }
    cJSON_Delete(root);

    return result;
}

} // namespace rest
} // namespace otbr
