#include "di_listener.h"
#include "spdlog/fmt/fmt.h"

#include <iostream>
#include <unordered_map>


std::unordered_map<GUID, DeviceSummary> g_device_info;


std::string type_to_str(JoystickInputType type)
{
    if(type == JoystickInputType::Axis)
    {
        return "Axis";
    }
    else if(type == JoystickInputType::Button)
    {
        return "Button";
    }
    else if(type == JoystickInputType::Hat)
    {
        return "Hat";
    }
    else
    {
        return "Unknown";
    }
}

void event_callback(JoystickInputData data)
{
    std::cout
        << fmt::format("{:30s}", g_device_info[data.device_guid].name) << " "
        << fmt::format("{:6s}", type_to_str(static_cast<JoystickInputType>(data.input_type))) << " "
        << fmt::format("{: 3d}", static_cast<int>(data.input_index)) << " "
        << fmt::format("{: 6d}", data.value)
        << std::endl;
}

void device_change_callback(DeviceSummary info)
{
    if(info.action == DeviceActionType::Disconnected)
    {
        std::cout << fmt::format(
            "{:12s}: {:30s} {} {} {} {}",
            "Disconnected",
            info.name,
            guid_to_string(info.device_guid),
            info.vendor_id,
            info.product_id,
            info.joystick_id
        ) << std::endl;
        return;
    }

    g_device_info[info.device_guid] = info;

    std::cout << fmt::format(
        "{:12s}: {:30s} {} {:X} {:X} {}",
        "Connected",
        info.name,
        guid_to_string(info.device_guid),
        info.vendor_id,
        info.product_id,
        info.joystick_id
    ) << std::endl;
}

int main(int argc, char *argv[])
{
    set_input_event_callback(event_callback);
    set_device_change_callback(device_change_callback);
    init();

    while(true)
    {
        SleepEx(1000, 0);
    }

    return 0;
}