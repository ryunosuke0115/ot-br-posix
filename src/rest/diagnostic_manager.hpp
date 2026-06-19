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
    };

    struct DeviceDiagCache
    {
        std::string              mExtAddr;
        std::string              mRloc16;
        std::vector<std::string> mIp6AddressList;
        // key: src-ipv6 (External->Thread) or dst-ipv6 (Thread->External)
        std::map<std::string, TrafficEntry> mExternalToThread;
        std::map<std::string, TrafficEntry> mThreadToExternal;
    };

    std::map<std::string, DeviceDiagCache> mDeviceCache;
};

} // namespace rest
} // namespace otbr

#endif // OTBR_REST_DIAGNOSTIC_MANAGER_H_
