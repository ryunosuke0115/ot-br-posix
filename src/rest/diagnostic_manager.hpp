#ifndef OTBR_REST_DIAGNOSTIC_MANAGER_H_
#define OTBR_REST_DIAGNOSTIC_MANAGER_H_

#include <map>
#include <string>
#include <vector>
#include <chrono>

#include <openthread/instance.h>
#include <openthread/netdiag.h>

namespace otbr {
namespace Host {
class RcpHost;
}
namespace rest {

class DiagnosticManager
{
public:
    explicit DiagnosticManager(otbr::Host::RcpHost &aHost);

    std::string GetDiagnosticData(void);

private:
    void        FetchDiagnosticData(void);
    void        HandleDiagnosticResponse(const otMessage *aMessage);
    otInstance *GetInstance(void) const;

    otbr::Host::RcpHost &mHost;

    struct DeviceDiagCache
    {
        std::string mExtAddr;
        std::string mRloc16;
    };

    std::map<std::string, DeviceDiagCache> mDeviceCache;
};

} // namespace rest
} // namespace otbr

#endif // OTBR_REST_DIAGNOSTIC_MANAGER_H_
