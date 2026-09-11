#ifndef OTBR_REST_DEVICE_REGISTRY_HPP_
#define OTBR_REST_DEVICE_REGISTRY_HPP_

#include <map>
#include <mutex>
#include <string>

namespace otbr {
namespace rest {

struct DeviceMetadata
{
    std::string mEui64;
    std::string mName;
    std::string mLocation;
    std::string mType;
};

class DeviceRegistry
{
public:
    explicit DeviceRegistry(const std::string &aPath);

    static bool        IsValidEui64(const std::string &aEui64);
    static std::string NormalizeEui64(const std::string &aEui64);

    bool        IsHealthy(void) const;
    std::string GetDevicesJson(void) const;
    std::string GetDeviceJson(const std::string &aEui64) const;
    bool Update(const DeviceMetadata &aDevice, std::string &aError);

private:
    bool Load(void);
    bool SaveLocked(std::string &aError) const;

    const std::string                     mPath;
    bool                                  mHealthy;
    std::map<std::string, DeviceMetadata> mDevices;
    mutable std::mutex                     mMutex;
};

} // namespace rest
} // namespace otbr

#endif // OTBR_REST_DEVICE_REGISTRY_HPP_
