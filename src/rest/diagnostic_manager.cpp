#define OTBR_LOG_TAG "REST"

#include "diagnostic_manager.hpp"
#include "host/rcp_host.hpp"
#include <cJSON.h>
#include "common/logging.hpp"

namespace otbr {
namespace rest {

// 取得する TLV Type
static const uint8_t kTlvTypes[] = {0, 1};

DiagnosticManager::DiagnosticManager(otbr::Host::RcpHost &aHost)
    : mHost(aHost)
{
    // Initialize
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

    // start get diagnostic data
    otbrLogInfo("DiagnosticManager: Start fetching diagnostic data from network");

    // to all devices in the Thread network
    otIp6AddressFromString("ff03::2", &multicastAddress);

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
    std::string           parsedExtAddr = "";
    std::string           parsedRloc = "";

    while (otThreadGetNextDiagnosticTlv(aMessage, &iterator, &diagTlv) == OT_ERROR_NONE)
    {
        // otbrLogInfo("DiagnosticManager: Found TLV Type = %u", diagTlv.mType);
        // Type 0: Extended Address
        if (diagTlv.mType == OT_NETWORK_DIAGNOSTIC_TLV_EXT_ADDRESS)
        {
            char buf[17];
            const uint8_t *ext = diagTlv.mData.mExtAddress.m8;
            snprintf(buf, sizeof(buf), "%02x%02x%02x%02x%02x%02x%02x%02x",
                     ext[0], ext[1], ext[2], ext[3], ext[4], ext[5], ext[6], ext[7]);
            parsedExtAddr = buf;
            // otbrLogInfo("DiagnosticManager: Parsed ExtAddr: %s", parsedExtAddr.c_str());
        }
        // Type 1: RLOC16
        else if (diagTlv.mType == OT_NETWORK_DIAGNOSTIC_TLV_SHORT_ADDRESS)
        {
            char buf[7];
            snprintf(buf, sizeof(buf), "0x%04x", diagTlv.mData.mAddr16);
            parsedRloc = buf;
        }
    }

    if (!parsedExtAddr.empty())
    {
        mDeviceCache[parsedExtAddr].mExtAddr = parsedExtAddr;
        mDeviceCache[parsedExtAddr].mRloc16 = parsedRloc;
        otbrLogInfo("DiagnosticManager: Cached device RLOC16 = %s, ExtAddr = %s", parsedRloc.c_str(), parsedExtAddr.c_str());
    }
}

std::string DiagnosticManager::GetDiagnosticData(void)
{
    // TODO: 定期的に実行する
    FetchDiagnosticData();

    cJSON *root  = cJSON_CreateObject();
    cJSON *nodes = cJSON_AddArrayToObject(root, "nodes");

    for (std::map<std::string, DeviceDiagCache>::const_iterator it = mDeviceCache.begin(); it != mDeviceCache.end(); ++it)
    {
        cJSON *node = cJSON_CreateObject();
        cJSON_AddStringToObject(node, "extAddr", it->first.c_str());
        cJSON_AddStringToObject(node, "rloc16", it->second.mRloc16.c_str());
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
