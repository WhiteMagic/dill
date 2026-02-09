#include "dill.h"

#include <atomic>
#include <iostream>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string.h>
#include <sstream>

#include "spdlog/spdlog.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/sinks/rotating_file_sink.h"
#include "spdlog/sinks/basic_file_sink.h"

#include "libguarded/cs_cow_guarded.h"
#include "libguarded/cs_shared_guarded.h"


#define HID_CLASSGUID { 0x4d1e55b2, 0xf16f, 0x11cf, { 0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30} }
#define CLS_NAME TEXT("GremlinInputListener")
#define HWND_MESSAGE ((HWND)-3)


namespace spd = spdlog;
namespace lg = libguarded;

// Setup log file rotation to limit maximum file size used.
static const int k_max_log_size = 1024 * 1024 * 2;
auto fixed_size_sink = std::make_shared<spd::sinks::rotating_file_sink_mt>(
    "dill_debug.log",
    k_max_log_size,
    /* rotated files count */ 1
);
auto logger = std::make_shared<spd::logger>("debug", fixed_size_sink);
//auto logger = spd::stdout_color_mt("console");

// DirectInput system handle.
lg::shared_guarded<LPDIRECTINPUT8, std::recursive_mutex> g_direct_input{nullptr};

// Size of the device object read buffer.
static const int g_buffer_size = 256;

// Storage for device related data structures, wrapped in guards to prevent
// multi-threading issues.
static lg::shared_guarded<
    std::unordered_map<GUID, DeviceState>, std::shared_mutex> g_state_store{};
static lg::shared_guarded<
    DeviceMetaDataStore, std::shared_mutex> g_meta_data_store{};
static lg::shared_guarded<
    std::unordered_map<GUID, DeviceSummary>,
    std::shared_mutex> g_summary_store{};

// Callback handles.
JoystickInputEventCallback g_event_callback = nullptr;
DeviceChangeCallback g_device_change_callback = nullptr;

// Thread handles.
HANDLE g_message_thread = NULL;
HANDLE g_joystick_thread = NULL;

// Flag indicating whether or not device initialization is complete.
std::atomic<bool> g_initialization_done = false;


// Default constructor for a DeviceState structure.
DeviceState::DeviceState()
    :   axis(9, 0)
      , button(128, false)
      , hat(4, -1)
{}


std::string error_to_string(DWORD error_code)
{
    static const std::unordered_map<DWORD, std::string> lut{
        // Success codes
        {DI_OK, "The operation completed successfully (DI_OK)"},
        {S_FALSE, "S_FALSE"},
        {DI_BUFFEROVERFLOW, "The device buffer overflowed.  Some input was lost. (DI_BUFFEROVERFLOW)"},
        {DI_PROPNOEFFECT, "The change in device properties had no effect. (DI_PROPNOEFFECT)"},
        // Error codes
        {E_HANDLE, "Invalid handle (E_HANDLE)"},
        {DIERR_INVALIDPARAM, "An invalid parameter was passed to the returning function, or the object was not in a state that admitted the function to be called (DIERR_INVALIDPARAM)"},
        {DIERR_NOTINITIALIZED, "This object has not been initialized (DIERR_NOTINITIALIZED)"},
        {DIERR_OTHERAPPHASPRIO, "Another app has a higher priority level, preventing this call from succeeding. (DIERR_OTHERAPPHASPRIO)"},
        {DIERR_ACQUIRED, "The operation cannot be performed while the device is acquired. (DIERR_ACQUIRED)"},
        {DIERR_DEVICENOTREG, "The device or device instance or effect is not registered with DirectInput. (DIERR_DEVICENOTREG)"},
        {DIERR_INPUTLOST, "Access to the device has been lost. It must be re-acquired. (DIERR_INPUTLOST)"},
        {DIERR_NOTACQUIRED, "The operation cannot be performed unless the device is acquired. (DIERR_NOTACQUIRED)"},
        {DIERR_NOTBUFFERED, "Attempted to read buffered device data from a device that is not buffered. (DIERR_NOTBUFFERED)"},
        {DIERR_NOINTERFACE, "The specified interface is not supported by the object (DIERR_NOINTERFACE)"},
        {DIERR_OBJECTNOTFOUND, "The requested object does not exist. (DIERR_OBJECTNOTFOUND)"},
        {DIERR_UNSUPPORTED, "The function called is not supported at this time (DIERR_UNSUPPORTED)"},
        {DI_POLLEDDEVICE, "The device is a polled device.  As a result, device buffering will not collect any data and event notifications will not be signalled until GetDeviceState is called. (DI_POLLEDDEVICE)"}
    };

    auto itr = lut.find(error_code);
    if(itr != lut.end())
    {
        return itr->second;
    }
    else
    {
        return "Unknown error code";
    }
}

std::string guid_to_string(GUID guid)
{
    LPOLESTR guid_string;
    StringFromCLSID(guid, &guid_string);

    std::wstring unicode_string(guid_string);
    CoTaskMemFree(guid_string);

    return std::string(unicode_string.begin(), unicode_string.end());
}

BOOL on_create_window(HWND window_hdl, LPARAM l_param)
{
    LPCREATESTRUCT params = reinterpret_cast<LPCREATESTRUCT>(l_param);
    GUID interface_class_guid = *(reinterpret_cast<GUID*>(params->lpCreateParams));
    DEV_BROADCAST_DEVICEINTERFACE notification_filter;
    ZeroMemory(&notification_filter, sizeof(notification_filter));
    notification_filter.dbcc_size = sizeof(DEV_BROADCAST_DEVICEINTERFACE);
    notification_filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    notification_filter.dbcc_classguid = interface_class_guid;
    HDEVNOTIFY dev_notify = RegisterDeviceNotification(
        window_hdl,
        &notification_filter,
        DEVICE_NOTIFY_WINDOW_HANDLE
    );

    if(dev_notify == NULL)
    {
        logger->critical("Could not register for devicenotifications!");
        throw std::runtime_error("Could not register for device notifications!");
    }

    return TRUE;
}

BOOL on_device_change(LPARAM l_param, WPARAM w_param)
{
    PDEV_BROADCAST_HDR lpdb = reinterpret_cast<PDEV_BROADCAST_HDR>(l_param);
    PDEV_BROADCAST_DEVICEINTERFACE lpdbv = reinterpret_cast<PDEV_BROADCAST_DEVICEINTERFACE>(lpdb);
    std::string path;
    if(lpdb->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE)
    {
        path = std::string(lpdbv->dbcc_name);
        if(w_param == DBT_DEVICEARRIVAL || w_param == DBT_DEVICEREMOVECOMPLETE)
        {
            enumerate_devices();
        }
    }

    return TRUE;
}

LRESULT window_proc(HWND window_hdl, UINT msg_type, WPARAM w_param, LPARAM l_param)
{
    // Window has not yet been created.
    if(msg_type == WM_NCCREATE)
    {
        return true;
    }
    // Create the window.
    else if(msg_type == WM_CREATE)
    {
        on_create_window(window_hdl, l_param);
    }
    // Device change event.
    else if(msg_type == WM_DEVICECHANGE)
    {
        on_device_change(l_param, w_param);
    }

    return 0;
}

void emit_joystick_input_event(DIDEVICEOBJECTDATA const& data, GUID const& guid)
{
    JoystickInputData evt;
    evt.device_guid = guid;

    static std::unordered_map<DWORD, int> axis_id_lookup =
    {
        {FIELD_OFFSET(DIJOYSTATE2, lX), 1},
        {FIELD_OFFSET(DIJOYSTATE2, lY), 2},
        {FIELD_OFFSET(DIJOYSTATE2, lZ), 3},
        {FIELD_OFFSET(DIJOYSTATE2, lRx), 4},
        {FIELD_OFFSET(DIJOYSTATE2, lRy), 5},
        {FIELD_OFFSET(DIJOYSTATE2, lRz), 6},
        {FIELD_OFFSET(DIJOYSTATE2, rglSlider[0]), 7},
        {FIELD_OFFSET(DIJOYSTATE2, rglSlider[1]), 8}
    };
    static std::unordered_map<DWORD, int> hat_id_lookup =
    {
        {FIELD_OFFSET(DIJOYSTATE2, rgdwPOV[0]), 1},
        {FIELD_OFFSET(DIJOYSTATE2, rgdwPOV[1]), 2},
        {FIELD_OFFSET(DIJOYSTATE2, rgdwPOV[2]), 3},
        {FIELD_OFFSET(DIJOYSTATE2, rgdwPOV[3]), 4}
    };

    {
        // Figure out the input type.
        auto store_handle = g_state_store.lock();
        if(data.dwOfs < FIELD_OFFSET(DIJOYSTATE2, rgdwPOV))
        {
            evt.input_type = JoystickInputType::Axis;
            evt.input_index = axis_id_lookup[data.dwOfs];
            evt.value = data.dwData;
            (*store_handle)[guid].axis[evt.input_index] = evt.value;
        }
        else if(data.dwOfs < FIELD_OFFSET(DIJOYSTATE2, rgbButtons))
        {
            evt.input_type = JoystickInputType::Hat;
            evt.input_index = hat_id_lookup[data.dwOfs];
            evt.value = data.dwData;
            (*store_handle)[guid].hat[evt.input_index] = evt.value;
        }
        else if(data.dwOfs < FIELD_OFFSET(DIJOYSTATE2, lVX))
        {
            evt.input_type = JoystickInputType::Button;
            evt.input_index = static_cast<UINT8>(data.dwOfs - FIELD_OFFSET(DIJOYSTATE2, rgbButtons) + 1);
            evt.value = (data.dwData & 0x0080) == 0 ? 0 : 1;
            (*store_handle)[guid].button[evt.input_index] = evt.value == 0 ? false : true;
        }
        else
        {
            logger->warn(
                "{}: Unexpected type of input event occurred",
                guid_to_string(guid)
            );
        }
    }

    if(g_event_callback != nullptr)
    {
        g_event_callback(evt);
    }
}

bool process_buffered_events(LPDIRECTINPUTDEVICE8 instance, GUID const& guid)
{
    // Poll device to get things going.
    auto result = instance->Poll();
    if(FAILED(result))
    {
        logger->error(
            "{} Polling failed, {}",
            guid_to_string(guid),
            error_to_string(result)
        );

        instance->Acquire();
        instance->Poll();
    }

    // Retrieve buffered data.
    DIDEVICEOBJECTDATA device_data[g_buffer_size];
    DWORD object_count = g_buffer_size;

    bool device_requires_polling = false;
    while(object_count == g_buffer_size)
    {
        auto result = instance->GetDeviceData(
            sizeof(DIDEVICEOBJECTDATA),
            device_data,
            &object_count,
            0
        );
        if(SUCCEEDED(result))
        {
            if(object_count > 0)
            {
                for(size_t i=0; i<object_count; ++i)
                {
                    emit_joystick_input_event(device_data[i], guid);
                }
            }
            if(result == DI_BUFFEROVERFLOW)
            {
                logger->error(
                    "Buffer overflow on device: {} - {}",
                    guid_to_string(guid),
                    error_to_string(result)
                );
            }
        }
        else
        {
            logger->error(
                "Failed to retrieve buffered data on device: {} - {}",
                guid_to_string(guid),
                error_to_string(result)
            );
            object_count = 0;

            // If this failure arose due to buffered reading not being possible
            // revert the device to polled mode.
            if(result == DIERR_NOTBUFFERED)
            {
                logger->error(
                    "{} Failed reading device in buffered mode, falling back to polling, {}",
                    guid_to_string(guid),
                    error_to_string(result)
                );
                device_requires_polling = true;
            }
        }
    }
    return device_requires_polling;
}

void poll_device(LPDIRECTINPUTDEVICE8 instance, GUID const& guid)
{
    // Poll device to update internal state.
    auto result = instance->Poll();
    if(FAILED(result))
    {
        logger->error(
            "{} Polling failed, {}",
            guid_to_string(guid),
            error_to_string(result)
        );

        instance->Acquire();
        instance->Poll();
    }

    // Obtain device state.
    DIJOYSTATE2 state;
    result = instance->GetDeviceState(sizeof(DIJOYSTATE2), &state);
    if(FAILED(result))
    {
        logger->error(
            "{} Retrieving device state failed, {}",
            guid_to_string(guid),
            error_to_string(result)
        );
        return;
    }

    // Event structure to use with the callback.
    JoystickInputData evt;
    evt.device_guid = guid;

    // Lock required data stores.
    auto const& device_summary = (*g_summary_store.lock_shared()).at(guid);
    auto & device_state = (*g_state_store.lock())[guid];

    // Detect and handle axis state changes.
    for(size_t i=0; i<device_summary.axis_count; ++i)
    {
        auto axis_index = device_summary.axis_map[i].axis_index;
        auto value = 0;
        if     (axis_index == 1) { value = state.lX; }
        else if(axis_index == 2) { value = state.lY; }
        else if(axis_index == 3) { value = state.lZ; }
        else if(axis_index == 4) { value = state.lRx; }
        else if(axis_index == 5) { value = state.lRy; }
        else if(axis_index == 6) { value = state.lRz; }
        else if(axis_index == 7) { value = state.rglSlider[0]; }
        else if(axis_index == 8) { value = state.rglSlider[1]; }

        if(device_state.axis[axis_index] != value)
        {
            evt.input_type = JoystickInputType::Axis;
            evt.input_index = static_cast<UINT8>(axis_index);
            evt.value = value;
            device_state.axis[axis_index] = value;

            if(g_event_callback != nullptr)
            {
                g_event_callback(evt);
            }
        }
    }

    // Detect and handle button state changes.
    for(size_t i=0; i<device_summary.button_count; ++i)
    {
        auto is_pressed = (state.rgbButtons[i] & 0x0080) == 0 ? false : true;
        if(device_state.button[i+1] != is_pressed)
        {
            evt.input_type = JoystickInputType::Button;
            evt.input_index = static_cast<UINT8>(i + 1);
            evt.value = is_pressed;
            device_state.button[evt.input_index] = is_pressed;

            if(g_event_callback != nullptr)
            {
                g_event_callback(evt);
            }
        }
    }

    // Detect and handle hat state changes.
    for(size_t i=0; i<device_summary.hat_count; ++i)
    {
        LONG direction = state.rgdwPOV[i];
        if(direction < 0 || direction > 36000)
        {
            direction = -1;
        }
        if(device_state.hat[i+1] != direction)
        {
            evt.input_type = JoystickInputType::Hat;
            evt.input_index = static_cast<UINT8>(i+1);
            evt.value = direction;
            device_state.hat[evt.input_index] = evt.value;

            if(g_event_callback != nullptr)
            {
                g_event_callback(evt);
            }
        }
    }
}

DWORD WINAPI joystick_update_thread(LPVOID l_param)
{
    while(true)
    {
        if(g_initialization_done)
        {
            auto meta_store_handle = g_meta_data_store.lock();
            for(auto & entry : (*meta_store_handle).device_map)
            {
                if(!(*meta_store_handle).is_ready[entry.first])
                {
                    logger->info(
                        "Skipping device {}, not yet fully initialized",
                        guid_to_string(entry.first)
                    );
                    continue;
                }

                if((*meta_store_handle).is_buffered[entry.first])
                {
                    auto requires_polling = process_buffered_events(
                        entry.second,
                        entry.first
                    );
                    if (requires_polling)
                    {
                        (*meta_store_handle).is_buffered[entry.first] = false;
                    }
                }
                else
                {
                    poll_device(entry.second, entry.first);
                }
            }
        }
        SleepEx(4, false);
    }

    return 0;
}

DWORD WINAPI message_handler_thread(LPVOID l_param)
{
    // Initialize the window to receive messages through.
    HWND hWnd = create_window();
    if(hWnd == NULL)
    {
        logger->critical("Could not create message window!");
        throw std::runtime_error("Could not create message window!");
    }

    // Start the message loop.
    MSG msg;
    while(GetMessage(&msg, NULL, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return 0;
}

HWND create_window()
{
    WNDCLASSEX wx;
    ZeroMemory(&wx, sizeof(wx));

    wx.cbSize = sizeof(WNDCLASSEX);
    wx.lpfnWndProc = reinterpret_cast<WNDPROC>(window_proc);
    wx.hInstance = reinterpret_cast<HINSTANCE>(GetModuleHandle(0));
    wx.style = CS_HREDRAW | CS_VREDRAW;
    wx.hInstance = GetModuleHandle(0);
    wx.hbrBackground = (HBRUSH)(COLOR_WINDOW);
    wx.lpszClassName = CLS_NAME;

    GUID guid = HID_CLASSGUID;
    HWND window_hdl = NULL;
    if(RegisterClassEx(&wx))
    {
        window_hdl = CreateWindow(
            CLS_NAME,
            TEXT("DevNotifWnd"),
            WS_ICONIC,
            0,
            0,
            CW_USEDEFAULT,
            0,
            HWND_MESSAGE,
            NULL,
            GetModuleHandle(0),
            reinterpret_cast<void*>(&guid)
        );
    }

    return window_hdl;
}

BOOL CALLBACK set_axis_range(LPCDIDEVICEOBJECTINSTANCE lpddoi, LPVOID pvRef)
{
    LPDIRECTINPUTDEVICE8 device = reinterpret_cast<LPDIRECTINPUTDEVICE8>(pvRef);
    if(lpddoi->dwType & DIDFT_AXIS)
    {
        DIPROPRANGE range;

        ZeroMemory(&range, sizeof(range));
        range.diph.dwSize = sizeof(range);
        range.diph.dwHeaderSize = sizeof(range.diph);
        range.diph.dwObj = lpddoi->dwType;
        range.diph.dwHow = DIPH_BYID;
        range.lMin = -32768;
        range.lMax = 32767;

        auto result = device->SetProperty(DIPROP_RANGE, &range.diph);
        if(FAILED(result))
        {
            logger->error(
                "Error while setting axis range, {}",
                error_to_string(result)
            );
            return DIENUM_CONTINUE;
        }
    }
    return DIENUM_CONTINUE;
}

void initialize_device(GUID guid, std::string name)
{
    // Acquire meta information database lock.
    auto handle_meta_store = g_meta_data_store.lock();

    // Prevent any operations on this device until initialization is done.
    (*handle_meta_store).is_ready[guid] = false;

    // Check if we have an existing instance in the device map.
    auto execute_callback = true;
    {
        if((*handle_meta_store).device_map.find(guid) != (*handle_meta_store).device_map.end())
        {
            execute_callback = false;
            auto device = (*handle_meta_store).device_map[guid];
            auto result = device->Unacquire();
            if(FAILED(result))
            {
                logger->error(
                    "{}: Failed unacquiring device, {}",
                    guid_to_string(guid),
                    error_to_string(result)
                );
            }
            (*handle_meta_store).device_map.erase(guid);
        }
    }

    // Create joystick device.
    LPDIRECTINPUTDEVICE8 device = nullptr;
    auto di_handle = g_direct_input.lock();
    auto result = (*di_handle)->CreateDevice(
        guid,
        &device,
        nullptr
    );
    if(FAILED(result))
    {
        logger->error(
            "{}: Failed creating device, {}",
            guid_to_string(guid),
            error_to_string(result)
        );
    }

    // Store device in the data storage.
    (*handle_meta_store).device_map[guid] = device;

    // Setting cooperation level.
    result = device->SetCooperativeLevel(
        NULL,
        DISCL_NONEXCLUSIVE | DISCL_BACKGROUND
    );
    if(FAILED(result))
    {
        logger->error(
            "{}: Failed setting cooperative level, {}",
            guid_to_string(guid),
            error_to_string(result)
        );
    }

    // Set data format for reports.
    result = device->SetDataFormat(&c_dfDIJoystick2);
    if(FAILED(result))
    {
        logger->error(
            "{}: Error while setting data format, {}",
            guid_to_string(guid),
            error_to_string(result)
        );
    }

    // Set device buffer size property.
    DIPROPDWORD prop_word;
    ZeroMemory(&prop_word, sizeof(DIPROPDWORD));
    prop_word.diph.dwSize = sizeof(DIPROPDWORD);
    prop_word.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    prop_word.diph.dwObj = 0;
    prop_word.diph.dwHow = DIPH_DEVICE;
    prop_word.dwData = g_buffer_size;
    // By default assume the device supports buffered reading and revert
    // to polled upon failure.
    (*handle_meta_store).is_buffered[guid] = true;
    result = device->SetProperty(DIPROP_BUFFERSIZE, &prop_word.diph);
    if(FAILED(result))
    {
        logger->error(
            "{}: Error while setting device properties, {}",
            guid_to_string(guid),
            error_to_string(result)
        );
    }
    if(result == DI_POLLEDDEVICE)
    {
        logger->warn("Device {} is not buffered", guid_to_string(guid));
        (*handle_meta_store).is_buffered[guid] = false;
    }

    // Acquire device.
    result = device->Acquire();
    if(FAILED(result))
    {
        logger->error(
            "{}: Failed to acquire the device, {}",
            guid_to_string(guid),
            error_to_string(result)
        );
    }

    // Query device capabilities.
    DIDEVCAPS capabilities;
    capabilities.dwSize = sizeof(DIDEVCAPS);
    result = device->GetCapabilities(&capabilities);
    if(FAILED(result))
    {
        logger->error(
            "{}: Failed to obtain device capabilities, {}",
            guid_to_string(guid),
            error_to_string(result)
        );
    }

    // Create device summary report.
    DeviceSummary info;
    info.device_guid = guid;
    info.vendor_id = get_vendor_id(device, guid);
    info.product_id = get_product_id(device, guid);
    info.joystick_id = get_joystick_id(device, guid);
    strcpy_s(info.name, MAX_PATH, name.c_str());
    info.axis_count = 0;
    for(int i=0; i<8; ++i)
    {
        info.axis_map[i].linear_index = 0;
        info.axis_map[i].axis_index = 0;
    }

    auto axis_indices = used_axis_indices(device);

    // Do some error checking on axis counts.
    if(axis_indices.size() > 8)
    {
        logger->error(
            "{} {}: Invalid number of axis reported, {} > 8",
            info.name,
            guid_to_string(info.device_guid),
            axis_indices.size()
        );
        axis_indices.resize(8);
    }
    if(capabilities.dwAxes > 8)
    {
        logger->error(
            "{} {}: Reports more then 8 axis, {}",
            info.name,
            guid_to_string(info.device_guid),
            capabilities.dwAxes
        );
    }

    // Handle all the various ways in which device can misreport device axes
    // information.
    // 1. dwAxes reports more then 8 axes simply discard dwAxes data an only use
    //    axis_indices.
    // 2. dwAxes and axis_indices value disagree and dwAxes is > 0 and < 9 while
    //    axis_info is empty, hope for the best and assume we have dwAxes linear
    //    axes present and fix axis_information.

    // There is something wrong with the reported axis counts, many ways to
    // fix the discrepancy.
    if(axis_indices.size() != capabilities.dwAxes)
    {
        if(capabilities.dwAxes > 0 && capabilities.dwAxes < 9 && axis_indices.size() == 0)
        {
            // No axis map data enumerated but valid looking axis count
            // reported, lets hope these are all one after the other.
            info.axis_count = capabilities.dwAxes;
            for(size_t i=0; i<info.axis_count; ++i)
            {
                info.axis_map[i].linear_index = i+1;
                info.axis_map[i].axis_index = i+1;
            }

            logger->warn(
                "{} {}: Axis information, invalid hoping for the best, capabilities={} enumerated={}",
                info.name,
                guid_to_string(info.device_guid),
                capabilities.dwAxes,
                axis_indices.size()
            );
        }

        else
        {
            // Some other invalid axis count information returned, simply trust
            // the axis enumeration.
            info.axis_count = axis_indices.size();
            for(size_t i=0; i<axis_indices.size(); ++i)
            {
                info.axis_map[i].linear_index = i+1;
                info.axis_map[i].axis_index = axis_indices[i];
            }
            logger->warn(
                "{} {}: Overriding reported number of axes,  capabilities={} enumerated={}",
                info.name,
                guid_to_string(info.device_guid),
                capabilities.dwAxes,
                axis_indices.size()
            );
        }

    }
    // Both axis counts agree, so we'll just use those.
    else
    {
        info.axis_count = capabilities.dwAxes;
        for(size_t i=0; i<axis_indices.size(); ++i)
        {
            info.axis_map[i].linear_index = i+1;
            info.axis_map[i].axis_index = axis_indices[i];
        }
    }


    info.button_count = capabilities.dwButtons;
    info.hat_count = capabilities.dwPOVs;

    // Write device summary to debug file.
    logger->info("Device summary: {} {}", info.name,guid_to_string(guid));
    logger->info("Axis={} Buttons={} Hats={}", info.axis_count, info.button_count, info.hat_count);
    logger->info("Axis map");
    for(auto const& entry : info.axis_map)
    {
        logger->info("  linear={} id={}", entry.linear_index, entry.axis_index);
    }


    // Add device to list of active guids.
    bool add_guid = true;
    for(size_t i=0; i<(*handle_meta_store).active_guids.size(); ++i)
    {
        if((*handle_meta_store).active_guids[i] == guid)
        {
            add_guid = false;
            break;
        }
    }
    if(add_guid)
    {
        (*handle_meta_store).active_guids.push_back(guid);
    }

    // Set the axis range for each axis of the device.
    device->EnumObjects(set_axis_range, device, DIDFT_ALL);

    {
        auto handle_summary_store = g_summary_store.lock();
        (*handle_summary_store)[guid] = info;
    }
    if(g_device_change_callback != nullptr && execute_callback)
    {
        g_device_change_callback(info, DeviceActionType::Connected);
    }

    // Allow operating on the device.
    (*handle_meta_store).is_ready[guid] = true;
}

BOOL CALLBACK handle_device_cb(LPCDIDEVICEINSTANCE instance, LPVOID data)
{
    // Convert user data pointer to data storage device.
    std::unordered_map<GUID, bool>* current_devices =
        reinterpret_cast<std::unordered_map<GUID, bool>*>(data);

    logger->info(
        "{}: Processing device: {}",
        guid_to_string(instance->guidInstance),
        std::string(instance->tszProductName)
    );

    // Aggregate device information.
    (*current_devices)[instance->guidInstance] = true;
    initialize_device(
        instance->guidInstance,
        std::string(instance->tszInstanceName)
    );

    // Continue to enumerate devices.
    return DIENUM_CONTINUE;
}

void enumerate_devices()
{
    g_initialization_done = false;
    std::unordered_map<GUID, bool> current_devices;

    {
    // Register with the DirectInput system, creating an instance to
    // interface with it.
    auto di_handle = g_direct_input.lock();
    if(*di_handle == nullptr)
    {
        LPDIRECTINPUT8 tmp_device = nullptr;
        auto result = DirectInput8Create(
            GetModuleHandle(nullptr),
            DIRECTINPUT_VERSION,
            IID_IDirectInput8,
            (VOID**)&tmp_device,
            nullptr
        );
        if(FAILED(result))
        {
            logger->critical(
                "Failed registering with DirectInput, {}",
                error_to_string(result)
            );
            throw std::runtime_error("Failed registering with DirectInput");
        }
        *di_handle = tmp_device;
    }

    auto result = (*di_handle)->EnumDevices(
        DI8DEVCLASS_GAMECTRL,
        handle_device_cb,
        &current_devices,
        DIEDFL_ATTACHEDONLY
    );
    if(FAILED(result))
    {
        logger->error("Failure occured while discovering devices, {}", error_to_string(result));
    }
    }

    // Get rid of devices we no longer have from the global map.
    std::vector<GUID> guid_to_remove;
    auto handle_meta_store = g_meta_data_store.lock();
    auto handle_summary_store = g_summary_store.lock();
    for(auto const& entry : (*handle_meta_store).device_map)
    {
        if(current_devices.find(entry.first) == current_devices.end())
        {
            guid_to_remove.push_back(entry.first);
        }
    }
    for(auto const& guid : guid_to_remove)
    {
        logger->info("{}: Removing device", guid_to_string(guid));
        (*handle_meta_store).device_map.erase(guid);

        // Emit DeviceInformation, copy existing device data if we know about
        // the device and have an existing record, otherwise return a shell.
        DeviceSummary di;
        if(handle_summary_store->contains(guid))
        {
            di = (*handle_summary_store)[guid];
        }
        else
        {
            di.device_guid = guid;
            strcpy_s(di.name, MAX_PATH, "Unknown");
        }

        // Remove guid from list of active ones.
        for(size_t i=0; i<(*handle_meta_store).active_guids.size(); ++i)
        {
            if((*handle_meta_store).active_guids[i] == guid)
            {
                (*handle_meta_store).active_guids.erase(
                    (*handle_meta_store).active_guids.begin() + i
                );
                break;
            }
        }

        if(g_device_change_callback != nullptr)
        {
            g_device_change_callback(di, DeviceActionType::Disconnected);
        }
    }

    g_initialization_done = true;
}

BOOL init()
{
    g_initialization_done = false;
    logger->info("Initializing DILL v1.4");

    // Force an update of device enumeration to bootstrap everything.
    enumerate_devices();

    // Start joystick update loop thread.
    g_joystick_thread = CreateThread(
            NULL,
            0,
            joystick_update_thread,
            NULL,
            0,
            NULL
    );
    if(g_joystick_thread == NULL)
    {
        logger->error("Creating joystick thread failed {}", GetLastError());
        return FALSE;
    }

    // Start joystick update loop thread.
    g_message_thread = CreateThread(
            NULL,
            0,
            message_handler_thread,
            NULL,
            0,
            NULL
    );
    if(g_message_thread == NULL)
    {
        logger->error(
            "Creating message handler thread failed {}",
            GetLastError()
        );
        return FALSE;
    }

    g_initialization_done = true;
    return TRUE;
}

void set_input_event_callback(JoystickInputEventCallback cb)
{
    logger->info("Setting event callback");
    g_event_callback = cb;
}

void set_device_change_callback(DeviceChangeCallback cb)
{
    logger->info("Setting device change callback");
    g_device_change_callback = cb;
}

DeviceSummary get_device_information_by_index(size_t index)
{
    auto meta_store_handle = g_meta_data_store.lock_shared();
    if(index >= (*meta_store_handle).active_guids.size())
    {
        return DeviceSummary();
    }
    auto guid = (*meta_store_handle).active_guids[index];
    return get_device_information_by_guid(guid);
}

DeviceSummary get_device_information_by_guid(GUID guid)
{
    auto summary_handle = g_summary_store.lock();
    if(summary_handle->find(guid) == summary_handle->end())
    {
        return DeviceSummary();
    }

    return (*summary_handle)[guid];
}


size_t get_device_count()
{
    return (*g_meta_data_store.lock_shared()).active_guids.size();
}

bool device_exists(GUID guid)
{
    bool exists = false;
    auto meta_store_handle = g_meta_data_store.lock_shared();
    for(auto const& dev_guid : (*meta_store_handle).active_guids)
    {
        if(dev_guid == guid)
        {
            exists = true;
        }
    }

    return exists;
}

LONG get_axis(GUID guid, DWORD index)
{
    if(index < 1 || index > 8)
    {
        logger->error(
            "{}: Requested invalid axis index {}",
            guid_to_string(guid),
            index
        );
        return 0;
    }

    return (*g_state_store.lock_shared()).at(guid).axis[index];
}

bool get_button(GUID guid, DWORD index)
{
    if(index < 0 || index >= 128)
    {
        logger->error(
            "{}: Requested invalid button index {}",
            guid_to_string(guid),
            index
        );
        return false;
    }

    return (*g_state_store.lock_shared()).at(guid).button[index];
}

LONG get_hat(GUID guid, DWORD index)
{
    if(index < 0 || index >= 4)
    {
        logger->error(
            "{}: Requested invalid hat index {}",
            guid_to_string(guid),
            index
        );
        return -1;
    }

    return (*g_state_store.lock_shared()).at(guid).hat[index];
}

std::vector<int> used_axis_indices(LPDIRECTINPUTDEVICE8 device)
{
    DIJOYSTATE2 state;
    {
        device->Poll();
        auto result = device->GetDeviceState(
            sizeof(state),
            &state
        );

        if(FAILED(result))
        {
            logger->critical("Failed determining used axes indices.");
            return {};
        }
    }

    std::vector<int> used_indices;
    if(state.lX != 0)           { used_indices.push_back(1); }
    if(state.lY != 0)           { used_indices.push_back(2); }
    if(state.lZ != 0)           { used_indices.push_back(3); }
    if(state.lRx != 0)          { used_indices.push_back(4); }
    if(state.lRy != 0)          { used_indices.push_back(5); }
    if(state.lRz != 0)          { used_indices.push_back(6); }
    if(state.rglSlider[0] != 0) { used_indices.push_back(7); }
    if(state.rglSlider[1] != 0) { used_indices.push_back(8); }

    return used_indices;
}

DWORD get_vendor_id(LPDIRECTINPUTDEVICE8 device, GUID guid)
{
    DIPROPDWORD data;

    ZeroMemory(&data, sizeof(DIPROPDWORD));
    data.diph.dwSize = sizeof(DIPROPDWORD);
    data.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    data.diph.dwObj = 0;
    data.diph.dwHow = DIPH_DEVICE;

    auto result = device->GetProperty(
        DIPROP_VIDPID,
        &data.diph
    );

    if(FAILED(result))
    {
        logger->critical(
            "{} Failed retrieving joystick vendor id data, {}",
            guid_to_string(guid),
            error_to_string(result)
        );
        return 0;
    }

    return LOWORD(data.dwData);
}

DWORD get_product_id(LPDIRECTINPUTDEVICE8 device, GUID guid)
{
    DIPROPDWORD data;

    ZeroMemory(&data, sizeof(DIPROPDWORD));
    data.diph.dwSize = sizeof(DIPROPDWORD);
    data.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    data.diph.dwObj = 0;
    data.diph.dwHow = DIPH_DEVICE;

    auto result = device->GetProperty(
        DIPROP_VIDPID,
        &data.diph
    );

    if(FAILED(result))
    {
        logger->critical(
            "{} Failed retrieving joystick product id data, {}",
            guid_to_string(guid),
            error_to_string(result)
        );
        return 0;
    }

    return HIWORD(data.dwData);
}

DWORD get_joystick_id(LPDIRECTINPUTDEVICE8 device, GUID guid)
{
    DIPROPDWORD data;

    ZeroMemory(&data, sizeof(DIPROPDWORD));
    data.diph.dwSize = sizeof(DIPROPDWORD);
    data.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    data.diph.dwObj = 0;
    data.diph.dwHow = DIPH_DEVICE;

    auto result = device->GetProperty(
        DIPROP_JOYSTICKID,
        &data.diph
    );

    if(FAILED(result))
    {
        logger->critical(
            "{} Failed retrieving joystick id data, {}",
            guid_to_string(guid),
            error_to_string(result)
        );
        return 0;
    }

    return data.dwData;
}
