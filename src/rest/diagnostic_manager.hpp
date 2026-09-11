#ifndef OTBR_REST_DIAGNOSTIC_MANAGER_H_
#define OTBR_REST_DIAGNOSTIC_MANAGER_H_

#include <map>
#include <string>
#include <vector>
#include <chrono>

#include <openthread/instance.h>
#include <openthread/netdiag.h>
#include "common/mainloop.hpp"

namespace otbr {
namespace Host {
class RcpHost;
}
namespace rest {

class DiagnosticManager: public MainloopProcessor
{
public:
    explicit DiagnosticManager(otbr::Host::RcpHost &aHost);

    void Update(MainloopContext &aMainloop) override;
    void Process(const MainloopContext &aMainloop) override;

    std::string GetDiagnosticData(void);
    std::string GetNetworkInfo(void);
    bool        HasEui64(const std::string &aEui64) const;

private:
    void        FetchDiagnosticData(void);
    void        HandleDiagnosticResponse(const otMessage *aMessage);
    void        UpdateTrafficStats(double aElapsedSec);
    otInstance *GetInstance(void) const;

    otbr::Host::RcpHost &mHost;
    std::chrono::steady_clock::time_point mNextFetchTime;

    struct TrafficEntry
    {
        uint64_t mPackets       = 0;
        uint64_t mBytes         = 0;
        double   mPacketsPerSec = 0.0;
        double   mBytesPerSec   = 0.0;
        uint64_t mLastPackets   = 0;
        uint64_t mLastBytes     = 0;
        bool     mHasPreviousSample = false;
    };

    struct RouteEntry
    {
        uint8_t  mRouterId       = 0;
        uint16_t mRloc16         = 0;
        uint8_t  mLinkQualityIn  = 0;
        uint8_t  mLinkQualityOut = 0;
        uint8_t  mRouteCost      = 0;
    };

    struct LeaderData
    {
        uint32_t mPartitionId       = 0;
        uint8_t  mWeighting         = 0;
        uint8_t  mDataVersion       = 0;
        uint8_t  mStableDataVersion = 0;
        uint8_t  mLeaderRouterId    = 0;
    };

    struct ChildEntry
    {
        uint16_t mChildId          = 0;
        uint16_t mRloc16           = 0;
        uint16_t mTimeout          = 0;
        uint8_t  mLinkQuality      = 0;
        uint8_t  mMode             = 0;
        bool     mRxOnWhenIdle     = false;
        bool     mFullThreadDevice = false;
        bool     mFullNetworkData  = false;
    };

    struct DeviceDiagCache
    {
        std::string              mExtAddr;
        std::string              mEui64;
        std::string              mRloc16;
        std::vector<std::string> mIp6AddressList;
        uint8_t                  mRouteIdSequence = 0;
        std::vector<RouteEntry>  mRouteList;
        LeaderData               mLeaderData;
        std::vector<ChildEntry>  mChildList;
        // key: src-ipv6 (External->Thread) or dst-ipv6 (Thread->External)
        std::map<std::string, TrafficEntry> mExternalToThread;
        std::map<std::string, TrafficEntry> mThreadToExternal;
    };

    std::map<std::string, DeviceDiagCache> mDeviceCache;
};

} // namespace rest
} // namespace otbr

#endif // OTBR_REST_DIAGNOSTIC_MANAGER_H_
