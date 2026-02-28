#define OTBR_LOG_TAG "REST"

#include "diagnostic_manager.hpp"
#include "host/rcp_host.hpp"
#include <cJSON.h>
#include "common/logging.hpp"

namespace otbr {
namespace rest {

// 取得する TLV Type
static const uint8_t kTlvTypes[] = {OT_NETWORK_DIAGNOSTIC_TLV_SHORT_ADDRESS};

DiagnosticManager::DiagnosticManager(otbr::Host::RcpHost &aHost)
    : mHost(aHost)
{
    // 初期化が必要な場合はここに記述
}

otInstance *DiagnosticManager::GetInstance(void) const
{
    return mHost.GetThreadHelper() ? mHost.GetThreadHelper()->GetInstance() : nullptr;
}

void DiagnosticManager::FetchDiagnosticData(void)
{
    otInstance *instance = GetInstance();
    if (instance == nullptr) return;

    struct otIp6Address multicastAddress;
    otIp6AddressFromString("ff03::2", &multicastAddress); // 全ルーター宛

    otThreadSendDiagnosticGet(
        instance, &multicastAddress, kTlvTypes, sizeof(kTlvTypes),
        [](otError aError, otMessage *aMessage, const otMessageInfo *aMessageInfo, void *aContext) {
            (void)aMessageInfo;
            if (aError == OT_ERROR_NONE && aMessage != nullptr)
            {
                static_cast<DiagnosticManager *>(aContext)->HandleDiagnosticResponse(aMessage);
            }
        },
        this);
}

void DiagnosticManager::HandleDiagnosticResponse(const otMessage *aMessage)
{
    otNetworkDiagTlv      diagTlv;
    otNetworkDiagIterator iterator = OT_NETWORK_DIAGNOSTIC_ITERATOR_INIT;
    std::string           parsedRloc = "";

    while (otThreadGetNextDiagnosticTlv(aMessage, &iterator, &diagTlv) == OT_ERROR_NONE)
    {
        if (diagTlv.mType == OT_NETWORK_DIAGNOSTIC_TLV_SHORT_ADDRESS)
        {
            char buf[7];
            snprintf(buf, sizeof(buf), "0x%04x", diagTlv.mData.mAddr16);
            parsedRloc = buf;
        }
    }

    if (!parsedRloc.empty())
    {
        mDeviceCache[parsedRloc].mRloc16 = parsedRloc;
        otbrLogInfo("DiagnosticManager: Cached device RLOC16 = %s", parsedRloc.c_str());
    }
}

std::string DiagnosticManager::GetDiagnosticData(void)
{
    // リクエスト時に収集を開始
    FetchDiagnosticData();

    cJSON *root  = cJSON_CreateObject();
    cJSON *nodes = cJSON_AddArrayToObject(root, "nodes");

    for (std::map<std::string, DeviceDiagCache>::const_iterator it = mDeviceCache.begin(); it != mDeviceCache.end(); ++it)
    {
        cJSON *node = cJSON_CreateObject();
        cJSON_AddStringToObject(node, "rloc16", it->first.c_str());
        cJSON_AddItemToArray(nodes, node);
    }

    char *jsonStr = cJSON_Print(root);
    std::string result = jsonStr ? jsonStr : "{}";
    if (jsonStr) cJSON_free(jsonStr);
    cJSON_Delete(root);

    return result;
}

} // namespace rest
} // namespace otbr
