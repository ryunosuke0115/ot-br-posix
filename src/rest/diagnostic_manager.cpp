#define OTBR_LOG_TAG "REST"

#include "diagnostic_manager.hpp"
#include "host/rcp_host.hpp"
#include <cJSON.h>
#include "common/logging.hpp"
#include <arpa/inet.h>
#include <algorithm>
#include <cctype>
#include <inttypes.h>
#include <openthread/openthread-system.h>

namespace otbr {
namespace rest {

static const std::chrono::seconds kFetchInterval = std::chrono::seconds(10);

// 取得する TLV Type
static const uint8_t kTlvTypes[] = {
    OT_NETWORK_DIAGNOSTIC_TLV_EXT_ADDRESS,   // # Extended Address
    OT_NETWORK_DIAGNOSTIC_TLV_EUI64,         // # Factory-assigned EUI-64
    OT_NETWORK_DIAGNOSTIC_TLV_SHORT_ADDRESS, // # RLOC16
    OT_NETWORK_DIAGNOSTIC_TLV_CHILD_TABLE,   // # Child Table
    OT_NETWORK_DIAGNOSTIC_TLV_ROUTE,         // # Route
    OT_NETWORK_DIAGNOSTIC_TLV_LEADER_DATA,   // # Leader Data
    OT_NETWORK_DIAGNOSTIC_TLV_IP6_ADDR_LIST, // # IPv6 Address List
};

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
        // トラフィック統計を更新
        UpdateTrafficStats(std::chrono::duration<double>(kFetchInterval).count());

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
    std::string           parsedEui64 = "";
    std::string           parsedRloc = "";
    uint16_t              parsedRloc16 = 0;
    std::vector<std::string> parsedIpList;
    uint8_t              parsedRouteIdSequence = 0;
    std::vector<RouteEntry> parsedRouteList;
    LeaderData           parsedLeaderData;
    std::vector<ChildEntry> parsedChildList;

    while (otThreadGetNextDiagnosticTlv(aMessage, &iterator, &diagTlv) == OT_ERROR_NONE)
    {
        // otbrLogInfo("DiagnosticManager: Found TLV Type = %u", diagTlv.mType);
        // # Extended Address
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
        // # Factory-assigned EUI-64
        // Type 23: EUI-64
        else if (diagTlv.mType == OT_NETWORK_DIAGNOSTIC_TLV_EUI64)
        {
            char           buf[17];
            const uint8_t *eui64 = diagTlv.mData.mEui64.m8;

            snprintf(buf, sizeof(buf), "%02x%02x%02x%02x%02x%02x%02x%02x",
                     eui64[0], eui64[1], eui64[2], eui64[3], eui64[4], eui64[5], eui64[6], eui64[7]);
            parsedEui64 = buf;
        }
        // # RLOC16
        // Type 1: RLOC16
        else if (diagTlv.mType == OT_NETWORK_DIAGNOSTIC_TLV_SHORT_ADDRESS)
        {
            char buf[7];
            parsedRloc16 = diagTlv.mData.mAddr16;
            snprintf(buf, sizeof(buf), "0x%04x", parsedRloc16);
            parsedRloc = buf;
        }
        // # Child Table
        // Type 7: Child Table
        else if (diagTlv.mType == OT_NETWORK_DIAGNOSTIC_TLV_CHILD_TABLE)
        {
            enum
            {
                kModeRxOnWhenIdle     = 1 << 3,
                kModeFullThreadDevice = 1 << 1,
                kModeFullNetworkData  = 1 << 0,
            };

            parsedChildList.clear();

            for (uint16_t i = 0; i < diagTlv.mData.mChildTable.mCount; i++)
            {
                const otNetworkDiagChildEntry &child = diagTlv.mData.mChildTable.mTable[i];
                ChildEntry entry;

                entry.mChildId          = child.mChildId;
                entry.mRloc16           = static_cast<uint16_t>((parsedRloc16 & 0xfc00) | child.mChildId);
                entry.mTimeout          = child.mTimeout;
                entry.mLinkQuality      = child.mLinkQuality;
                entry.mRxOnWhenIdle     = child.mMode.mRxOnWhenIdle;
                entry.mFullThreadDevice = child.mMode.mDeviceType;
                entry.mFullNetworkData  = child.mMode.mNetworkData;
                entry.mMode             = (entry.mRxOnWhenIdle ? kModeRxOnWhenIdle : 0) |
                              (entry.mFullThreadDevice ? kModeFullThreadDevice : 0) |
                              (entry.mFullNetworkData ? kModeFullNetworkData : 0);
                parsedChildList.push_back(entry);
            }
        }
        // # IPv6 Address List
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
        // # Route
        // Type 5: Route64
        else if (diagTlv.mType == OT_NETWORK_DIAGNOSTIC_TLV_ROUTE)
        {
            const otNetworkDiagRoute &route = diagTlv.mData.mRoute;

            parsedRouteIdSequence = route.mIdSequence;
            parsedRouteList.clear();

            for (uint16_t i = 0; i < route.mRouteCount; i++)
            {
                RouteEntry entry;

                entry.mRouterId       = route.mRouteData[i].mRouterId;
                entry.mRloc16         = static_cast<uint16_t>(route.mRouteData[i].mRouterId << 10);
                entry.mLinkQualityIn  = route.mRouteData[i].mLinkQualityIn;
                entry.mLinkQualityOut = route.mRouteData[i].mLinkQualityOut;
                entry.mRouteCost      = route.mRouteData[i].mRouteCost;
                parsedRouteList.push_back(entry);
            }
        }
        // # Leader Data
        // Type 9: Leader Data
        else if (diagTlv.mType == OT_NETWORK_DIAGNOSTIC_TLV_LEADER_DATA)
        {
            const otLeaderData &leaderData = diagTlv.mData.mLeaderData;

            parsedLeaderData.mPartitionId       = leaderData.mPartitionId;
            parsedLeaderData.mWeighting         = leaderData.mWeighting;
            parsedLeaderData.mDataVersion       = leaderData.mDataVersion;
            parsedLeaderData.mStableDataVersion = leaderData.mStableDataVersion;
            parsedLeaderData.mLeaderRouterId    = leaderData.mLeaderRouterId;
        }
    }

    if (!parsedExtAddr.empty())
    {
        mDeviceCache[parsedExtAddr].mExtAddr = parsedExtAddr;
        if (!parsedEui64.empty())
        {
            mDeviceCache[parsedExtAddr].mEui64 = parsedEui64;
        }
        mDeviceCache[parsedExtAddr].mRloc16 = parsedRloc;
        mDeviceCache[parsedExtAddr].mIp6AddressList = parsedIpList;
        mDeviceCache[parsedExtAddr].mRouteIdSequence = parsedRouteIdSequence;
        mDeviceCache[parsedExtAddr].mRouteList = parsedRouteList;
        mDeviceCache[parsedExtAddr].mLeaderData = parsedLeaderData;
        mDeviceCache[parsedExtAddr].mChildList = parsedChildList;
        otbrLogInfo("DiagnosticManager: Cached device RLOC16 = %s, ExtAddr = %s, IPs count = %zu, routes count = %zu, children count = %zu, leader router id = %u",
                    parsedRloc.c_str(), parsedExtAddr.c_str(), parsedIpList.size(), parsedRouteList.size(),
                    parsedChildList.size(), parsedLeaderData.mLeaderRouterId);
    }
}

void DiagnosticManager::UpdateTrafficStats(double aElapsedSec)
{
    const otSysPerDestStats *perDest = otSysGetPerDestStats();
    if (perDest == nullptr) return;

    for (auto &kv : mDeviceCache)
    {
        DeviceDiagCache &device = kv.second;

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

            const std::string src(perDest->mExternalToThread[i].mSrcAddr);
            TrafficEntry &entry  = device.mExternalToThread[src];
            entry.mPackets       = perDest->mExternalToThread[i].mPackets;
            entry.mBytes         = perDest->mExternalToThread[i].mBytes;
            if (entry.mHasPreviousSample && entry.mPackets >= entry.mLastPackets && entry.mBytes >= entry.mLastBytes)
            {
                entry.mPacketsPerSec = static_cast<double>(entry.mPackets - entry.mLastPackets) / aElapsedSec;
                entry.mBytesPerSec   = static_cast<double>(entry.mBytes   - entry.mLastBytes)   / aElapsedSec;
            }
            else
            {
                entry.mPacketsPerSec = 0.0;
                entry.mBytesPerSec   = 0.0;
            }
            entry.mLastPackets   = entry.mPackets;
            entry.mLastBytes     = entry.mBytes;
            entry.mHasPreviousSample = true;
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

            const std::string dst(perDest->mThreadToExternal[i].mDstAddr);
            TrafficEntry &entry  = device.mThreadToExternal[dst];
            entry.mPackets       = perDest->mThreadToExternal[i].mPackets;
            entry.mBytes         = perDest->mThreadToExternal[i].mBytes;
            if (entry.mHasPreviousSample && entry.mPackets >= entry.mLastPackets && entry.mBytes >= entry.mLastBytes)
            {
                entry.mPacketsPerSec = static_cast<double>(entry.mPackets - entry.mLastPackets) / aElapsedSec;
                entry.mBytesPerSec   = static_cast<double>(entry.mBytes   - entry.mLastBytes)   / aElapsedSec;
            }
            else
            {
                entry.mPacketsPerSec = 0.0;
                entry.mBytesPerSec   = 0.0;
            }
            entry.mLastPackets   = entry.mPackets;
            entry.mLastBytes     = entry.mBytes;
            entry.mHasPreviousSample = true;
        }
    }
}

std::string DiagnosticManager::GetNetworkInfo(void)
{
    cJSON *root  = cJSON_CreateObject();
    cJSON *nodes = cJSON_AddArrayToObject(root, "nodes");

    for (std::map<std::string, DeviceDiagCache>::const_iterator it = mDeviceCache.begin(); it != mDeviceCache.end();
         ++it)
    {
        const DeviceDiagCache &device = it->second;

        cJSON *node = cJSON_CreateObject();
        cJSON_AddStringToObject(node, "extendedAddress", it->first.c_str());
        if (!device.mEui64.empty())
        {
            cJSON_AddStringToObject(node, "eui64", device.mEui64.c_str());
        }
        cJSON_AddStringToObject(node, "rloc16", device.mRloc16.c_str());

        cJSON *ipList = cJSON_AddArrayToObject(node, "ipv6-lists");
        for (const std::string &ip : device.mIp6AddressList)
        {
            cJSON_AddItemToArray(ipList, cJSON_CreateString(ip.c_str()));
        }

        cJSON_AddNumberToObject(node, "route-id-sequence", device.mRouteIdSequence);
        cJSON *routes = cJSON_AddArrayToObject(node, "routes");
        for (const RouteEntry &route : device.mRouteList)
        {
            char rloc16[7];
            cJSON *entry = cJSON_CreateObject();

            snprintf(rloc16, sizeof(rloc16), "0x%04x", route.mRloc16);
            cJSON_AddNumberToObject(entry, "router-id", route.mRouterId);
            cJSON_AddStringToObject(entry, "rloc16", rloc16);
            cJSON_AddNumberToObject(entry, "link-quality-in", route.mLinkQualityIn);
            cJSON_AddNumberToObject(entry, "link-quality-out", route.mLinkQualityOut);
            cJSON_AddNumberToObject(entry, "route-cost", route.mRouteCost);
            cJSON_AddItemToArray(routes, entry);
        }

        cJSON *leaderData = cJSON_AddObjectToObject(node, "leader-data");
        cJSON_AddNumberToObject(leaderData, "partition-id", device.mLeaderData.mPartitionId);
        cJSON_AddNumberToObject(leaderData, "weighting", device.mLeaderData.mWeighting);
        cJSON_AddNumberToObject(leaderData, "data-version", device.mLeaderData.mDataVersion);
        cJSON_AddNumberToObject(leaderData, "stable-data-version", device.mLeaderData.mStableDataVersion);
        cJSON_AddNumberToObject(leaderData, "leader-router-id", device.mLeaderData.mLeaderRouterId);

        cJSON *children = cJSON_AddArrayToObject(node, "children");
        for (const ChildEntry &child : device.mChildList)
        {
            char rloc16[7];
            cJSON *entry = cJSON_CreateObject();

            snprintf(rloc16, sizeof(rloc16), "0x%04x", child.mRloc16);
            cJSON_AddNumberToObject(entry, "child-id", child.mChildId);
            cJSON_AddStringToObject(entry, "rloc16", rloc16);
            cJSON_AddNumberToObject(entry, "timeout", child.mTimeout);
            cJSON_AddNumberToObject(entry, "link-quality", child.mLinkQuality);
            cJSON_AddNumberToObject(entry, "mode", child.mMode);
            cJSON_AddBoolToObject(entry, "rx-on-when-idle", child.mRxOnWhenIdle);
            cJSON_AddBoolToObject(entry, "full-thread-device", child.mFullThreadDevice);
            cJSON_AddBoolToObject(entry, "full-network-data", child.mFullNetworkData);
            cJSON_AddItemToArray(children, entry);
        }

        cJSON *traffic = cJSON_CreateObject();
        cJSON_AddItemToObject(node, "traffic", traffic);
        cJSON *externalToThread = cJSON_AddArrayToObject(traffic, "external-to-thread");
        cJSON *threadToExternal = cJSON_AddArrayToObject(traffic, "thread-to-external");

        // External->Thread
        for (std::map<std::string, TrafficEntry>::const_iterator e = device.mExternalToThread.begin();
             e != device.mExternalToThread.end(); ++e)
        {
            cJSON *entry = cJSON_CreateObject();
            cJSON_AddStringToObject(entry, "src-ipv6",        e->first.c_str());
            cJSON_AddNumberToObject(entry, "packets",         static_cast<double>(e->second.mPackets));
            cJSON_AddNumberToObject(entry, "bytes",           static_cast<double>(e->second.mBytes));
            cJSON_AddNumberToObject(entry, "packets-per-sec", e->second.mPacketsPerSec);
            cJSON_AddNumberToObject(entry, "bytes-per-sec",   e->second.mBytesPerSec);
            cJSON_AddItemToArray(externalToThread, entry);
        }

        // Thread->External
        for (std::map<std::string, TrafficEntry>::const_iterator e = device.mThreadToExternal.begin();
             e != device.mThreadToExternal.end(); ++e)
        {
            cJSON *entry = cJSON_CreateObject();
            cJSON_AddStringToObject(entry, "dst-ipv6",        e->first.c_str());
            cJSON_AddNumberToObject(entry, "packets",         static_cast<double>(e->second.mPackets));
            cJSON_AddNumberToObject(entry, "bytes",           static_cast<double>(e->second.mBytes));
            cJSON_AddNumberToObject(entry, "packets-per-sec", e->second.mPacketsPerSec);
            cJSON_AddNumberToObject(entry, "bytes-per-sec",   e->second.mBytesPerSec);
            cJSON_AddItemToArray(threadToExternal, entry);
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

bool DiagnosticManager::HasEui64(const std::string &aEui64) const
{
    std::string normalized = aEui64;

    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char aChar) { return static_cast<char>(std::tolower(aChar)); });

    for (const auto &entry : mDeviceCache)
    {
        if (entry.second.mEui64 == normalized)
        {
            return true;
        }
    }

    return false;
}

} // namespace rest
} // namespace otbr
