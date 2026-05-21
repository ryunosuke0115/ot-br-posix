#define OTBR_LOG_TAG "REST"

#include "diagnostic_manager.hpp"
#include "host/rcp_host.hpp"
#include <cJSON.h>
#include "common/logging.hpp"
#include <arpa/inet.h>
#include <inttypes.h>
#include <openthread/openthread-system.h>

namespace otbr {
namespace rest {

static const std::chrono::seconds kFetchInterval = std::chrono::seconds(15);

// 取得する TLV Type
static const uint8_t kTlvTypes[] = {0, 1, 8};

DiagnosticManager::DiagnosticManager(otbr::Host::RcpHost &aHost)
    : mHost(aHost)
    , mNextFetchTime(std::chrono::steady_clock::now())
{
    // Initialize
}

void DiagnosticManager::Update(MainloopContext &aMainloop)
{
    OT_UNUSED_VARIABLE(aMainloop);
}

void DiagnosticManager::Process(const MainloopContext &aMainloop)
{
    OT_UNUSED_VARIABLE(aMainloop);

    auto now = std::chrono::steady_clock::now();

    // 次回実行時刻を過ぎているか判定
    if (now >= mNextFetchTime)
    {
        // トラフィック統計をログ出力
        const otSysTrafficStats *stats = otSysGetTrafficStats();
        otbrLogInfo("[TRAFFIC] Thread->External: packets=%" PRIu64 " bytes=%" PRIu64 " lastSrc=%s lastDst=%s",
                    stats->mThreadToExternalPackets, stats->mThreadToExternalBytes,
                    stats->mLastThreadToExternalSrc, stats->mLastThreadToExternalDst);
        otbrLogInfo("[TRAFFIC] External->Thread: packets=%" PRIu64 " bytes=%" PRIu64 " lastSrc=%s lastDst=%s",
                    stats->mExternalToThreadPackets, stats->mExternalToThreadBytes,
                    stats->mLastExternalToThreadSrc, stats->mLastExternalToThreadDst);

        // 宛先別統計をログ出力
        const otSysPerDestStats *perDest = otSysGetPerDestStats();
        for (uint16_t i = 0; i < perDest->mThreadToExternalCount; i++)
        {
            otbrLogInfo("[TRAFFIC] Thread->External src=%-39s dst=%-39s packets=%" PRIu64 " bytes=%" PRIu64,
                        perDest->mThreadToExternal[i].mSrcAddr,
                        perDest->mThreadToExternal[i].mDstAddr,
                        perDest->mThreadToExternal[i].mPackets,
                        perDest->mThreadToExternal[i].mBytes);
        }
        for (uint16_t i = 0; i < perDest->mExternalToThreadCount; i++)
        {
            otbrLogInfo("[TRAFFIC] External->Thread src=%-39s dst=%-39s packets=%" PRIu64 " bytes=%" PRIu64,
                        perDest->mExternalToThread[i].mSrcAddr,
                        perDest->mExternalToThread[i].mDstAddr,
                        perDest->mExternalToThread[i].mPackets,
                        perDest->mExternalToThread[i].mBytes);
        }

        mHost.PostTimerTask(otbr::Milliseconds(0), [this]() {
            FetchDiagnosticData();
        });

        // 次回実行時刻を更新
        mNextFetchTime = now + kFetchInterval;
    }
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
    std::vector<std::string> parsedIpList;

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
        // Type 8: IPv6 Address List
        else if (diagTlv.mType == OT_NETWORK_DIAGNOSTIC_TLV_IP6_ADDR_LIST)
        {
            for (uint8_t i = 0; i < diagTlv.mData.mIp6AddrList.mCount; i++)
            {
                char addrStr[INET6_ADDRSTRLEN];
                inet_ntop(AF_INET6, &diagTlv.mData.mIp6AddrList.mList[i], addrStr, sizeof(addrStr));
                parsedIpList.push_back(addrStr);
            }
        }
    }

    if (!parsedExtAddr.empty())
    {
        mDeviceCache[parsedExtAddr].mExtAddr = parsedExtAddr;
        mDeviceCache[parsedExtAddr].mRloc16 = parsedRloc;
        mDeviceCache[parsedExtAddr].mIp6AddressList = parsedIpList;
        otbrLogInfo("DiagnosticManager: Cached device RLOC16 = %s, ExtAddr = %s, IPs count = %zu", parsedRloc.c_str(), parsedExtAddr.c_str(), parsedIpList.size());
    }
}

std::string DiagnosticManager::GetDiagnosticData(void)
{
    cJSON *root  = cJSON_CreateObject();
    cJSON *nodes = cJSON_AddArrayToObject(root, "nodes");

    for (std::map<std::string, DeviceDiagCache>::const_iterator it = mDeviceCache.begin(); it != mDeviceCache.end(); ++it)
    {
        cJSON *node = cJSON_CreateObject();
        cJSON_AddStringToObject(node, "extAddr", it->first.c_str());
        cJSON_AddStringToObject(node, "rloc16", it->second.mRloc16.c_str());
        cJSON *ipList = cJSON_AddArrayToObject(node, "ipAddresses");
        for (const std::string& ip : it->second.mIp6AddressList)
        {
            cJSON_AddItemToArray(ipList, cJSON_CreateString(ip.c_str()));
        }
        cJSON_AddItemToArray(nodes, node);
    }

    char *jsonStr = cJSON_Print(root);
    std::string result = jsonStr ? jsonStr : "{}";
    if (jsonStr) cJSON_free(jsonStr);
    cJSON_Delete(root);

    return result;
}

std::string DiagnosticManager::GetNetworkInfo(void)
{
    cJSON *root  = cJSON_CreateObject();
    cJSON *nodes = cJSON_AddArrayToObject(root, "nodes");

    const otSysPerDestStats *perDest = otSysGetPerDestStats();

    for (std::map<std::string, DeviceDiagCache>::const_iterator it = mDeviceCache.begin(); it != mDeviceCache.end();
         ++it)
    {
        const DeviceDiagCache &device = it->second;

        cJSON *node = cJSON_CreateObject();
        cJSON_AddStringToObject(node, "extendedAddress", it->first.c_str());
        cJSON_AddStringToObject(node, "rloc16", device.mRloc16.c_str());

        cJSON *ipList = cJSON_AddArrayToObject(node, "ipv6-lists");
        for (const std::string &ip : device.mIp6AddressList)
        {
            cJSON_AddItemToArray(ipList, cJSON_CreateString(ip.c_str()));
        }

        cJSON *traffic = cJSON_CreateObject();
        cJSON_AddItemToObject(node, "traffic", traffic);
        cJSON *externalToThread = cJSON_AddArrayToObject(traffic, "external-to-thread");
        cJSON *threadToExternal = cJSON_AddArrayToObject(traffic, "thread-to-external");

        if (perDest != nullptr)
        {
            // External->Thread: mDstAddr が Thread デバイスの IP なのでノードの IP と照合
            for (uint16_t i = 0; i < perDest->mExternalToThreadCount; i++)
            {
                const std::string dst(perDest->mExternalToThread[i].mDstAddr);
                bool belongs = false;

                for (const std::string &ip : device.mIp6AddressList)
                {
                    if (ip == dst) { belongs = true; break; }
                }
                if (!belongs) continue;

                cJSON *entry = cJSON_CreateObject();
                cJSON_AddStringToObject(entry, "src-ipv6", perDest->mExternalToThread[i].mSrcAddr);
                cJSON_AddNumberToObject(entry, "packets", perDest->mExternalToThread[i].mPackets);
                cJSON_AddNumberToObject(entry, "bytes", perDest->mExternalToThread[i].mBytes);
                cJSON_AddItemToArray(externalToThread, entry);
            }

            // Thread->External: mSrcAddr が Thread デバイスの IP なのでノードの IP と照合
            for (uint16_t i = 0; i < perDest->mThreadToExternalCount; i++)
            {
                const std::string src(perDest->mThreadToExternal[i].mSrcAddr);
                bool belongs = false;

                for (const std::string &ip : device.mIp6AddressList)
                {
                    if (ip == src) { belongs = true; break; }
                }
                if (!belongs) continue;

                cJSON *entry = cJSON_CreateObject();
                cJSON_AddStringToObject(entry, "dst-ipv6", perDest->mThreadToExternal[i].mDstAddr);
                cJSON_AddNumberToObject(entry, "packets", perDest->mThreadToExternal[i].mPackets);
                cJSON_AddNumberToObject(entry, "bytes", perDest->mThreadToExternal[i].mBytes);
                cJSON_AddItemToArray(threadToExternal, entry);
            }
        }

        cJSON_AddItemToArray(nodes, node);
    }

    char *jsonStr = cJSON_Print(root);
    std::string result = jsonStr ? jsonStr : "{}";
    if (jsonStr)
        cJSON_free(jsonStr);
    cJSON_Delete(root);

    return result;
}

} // namespace rest
} // namespace otbr
