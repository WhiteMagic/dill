#include "di_listener.h"

#include <memory>
#include <string.h>
#include <sstream>

#include "spdlog/spdlog.h"
#include "spdlog/sinks/stdout_color_sinks.h" 
#include "spdlog/sinks/basic_file_sink.h"


#define HID_CLASSGUID {0x4d1e55b2, 0xf16f, 0x11cf,{ 0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}}
#define CLS_NAME TEXT("GremlinInputListener")
#define HWND_MESSAGE ((HWND)-3)


namespace spd = spdlog;

// Setup logger
//auto logger = spd::stdout_color_mt("console");
auto logger = spd::basic_logger_mt("debug", "debug.txt");

// DirectInput system handle
LPDIRECTINPUT8 g_direct_input = nullptr;

// Size of the device object read buffer
static const int g_buffer_size = 64;

// Storage for device data
static DeviceDataStore g_data_store;

// Callback handles
JoystickInputEventCallback g_event_callback = nullptr;
DeviceChangeCallback g_device_change_callback = nullptr;

// Thread handles
HANDLE g_message_thread = NULL;
HANDLE g_joystick_thread = NULL;


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
        if(w_param == DBT_DEVICEARRIVAL)
        {
            //logger->info("Device connected:    {}", path);
            enumerate_devices();
        }
        else if(w_param == DBT_DEVICEREMOVECOMPLETE)
        {
            //logger->info("Device disconnected: {}", path);
            enumerate_devices();
        }
    }

    return TRUE;
}

LRESULT window_proc(HWND window_hdl, UINT msg_type, WPARAM w_param, LPARAM l_param)
{
    // Window has not yet been created
    if(msg_type == WM_NCCREATE)
    {
        return true;
    }
    // Create the window
    else if(msg_type == WM_CREATE)
    {
        on_create_window(window_hdl, l_param);
    }
    // Device change event
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

    // Figure out the type
    if(data.dwOfs < FIELD_OFFSET(DIJOYSTATE2, rgdwPOV))
    {
        evt.input_type = JoystickInputType::Axis;
        evt.input_index = axis_id_lookup[data.dwOfs];
        evt.value = data.dwData;
        g_data_store.state[guid].axis[evt.input_index] = evt.value;

        logger->info(
            "{}: Axis   {} value={}",
            guid_to_string(guid),
            evt.input_index,
            evt.value
        );
    }
    else if(data.dwOfs < FIELD_OFFSET(DIJOYSTATE2, rgbButtons))
    {
        evt.input_type = JoystickInputType::Hat;
        evt.input_index = hat_id_lookup[data.dwOfs];
        evt.value = data.dwData;
        g_data_store.state[guid].hat[evt.input_index] = evt.value;

        logger->info(
            "{}: Hat    {:d} direction={}",
            guid_to_string(guid),
            evt.input_index,
            evt.value
        );
    }
    else if(data.dwOfs < FIELD_OFFSET(DIJOYSTATE2, lVX))
    {
        evt.input_type = JoystickInputType::Button;
        evt.input_index = static_cast<UINT8>(data.dwOfs - FIELD_OFFSET(DIJOYSTATE2, rgbButtons) + 1);
        evt.value = (data.dwData & 0x0080) == 0 ? 0 : 1;
        g_data_store.state[guid].button[evt.input_index] = evt.value == 0 ? false : true;

        logger->info(
            "{}: Button {:d} pressed={}",
            guid_to_string(guid),
            evt.input_index,
            evt.value
        );
    }
    else
    {
        logger->warn(
            "{}: Unexpected type of input event occurred",
            guid_to_string(guid)
        );
    }

    if(g_event_callback != nullptr)
    {
        g_event_callback(evt);
    }
}

void process_buffered_events(LPDIRECTINPUTDEVICE8 instance, GUID const& guid)
{
    // Retrieve buffered data
    DIDEVICEOBJECTDATA device_data[g_buffer_size]; 
    DWORD object_count = g_buffer_size;

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
                    "{}: {}",
                    guid_to_string(guid),
                    error_to_string(result)
                );
            }
        }
        else
        {
            logger->error(
                "{}: {}",
                guid_to_string(guid),
                error_to_string(result)
            );
            object_count = 0;
        }
    }
}

DWORD WINAPI joystick_update_thread(LPVOID l_param)
{
    while(true)
    {
        {
            std::lock_guard<std::mutex> lock(g_data_store.mutex);
            for(auto & entry : g_data_store.device_map)
            {
                process_buffered_events(entry.second, entry.first);
            }
        }
        SleepEx(4, false);
    }

    return 0;
}

DWORD WINAPI message_handler_thread(LPVOID l_param)
{
    // Initialize the window to receive messages through
    HWND hWnd = create_window();
    if(hWnd == NULL)
    {
        logger->critical("Could not create message window!");
        throw std::runtime_error("Could not create message window!");
    }

    // Start the message loop
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

BOOL CALLBACK extract_axis_data(LPCDIDEVICEOBJECTINSTANCE lpddoi, LPVOID pvRef)
{
    DeviceSummary * info = reinterpret_cast<DeviceSummary*>(pvRef);
    if(lpddoi->dwType & DIDFT_AXIS)
    {
        AxisData data;
        data.linear_index = info->axis_count;
        data.axis_index = DIDFT_GETINSTANCE(lpddoi->dwType);
        strcpy_s(data.name, MAX_PATH, lpddoi->tszName);
        info->axis_data[info->axis_count] = data;
        info->axis_count++;
    }
    assert(info->axis_count <= 8);
    return DIENUM_CONTINUE;
}

void initialize_device(GUID guid, std::string name)
{
    // Check if we have an existing instance in the device map
    auto execute_callback = true;
    {
        std::lock_guard<std::mutex> lock(g_data_store.mutex);
        if(g_data_store.device_map.find(guid) != g_data_store.device_map.end())
        {
            execute_callback = false;
            auto device = g_data_store.device_map[guid];
            auto result = device->Unacquire();
            if(FAILED(result))
            {
                logger->error(
                    "{}: Failed unacquiring device, {}",
                    guid_to_string(guid),
                    error_to_string(result)
                );
            }
            g_data_store.device_map.erase(guid);
        }
    }

    // Create joystick device
    LPDIRECTINPUTDEVICE8 device = nullptr;
    auto result = g_direct_input->CreateDevice(
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

    // Store device in the data storage
    {
        std::lock_guard<std::mutex> lock(g_data_store.mutex);
        g_data_store.device_map[guid] = device;
    }

    // Setting cooperation level
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

    // Set data format for reports
    result = device->SetDataFormat(&c_dfDIJoystick2);
    if(FAILED(result))
    {
        logger->error(
            "{}: Error while setting data format, {}",
            guid_to_string(guid),
            error_to_string(result)
        );
    }
 
    // Set properties
    DIPROPDWORD prop_word;
    DIPROPHEADER prop_header;
    prop_header.dwSize = sizeof(DIPROPDWORD);
    prop_header.dwHeaderSize = sizeof(DIPROPHEADER);
    prop_header.dwObj = 0;
    prop_header.dwHow = DIPH_DEVICE;
    prop_word.diph = prop_header;
    prop_word.dwData = g_buffer_size;
    
    result = device->SetProperty(
         DIPROP_BUFFERSIZE,
         &prop_header
    );
    if(FAILED(result))
    {
        logger->error(
            "{}: Error while setting device properties, {}",
            guid_to_string(guid),
            error_to_string(result)
        );
    }

    // Acquire device
    result = device->Acquire();
    if(FAILED(result))
    {
        logger->error(
            "{}: Failed to acquire the device, {}",
            guid_to_string(guid),
            error_to_string(result)
        );
    }

    /*
    DIPROPRANGE result;
    device->GetProperty(DIPROP_LOGICALRANGE, &result.diph);

    logger->info("{:d} {:d}", result.lMin, result.lMax);
    */

    // Query device capabilities
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

    // Create device summary report
    DeviceSummary info;
    info.device_guid = guid;
    info.action = DeviceActionType::Connected;
    strcpy_s(info.name, MAX_PATH, name.c_str());
    info.axis_count = 0;
    for(int i=0; i<8; ++i)
    {
        info.axis_data[i].linear_index = 0;
        info.axis_data[i].axis_index = 0;
        info.axis_data[i].name[0] = '\0';
    }

    auto axis_indices = used_axis_indices(guid);
    for(size_t i=0; i<axis_indices.size(); ++i)
    {
        info.axis_data[i].linear_index = i+1;
        info.axis_data[i].axis_index = axis_indices[i];
    }

    info.axis_count = capabilities.dwAxes;
    info.button_count = capabilities.dwButtons;
    info.hat_count = capabilities.dwPOVs;

    g_data_store.cache[guid] = info;
    if(g_device_change_callback != nullptr && execute_callback)
    {
        g_device_change_callback(info);
    }
}

BOOL CALLBACK handle_device_cb(LPCDIDEVICEINSTANCE instance, LPVOID data)
{
    // Convert user data pointer to data storage device
    std::unordered_map<GUID, bool>* current_devices = 
        reinterpret_cast<std::unordered_map<GUID, bool>*>(data);

    logger->info(
        "{}: Processing device: {}",
        guid_to_string(instance->guidInstance),
        std::string(instance->tszProductName)
    );

    // Aggregate device information
    (*current_devices)[instance->guidInstance] = true;
    initialize_device(
        instance->guidInstance,
        std::string(instance->tszInstanceName)
    );

    // Continue to enumerate devices
    return DIENUM_CONTINUE;
}

void enumerate_devices()
{
    // Register with the DirectInput system, creating an instance to
    // interface with it
    if(g_direct_input == nullptr)
    {
        auto result = DirectInput8Create(
            GetModuleHandle(nullptr),
            DIRECTINPUT_VERSION,
            IID_IDirectInput8,
            (VOID**)&g_direct_input,
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
    }

    std::unordered_map<GUID, bool> current_devices;
    auto result = g_direct_input->EnumDevices(
        DI8DEVCLASS_GAMECTRL,
        handle_device_cb,
        &current_devices,
        DIEDFL_ATTACHEDONLY
    );
    if(FAILED(result))
    {
        logger->error("Failure occured while discovering devices, {}", error_to_string(result));
    }

    // Get rid of devices we no longer have from the global map
    std::lock_guard<std::mutex> lock(g_data_store.mutex);
    std::vector<GUID> guid_to_remove;
    for(auto const& entry : g_data_store.device_map)
    {
        if(current_devices.find(entry.first) == current_devices.end())
        {
            guid_to_remove.push_back(entry.first);
        }
    }
    for(auto const& guid : guid_to_remove)
    {
        logger->info("{}: Removing device", guid_to_string(guid));
        g_data_store.device_map.erase(guid);

        // Emit DeviceInformation
        DeviceSummary di;
        di.device_guid = guid;
        di.action = DeviceActionType::Disconnected;
        strcpy_s(di.name, MAX_PATH, "Unknown");
        if(g_data_store.cache.find(guid) != g_data_store.cache.end())
        {
            strcpy_s(di.name, MAX_PATH, g_data_store.cache[guid].name);
        }
        if(g_device_change_callback != nullptr)
        {
            g_device_change_callback(di);
        }
    }
}

BOOL init()
{
    // Start joystick update loop thread
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

    // Start joystick update loop thread
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

    // Force an update of device enumeration to bootstrap everything
    enumerate_devices();

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

    return g_data_store.state[guid].axis[index];
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

    return g_data_store.state[guid].button[index];
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

    return g_data_store.state[guid].hat[index];
}

std::vector<int> used_axis_indices(GUID guid)
{
    DIJOYSTATE2 state;
    g_data_store.device_map[guid]->Poll();
    auto result = g_data_store.device_map[guid]->GetDeviceState(
        sizeof(state),
        &state
    );

    if(FAILED(result))
    {
        logger->critical("Failed determining used axes indices.");
        return {};
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