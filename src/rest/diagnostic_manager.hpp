#ifndef OTBR_REST_DIAGNOSTIC_MANAGER_H_
#define OTBR_REST_DIAGNOSTIC_MANAGER_H_

#include <string>
#include <openthread/instance.h>

namespace otbr {
namespace Host {
class RcpHost;
}
namespace rest {

class DiagnosticManager
{
public:
    explicit DiagnosticManager(otbr::Host::RcpHost &aHost);

    std::string GetTopologyJson(void);

private:
    otbr::Host::RcpHost &mHost;
};

} // namespace rest
} // namespace otbr

#endif // OTBR_REST_DIAGNOSTIC_MANAGER_H_
