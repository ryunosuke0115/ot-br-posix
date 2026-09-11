#include "rest/device_registry.hpp"

#include <cJSON.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace otbr {
namespace rest {

namespace {

bool GetOptionalString(const cJSON *aObject, const char *aName, std::string &aValue)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(aObject, aName);

    if (item == nullptr)
    {
        aValue.clear();
        return true;
    }
    if (!cJSON_IsString(item) || item->valuestring == nullptr)
    {
        return false;
    }

    aValue = item->valuestring;
    return true;
}

void AddDeviceJson(cJSON *aDevices, const DeviceMetadata &aDevice)
{
    cJSON *device = cJSON_CreateObject();

    cJSON_AddStringToObject(device, "eui64", aDevice.mEui64.c_str());
    cJSON_AddStringToObject(device, "name", aDevice.mName.c_str());
    cJSON_AddStringToObject(device, "location", aDevice.mLocation.c_str());
    cJSON_AddStringToObject(device, "type", aDevice.mType.c_str());
    cJSON_AddItemToArray(aDevices, device);
}

std::string PrintJson(cJSON *aRoot)
{
    char       *json   = cJSON_Print(aRoot);
    std::string result = json == nullptr ? "{}" : json;

    if (json != nullptr)
    {
        cJSON_free(json);
    }
    cJSON_Delete(aRoot);
    return result;
}

} // namespace

DeviceRegistry::DeviceRegistry(const std::string &aPath)
    : mPath(aPath)
    , mHealthy(false)
{
    mHealthy = Load();
}

bool DeviceRegistry::IsValidEui64(const std::string &aEui64)
{
    return aEui64.size() == 16 &&
           std::all_of(aEui64.begin(), aEui64.end(), [](unsigned char aChar) { return std::isxdigit(aChar) != 0; });
}

std::string DeviceRegistry::NormalizeEui64(const std::string &aEui64)
{
    std::string normalized = aEui64;

    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char aChar) { return static_cast<char>(std::tolower(aChar)); });
    return normalized;
}

bool DeviceRegistry::IsHealthy(void) const
{
    std::lock_guard<std::mutex> lock(mMutex);
    return mHealthy;
}

std::string DeviceRegistry::GetDevicesJson(void) const
{
    std::lock_guard<std::mutex> lock(mMutex);
    cJSON                      *root    = cJSON_CreateObject();
    cJSON                      *devices = cJSON_AddArrayToObject(root, "devices");

    for (const auto &entry : mDevices)
    {
        AddDeviceJson(devices, entry.second);
    }
    return PrintJson(root);
}

std::string DeviceRegistry::GetDeviceJson(const std::string &aEui64) const
{
    std::lock_guard<std::mutex> lock(mMutex);
    const auto                  it = mDevices.find(NormalizeEui64(aEui64));

    if (it == mDevices.end())
    {
        return "{}";
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "eui64", it->second.mEui64.c_str());
    cJSON_AddStringToObject(root, "name", it->second.mName.c_str());
    cJSON_AddStringToObject(root, "location", it->second.mLocation.c_str());
    cJSON_AddStringToObject(root, "type", it->second.mType.c_str());
    return PrintJson(root);
}

bool DeviceRegistry::Update(const DeviceMetadata &aDevice, std::string &aError)
{
    if (!IsValidEui64(aDevice.mEui64) || aDevice.mName.empty())
    {
        aError = "Invalid device metadata";
        return false;
    }

    std::lock_guard<std::mutex> lock(mMutex);
    DeviceMetadata              device = aDevice;

    if (!mHealthy)
    {
        aError = "Device metadata file is unavailable";
        return false;
    }

    device.mEui64 = NormalizeEui64(device.mEui64);
    const auto existing = mDevices.find(device.mEui64);
    const bool hadExisting = existing != mDevices.end();
    DeviceMetadata previous;

    if (hadExisting)
    {
        previous = existing->second;
    }
    mDevices[device.mEui64] = device;

    if (!SaveLocked(aError))
    {
        if (hadExisting)
        {
            mDevices[device.mEui64] = previous;
        }
        else
        {
            mDevices.erase(device.mEui64);
        }
        return false;
    }
    return true;
}

bool DeviceRegistry::Load(void)
{
    std::ifstream input(mPath.c_str());

    if (!input.is_open())
    {
        return true;
    }

    std::stringstream buffer;
    buffer << input.rdbuf();
    cJSON *root = cJSON_Parse(buffer.str().c_str());

    if (root == nullptr)
    {
        return false;
    }

    cJSON *devices = cJSON_GetObjectItemCaseSensitive(root, "devices");
    if (!cJSON_IsArray(devices))
    {
        cJSON_Delete(root);
        return false;
    }

    for (const cJSON *item = devices->child; item != nullptr; item = item->next)
    {
        DeviceMetadata device;
        cJSON         *eui64 = cJSON_GetObjectItemCaseSensitive(item, "eui64");
        cJSON         *name  = cJSON_GetObjectItemCaseSensitive(item, "name");

        if (!cJSON_IsObject(item) || !cJSON_IsString(eui64) || eui64->valuestring == nullptr ||
            !cJSON_IsString(name) || name->valuestring == nullptr || !IsValidEui64(eui64->valuestring) ||
            name->valuestring[0] == '\0' || !GetOptionalString(item, "location", device.mLocation) ||
            !GetOptionalString(item, "type", device.mType))
        {
            cJSON_Delete(root);
            return false;
        }

        device.mEui64 = NormalizeEui64(eui64->valuestring);
        device.mName  = name->valuestring;
        mDevices[device.mEui64] = device;
    }

    cJSON_Delete(root);
    return true;
}

bool DeviceRegistry::SaveLocked(std::string &aError) const
{
    cJSON *root    = cJSON_CreateObject();
    cJSON *devices = cJSON_AddArrayToObject(root, "devices");

    for (const auto &entry : mDevices)
    {
        AddDeviceJson(devices, entry.second);
    }

    char *json = cJSON_Print(root);
    cJSON_Delete(root);

    if (json == nullptr)
    {
        aError = "Failed to serialize device metadata";
        return false;
    }

    const std::string temporaryPath = mPath + ".tmp";
    std::ofstream     output(temporaryPath.c_str(), std::ios::out | std::ios::trunc);

    if (!output.is_open())
    {
        cJSON_free(json);
        aError = "Failed to open temporary metadata file";
        return false;
    }

    output << json;
    output.close();
    cJSON_free(json);

    if (!output)
    {
        std::remove(temporaryPath.c_str());
        aError = "Failed to write device metadata";
        return false;
    }

    if (std::rename(temporaryPath.c_str(), mPath.c_str()) != 0)
    {
        std::remove(temporaryPath.c_str());
        aError = "Failed to replace device metadata";
        return false;
    }

    return true;
}

} // namespace rest
} // namespace otbr
