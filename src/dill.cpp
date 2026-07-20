#include "dill.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <iostream>
#include <memory>
#include <mutex>
#include <string.h>
#include <sstream>
#include <thread>

#include <objbase.h>

#include "axis_mapping.h"
#include "spdlog/spdlog.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/sinks/rotating_file_sink.h"
#include "spdlog/sinks/basic_file_sink.h"

namespace spd = spdlog;

#define HID_CLASSGUID {0x4d1e55b2, 0xf16f, 0x11cf, {0x88, 0xcb, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}}
#define CLS_NAME TEXT("DirectInputListenerLibrary")
#define HWND_MESSAGE ((HWND)-3)

// Setup logger with file rotation.
static const int k_max_log_size = 1024 * 1024 * 2;
auto fixed_size_sink = std::make_shared<spd::sinks::rotating_file_sink_mt>(
    "dill_debug.log",
    k_max_log_size,
    /* rotated files count */ 1
);
auto logger = std::make_shared<spd::logger>("debug", fixed_size_sink);
//auto logger = spd::stdout_color_mt("console");

// DirectInput system handle used in the event loop thread.
LPDIRECTINPUT8 g_direct_input = nullptr;

// Size of the device object read buffer.
static const int g_buffer_size = 64;

// Storage for device data, access guarded by g_data_store_mutex.
static DeviceDataStore g_data_store;
static std::mutex g_data_store_mutex;

// Callback handles, provided by client code and called from the event thread.
std::atomic<JoystickInputEventCallback> g_event_callback{nullptr};
std::atomic<DeviceChangeCallback> g_device_change_callback{nullptr};

// Handle for window and device notification messages.
static HWND g_hwnd = nullptr;
static HDEVNOTIFY g_device_notify = nullptr;

// Event handles to control the MsgWaitForMultipleObjectsEx processing loop.
static HANDLE g_quit_event = nullptr;
static HANDLE g_rebuild_event = nullptr;
static HANDLE g_hotplug_event = nullptr;
static HANDLE g_startup_done_event = nullptr;
static std::atomic<bool> g_startup_failed{false};

// Status flag.
static std::atomic<bool> g_running{false};

// A joinable std::thread's destructor calls std::terminate(). If
// shutdown() was never called, detaching here avoids that crash when the
// process exits.
struct LoopThreadGuard
{
    std::thread thread;
    ~LoopThreadGuard()
    {
        if(thread.joinable())
        {
            logger->warn(
                "shutdown() was never called; detaching the event "
                "loop thread to avoid a crash on exit"
            );
            logger->flush();
            thread.detach();
        }
    }
};
static LoopThreadGuard g_loop;


namespace
{
    // Context used with EnumObjects to manage axis detection.
    struct AxisEnumContext
    {
        LPDIRECTINPUTDEVICE8        device;
        DWORD                       slider_count;
        std::vector<AxisOffset>     detected_offsets;
    };

    // Lookup table for axis GUID to axis_index.
    const std::unordered_map<GUID, DWORD> g_axis_guid_lookup =
    {
        {GUID_XAxis,  1},
        {GUID_YAxis,  2},
        {GUID_ZAxis,  3},
        {GUID_RxAxis, 4},
        {GUID_RyAxis, 5},
        {GUID_RzAxis, 6},
    };

    // Resolves axis_index to its DIJOYSTATE2 offset, logging and returning
    // false if axis_index does not name a known axis.
    bool try_get_axis_offset(DWORD axis_index, AxisOffset& out_offset)
    {
        out_offset = offset_for_axis_index(axis_index);
        if(out_offset == static_cast<AxisOffset>(-1))
        {
            logger->error("No DIJOYSTATE2 offset for axis_index {}", axis_index);
            return false;
        }
        return true;
    }

    // Helper function to close and clean up all still active control events.
    void close_control_events()
    {
        if(g_quit_event != nullptr)
        {
            CloseHandle(g_quit_event);
            g_quit_event = nullptr;
        }
        if(g_rebuild_event != nullptr)
        {
            CloseHandle(g_rebuild_event);
            g_rebuild_event = nullptr;
        }
        if(g_hotplug_event != nullptr)
        {
            CloseHandle(g_hotplug_event);
            g_hotplug_event = nullptr;
        }
        if(g_startup_done_event != nullptr)
        {
            CloseHandle(g_startup_done_event);
            g_startup_done_event = nullptr;
        }
    }

    // Closes the control events unless dismiss() is called first. Used by
    // init() so every failure path (including exceptions thrown by
    // std::thread's constructor) closes any events it already created,
    // instead of leaking them when the globals get overwritten by the next
    // init() attempt.
    struct ControlEventGuard
    {
        bool dismissed = false;
        void dismiss() { dismissed = true; }
        ~ControlEventGuard()
        {
            if(!dismissed)
            {
                close_control_events();
            }
        }
    };

    // Explicitly brackets COM apartment lifetime on the event loop thread.
    // DirectInput8Create/EnumDevices use COM interfaces; without an explicit
    // CoInitializeEx/CoUninitialize pair, a fresh apartment gets implicitly
    // initialized on first use and its teardown on thread exit is not
    // guaranteed to be prompt - since a new OS thread is created for every
    // init()/shutdown() cycle, that showed up as steadily growing process
    // handle counts across repeated cycles.
    struct COMInitializer
    {
        HRESULT result;
        COMInitializer() : result(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
        ~COMInitializer()
        {
            if(SUCCEEDED(result))
            {
                CoUninitialize();
            }
        }
    };
}


DeviceState::DeviceState()
    :   axis(9, 0)
      , button(129, false)
      , hat(5, -1)
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
    g_device_notify = dev_notify;

    return TRUE;
}

BOOL on_device_change(LPARAM l_param, WPARAM w_param)
{
    // WM_DEVICECHANGE is delivered via SendMessage, so this can run
    // reentrantly from inside MsgWaitForMultipleObjectsEx on the event loop
    // thread (see g_hotplug_event's comment). Only signal the event here -
    // the actual enumerate_devices() call happens from the loop's own
    // top-level iteration, never nested inside this callback.
    PDEV_BROADCAST_HDR lpdb = reinterpret_cast<PDEV_BROADCAST_HDR>(l_param);
    if(lpdb->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE)
    {
        if(w_param == DBT_DEVICEARRIVAL || w_param == DBT_DEVICEREMOVECOMPLETE)
        {
            SetEvent(g_hotplug_event);
        }
    }

    return TRUE;
}

LRESULT window_proc(
    HWND                                window_hdl,
    UINT                                msg_type,
    WPARAM                              w_param,
    LPARAM                              l_param
)
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

    static std::unordered_map<DWORD, int> hat_id_lookup =
    {
        {FIELD_OFFSET(DIJOYSTATE2, rgdwPOV[0]), 1},
        {FIELD_OFFSET(DIJOYSTATE2, rgdwPOV[1]), 2},
        {FIELD_OFFSET(DIJOYSTATE2, rgdwPOV[2]), 3},
        {FIELD_OFFSET(DIJOYSTATE2, rgdwPOV[3]), 4}
    };

    // Figure out the event's input type based on the DIJOYSTATE2 data
    // structure's offset.
    if(data.dwOfs < FIELD_OFFSET(DIJOYSTATE2, rgdwPOV))
    {
        const DWORD axis_index = axis_index_for_offset(data.dwOfs);
        if(axis_index == 0 || axis_index > 8)
        {
            logger->error(
                "{}: Received axis event for unrecognized offset {}",
                guid_to_string(guid),
                data.dwOfs
            );
            return;
        }

        evt.input_type = JoystickInputType::Axis;
        evt.input_index = static_cast<UINT8>(axis_index);
        evt.value = data.dwData;
        {
            std::lock_guard<std::mutex> lock(g_data_store_mutex);
            g_data_store.state[guid].axis[evt.input_index] = evt.value;
        }
    }
    else if(data.dwOfs < FIELD_OFFSET(DIJOYSTATE2, rgbButtons))
    {
        evt.input_type = JoystickInputType::Hat;
        evt.input_index = hat_id_lookup[data.dwOfs];
        evt.value = data.dwData;
        {
            std::lock_guard<std::mutex> lock(g_data_store_mutex);
            g_data_store.state[guid].hat[evt.input_index] = evt.value;
        }
    }
    else if(data.dwOfs < FIELD_OFFSET(DIJOYSTATE2, lVX))
    {
        evt.input_type = JoystickInputType::Button;
        evt.input_index = static_cast<UINT8>(
            data.dwOfs - FIELD_OFFSET(DIJOYSTATE2, rgbButtons) + 1
        );
        evt.value = (data.dwData & 0x0080) == 0 ? 0 : 1;
        {
            std::lock_guard<std::mutex> lock(g_data_store_mutex);
            g_data_store.state[guid].button[evt.input_index] =
                evt.value == 0 ? false : true;
        }
    }
    else
    {
        logger->warn(
            "{}: Unexpected type of input event occurred",
            guid_to_string(guid)
        );
    }

    auto callback = g_event_callback.load();
    if(callback != nullptr)
    {
        callback(evt);
    }
}

void process_buffered_events(LPDIRECTINPUTDEVICE8 instance, GUID const& guid)
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
                "Failed to retrieve buffere data on device: {} - {}",
                guid_to_string(guid),
                error_to_string(result)
            );
            object_count = 0;

            // If this failure arose due to buffered reading not being possible
            // revert the device to polled mode.
            if(result == DIERR_NOTBUFFERED)
            {
                logger->error(
                    "{} Failed reading device in buffered mode, falling back "
                    "to polling, {}",
                    guid_to_string(guid),
                    error_to_string(result)
                );

                // Remove the event notification handle of this device as it
                // no longer operates in buffered mode.
                HANDLE old_event = nullptr;
                {
                    std::lock_guard<std::mutex> lock(g_data_store_mutex);
                    g_data_store.is_buffered[guid] = false;
                    const auto it = g_data_store.event_handles.find(guid);
                    if(it != g_data_store.event_handles.end())
                    {
                        old_event = it->second;
                        g_data_store.event_handles.erase(it);
                    }
                }
                // Resetthe handle and force a refresh of all handles.
                if(old_event != nullptr)
                {
                    instance->SetEventNotification(nullptr);
                    CloseHandle(old_event);
                    SetEvent(g_rebuild_event);
                }
            }
        }
    }
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

    // Gather changed values while acquiring the lock once, then emit callbacks
    // afterward without the lock.
    auto callback = g_event_callback.load();
    std::vector<JoystickInputData> change_events;

    {
        std::lock_guard<std::mutex> lock(g_data_store_mutex);

        // Detect axis state changes.
        for(size_t i=0; i<g_data_store.cache[guid].axis_count; ++i)
        {
            auto axis_index = g_data_store.cache[guid].axis_map[i].axis_index;
            AxisOffset offset;
            if(!try_get_axis_offset(axis_index, offset))
            {
                continue;
            }
            LONG value = *reinterpret_cast<LONG const*>(
                reinterpret_cast<char const*>(&state) + offset
            );

            if(g_data_store.state[guid].axis[axis_index] != value)
            {
                g_data_store.state[guid].axis[axis_index] = value;

                JoystickInputData evt;
                evt.device_guid = guid;
                evt.input_type = JoystickInputType::Axis;
                evt.input_index = static_cast<UINT8>(axis_index);
                evt.value = value;
                change_events.push_back(evt);
            }
        }

        // Detect button state changes.
        for(size_t i=0; i<g_data_store.cache[guid].button_count; ++i)
        {
            auto is_pressed = (state.rgbButtons[i] & 0x0080) == 0 ? false : true;
            if(g_data_store.state[guid].button[i+1] != is_pressed)
            {
                g_data_store.state[guid].button[i+1] = is_pressed;

                JoystickInputData evt;
                evt.device_guid = guid;
                evt.input_type = JoystickInputType::Button;
                evt.input_index = static_cast<UINT8>(i + 1);
                evt.value = is_pressed;
                change_events.push_back(evt);
            }
        }

        // Detect hat state changes.
        for(size_t i=0; i<g_data_store.cache[guid].hat_count; ++i)
        {
            LONG direction = state.rgdwPOV[i];
            if(direction < 0 || direction > 36000)
            {
                direction = -1;
            }
            if(g_data_store.state[guid].hat[i+1] != direction)
            {
                g_data_store.state[guid].hat[i+1] = direction;

                JoystickInputData evt;
                evt.device_guid = guid;
                evt.input_type = JoystickInputType::Hat;
                evt.input_index = static_cast<UINT8>(i+1);
                evt.value = direction;
                change_events.push_back(evt);
            }
        }
    }

    // Emit events via the callback in quick succession without the lock.
    if(callback != nullptr)
    {
        for(auto const& evt : change_events)
        {
            callback(evt);
        }
    }
}

void rebuild_wait_handles(
    std::array<HANDLE, k_max_wait_handles>& handles,
    DWORD&                              handle_count,
    std::vector<GUID>&                  handle_guids,
    bool&                               periodic_service_needed
)
{
    // Create a copy of the mapping between GUID and HANDLE for each buffered
    // device before processing this without a lock.
    std::vector<std::pair<GUID, HANDLE>> buffered_snapshot;
    bool any_polled = false;
    {
        std::lock_guard<std::mutex> lock(g_data_store_mutex);
        for(auto const& [guid, buffered] : g_data_store.is_buffered)
        {
            if(buffered)
            {
                const auto it = g_data_store.event_handles.find(guid);
                if(it != g_data_store.event_handles.end())
                {
                    buffered_snapshot.emplace_back(guid, it->second);
                }
            }
            else
            {
                any_polled = true;
            }
        }
    }

    handle_guids.clear();
    handle_count = k_control_handle_count;
    for(auto const& [guid, event] : buffered_snapshot)
    {
        // If more devices are connected than we support, log an error message
        // and ignore it.
        if(handle_count >= k_max_wait_handles)
        {
            logger->error(
                "{}: more buffered devices than the {} supported wait "
                "slots; this device will be ignored.",
                guid_to_string(guid),
                k_max_wait_handles - k_control_handle_count
            );
            continue;
        }
        handles[handle_count] = event;
        handle_guids.push_back(guid);
        ++handle_count;
    }
    periodic_service_needed = any_polled;
}

void event_loop_main()
{
    try
    {
        COMInitializer com_initializer;
        if(FAILED(com_initializer.result))
        {
            logger->warn("CoInitializeEx failed, {}", com_initializer.result);
        }

        // Create window and setup device notification.
        HWND hwnd = create_window();
        if(hwnd == NULL)
        {
            logger->critical("Could not create message window!");
            g_startup_failed = true;
            SetEvent(g_startup_done_event);
            return;
        }
        g_hwnd = hwnd;

        // Enumerate devices and then indicate startup as being done.
        enumerate_devices();
        SetEvent(g_startup_done_event);

        std::array<HANDLE, k_max_wait_handles> handles;
        std::vector<GUID> handle_guids;
        DWORD handle_count = k_control_handle_count;
        bool periodic_needed = false;
        handles[0] = g_quit_event;
        handles[1] = g_rebuild_event;
        handles[2] = g_hotplug_event;
        rebuild_wait_handles(handles, handle_count, handle_guids, periodic_needed);

        // Reused across WAIT_TIMEOUT ticks (which can fire every 1ms) so
        // steady-state polling doesn't reallocate on every iteration.
        std::vector<std::pair<GUID, LPDIRECTINPUTDEVICE8>> to_poll;

        for(;;)
        {
            DWORD timeout = periodic_needed ? 1 : INFINITE;
            DWORD wait_result = MsgWaitForMultipleObjectsEx(
                handle_count,
                handles.data(),
                timeout,
                QS_ALLINPUT,
                MWMO_INPUTAVAILABLE
            );

            // Quit requested.
            if(wait_result == WAIT_OBJECT_0)
            {
                break;
            }
            // Rebuild the wait array, typically due to device hotplug.
            else if(wait_result == WAIT_OBJECT_0 + 1)
            {
                ResetEvent(g_rebuild_event);
                rebuild_wait_handles(
                    handles,
                    handle_count,
                    handle_guids,
                    periodic_needed
                );
            }
            // Hotplug notification deferred by on_device_change().
            else if(wait_result == WAIT_OBJECT_0 + 2)
            {
                // Run the blocking enumeration of devices here in the main
                // event loop.
                ResetEvent(g_hotplug_event);
                enumerate_devices();

                // Rebuild the wait array to be in sync with device hotplug.
                ResetEvent(g_rebuild_event);
                rebuild_wait_handles(
                    handles,
                    handle_count,
                    handle_guids,
                    periodic_needed
                );
            }
            else if(
                wait_result > WAIT_OBJECT_0 + 2 &&
                wait_result < WAIT_OBJECT_0 + handle_count
            )
            {
                // A single buffered device signaled.
                size_t slot = wait_result - WAIT_OBJECT_0;
                GUID guid = handle_guids[slot - k_control_handle_count];
                LPDIRECTINPUTDEVICE8 device = nullptr;
                {
                    std::lock_guard<std::mutex> lock(g_data_store_mutex);
                    const auto it = g_data_store.device_map.find(guid);
                    if(it != g_data_store.device_map.end())
                    {
                        device = it->second;
                    }
                }
                if(device != nullptr)
                {
                    process_buffered_events(device, guid);
                }
            }
            else if(wait_result == WAIT_OBJECT_0 + handle_count)
            {
                // Window message queue has input, e.g. WM_DEVICECHANGE.
                MSG msg;
                while(PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE))
                {
                    TranslateMessage(&msg);
                    DispatchMessage(&msg);
                }
            }
            else if(wait_result == WAIT_TIMEOUT)
            {
                // Process polled-fallback devices.
                to_poll.clear();
                {
                    std::lock_guard<std::mutex> lock(g_data_store_mutex);
                    for(auto& [guid, device] : g_data_store.device_map)
                    {
                        if(!g_data_store.is_ready[guid])
                        {
                            continue;
                        }
                        if(!g_data_store.is_buffered[guid])
                        {
                            to_poll.emplace_back(guid, device);
                        }
                    }
                }
                for(auto& [guid, device] : to_poll)
                {
                    poll_device(device, guid);
                }
            }
            else
            {
                logger->critical(
                    "MsgWaitForMultipleObjectsEx failed, {}",
                    GetLastError()
                );
                break;
            }
        }
    }
    catch(std::exception const& e)
    {
        logger->critical(
            "event_loop_main terminated by exception: {}",
            e.what()
        );
        g_startup_failed = true;
        SetEvent(g_startup_done_event);
    }
    catch(...)
    {
        logger->critical("event_loop_main terminated by unknown exception");
        g_startup_failed = true;
        SetEvent(g_startup_done_event);
    }
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

BOOL CALLBACK enumerate_axis_objects(
    LPCDIDEVICEOBJECTINSTANCE       lpddoi,
    LPVOID                          pvRef
)
{
    AxisEnumContext* ctx = reinterpret_cast<AxisEnumContext*>(pvRef);

    if(lpddoi->guidType == GUID_Slider)
    {
        // There should only ever be two sliders, any more will be logged and
        // ignored.
        if (ctx->slider_count < 2)
        {
            AxisOffset offset;
            if(try_get_axis_offset(7 + ctx->slider_count, offset))
            {
                ctx->detected_offsets.push_back(offset);
            }
            ++ctx->slider_count;
        }
        else
        {
            logger->warn(
                "More than two sliders detected, ignoring additional ones.")
            ;
        }
    }
    else
    {
        const auto it = g_axis_guid_lookup.find(lpddoi->guidType);
        if(it != g_axis_guid_lookup.end())
        {
            AxisOffset offset;
            if(try_get_axis_offset(it->second, offset))
            {
                ctx->detected_offsets.push_back(offset);
            }
        }
    }

    DIPROPRANGE range;
    ZeroMemory(&range, sizeof(range));
    range.diph.dwSize = sizeof(range);
    range.diph.dwHeaderSize = sizeof(range.diph);
    range.diph.dwObj = lpddoi->dwType;
    range.diph.dwHow = DIPH_BYID;
    range.lMin = -32768;
    range.lMax = 32767;

    auto result = ctx->device->SetProperty(DIPROP_RANGE, &range.diph);
    if(FAILED(result))
    {
        logger->error(
            "Error while setting axis range, {}",
            error_to_string(result)
        );
    }

    return DIENUM_CONTINUE;
}

void initialize_device(GUID guid, std::string name)
{
    // Prevent any operations on this device until initialization is done.
    {
        std::lock_guard<std::mutex> lock(g_data_store_mutex);
        g_data_store.is_ready[guid] = false;
    }

    // Create joystick device.
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
        return;
    }
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
    // Set device buffer size property. By default assume the device supports
    // buffered reading and revert to polled upon failure.
    DIPROPDWORD prop_word;
    prop_word.diph.dwSize = sizeof(DIPROPDWORD);
    prop_word.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    prop_word.diph.dwObj = 0;
    prop_word.diph.dwHow = DIPH_DEVICE;
    prop_word.dwData = g_buffer_size;
    bool buffered = true;
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
        buffered = false;
    }

    // For buffered devices, wire up event-driven notification before
    // acquiring the device. SetEventNotification fails with DIERR_ACQUIRED
    // on an already-acquired device. Either step failing degrades this
    // device to polled rather than aborting initialization.
    HANDLE new_event = nullptr;
    if(buffered)
    {
        new_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if(new_event == nullptr)
        {
            logger->error(
                "{}: Failed creating notification event, {}",
                guid_to_string(guid),
                GetLastError()
            );
            buffered = false;
        }
        else
        {
            result = device->SetEventNotification(new_event);
            if(FAILED(result))
            {
                logger->error(
                    "{}: Failed setting event notification, {}",
                    guid_to_string(guid),
                    error_to_string(result)
                );
                CloseHandle(new_event);
                new_event = nullptr;
                buffered = false;
            }
        }
    }

    // Acquire the device.
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

    // Detect present axes via object enumeration and set each one's range
    // in the same pass.
    AxisEnumContext axis_ctx;
    axis_ctx.device = device;
    axis_ctx.slider_count = 0;
    device->EnumObjects(enumerate_axis_objects, &axis_ctx, DIDFT_AXIS);

    build_axis_map(axis_ctx.detected_offsets, info.axis_count, info.axis_map);

    if(capabilities.dwAxes > 8)
    {
        logger->error(
            "{} {}: Reports more than 8 axes, {}",
            info.name,
            guid_to_string(info.device_guid),
            capabilities.dwAxes
        );
    }
    if(info.axis_count != capabilities.dwAxes)
    {
        logger->warn(
            "{} {}: Axis count mismatch, enumerated={} capabilities={}",
            info.name,
            guid_to_string(info.device_guid),
            info.axis_count,
            capabilities.dwAxes
        );
    }

    info.button_count = capabilities.dwButtons;
    info.hat_count = capabilities.dwPOVs;

    // Write device summary to debug file.
    logger->info("Device summary: {} {}", info.name,guid_to_string(guid));
    logger->info(
        "Axis={} Buttons={} Hats={}",
        info.axis_count,
        info.button_count,
        info.hat_count
    );
    logger->info("Axis map");
    for(auto const& entry : info.axis_map)
    {
        logger->info("  linear={} id={}", entry.linear_index, entry.axis_index);
    }


    // Write everything gathered above into the data store in one section.
    {
        std::lock_guard<std::mutex> lock(g_data_store_mutex);
        g_data_store.device_map[guid] = device;
        g_data_store.is_buffered[guid] = buffered;
        if(new_event != nullptr)
        {
            g_data_store.event_handles[guid] = new_event;
        }

        if(
            std::find(
                g_data_store.active_guids.begin(),
                g_data_store.active_guids.end(),
                guid
            ) == g_data_store.active_guids.end()
        )
        {
            g_data_store.active_guids.push_back(guid);
        }

        g_data_store.cache[guid] = info;
    }

    if(new_event != nullptr)
    {
        SetEvent(g_rebuild_event);
    }
    auto device_change_callback = g_device_change_callback.load();
    if(device_change_callback != nullptr)
    {
        device_change_callback(info, DeviceActionType::Connected);
    }

    // Allow operating on the device.
    {
        std::lock_guard<std::mutex> lock(g_data_store_mutex);
        g_data_store.is_ready[guid] = true;
    }
}

BOOL CALLBACK handle_device_cb(LPCDIDEVICEINSTANCE instance, LPVOID data)
{
    // Convert user data pointer to data storage device.
    std::unordered_map<GUID, bool>* current_devices =
        reinterpret_cast<std::unordered_map<GUID, bool>*>(data);

    (*current_devices)[instance->guidInstance] = true;

    // Devices we've already initialized and that are still attached are
    // left alone.
    {
        std::lock_guard<std::mutex> lock(g_data_store_mutex);
        if(g_data_store.device_map.find(instance->guidInstance) !=
           g_data_store.device_map.end())
        {
            return DIENUM_CONTINUE;
        }
    }

    logger->info(
        "{}: Processing device: {}",
        guid_to_string(instance->guidInstance),
        std::string(instance->tszProductName)
    );

    initialize_device(
        instance->guidInstance,
        std::string(instance->tszInstanceName)
    );

    // Continue to enumerate devices.
    return DIENUM_CONTINUE;
}

void enumerate_devices()
{
    // Register with the DirectInput system, creating an instance to
    // interface with it.
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
        logger->error(
            "Failure occured while discovering devices, {}",
            error_to_string(result)
        );
    }

    // Get rid of devices we no longer have from the global map.
    std::vector<GUID> guid_to_remove;
    {
        std::lock_guard<std::mutex> lock(g_data_store_mutex);
        for(auto const& [guid, device] : g_data_store.device_map)
        {
            if(current_devices.find(guid) == current_devices.end())
            {
                guid_to_remove.push_back(guid);
            }
        }
    }

    bool any_buffered_removed = false;
    for(auto const& guid : guid_to_remove)
    {
        logger->info("{}: Removing device", guid_to_string(guid));

        LPDIRECTINPUTDEVICE8 device = nullptr;
        HANDLE event_handle = nullptr;
        DeviceSummary di;
        {
            std::lock_guard<std::mutex> lock(g_data_store_mutex);
            auto device_it = g_data_store.device_map.find(guid);
            if(device_it != g_data_store.device_map.end())
            {
                device = device_it->second;
                g_data_store.device_map.erase(device_it);
            }

            auto event_it = g_data_store.event_handles.find(guid);
            if(event_it != g_data_store.event_handles.end())
            {
                event_handle = event_it->second;
                g_data_store.event_handles.erase(event_it);
            }

            // Emit DeviceInformation, copy existing device data if we know
            // about the device and have an existing record, otherwise
            // return an empty shell.
            auto cache_it = g_data_store.cache.find(guid);
            if(cache_it != g_data_store.cache.end())
            {
                di = cache_it->second;
            }
            else
            {
                di.device_guid = guid;
                strcpy_s(di.name, MAX_PATH, "Unknown");
            }

            // Drop every remaining per-device entries so a guid that never
            // reconnects doesn't linger in these maps indefinitely.
            g_data_store.cache.erase(guid);
            g_data_store.state.erase(guid);
            g_data_store.is_ready.erase(guid);
            g_data_store.is_buffered.erase(guid);

            // Remove guid from list of active ones.
            const auto it = std::find(
                g_data_store.active_guids.begin(),
                g_data_store.active_guids.end(),
                guid
            );
            if(it != g_data_store.active_guids.end())
            {
                g_data_store.active_guids.erase(it);
            }
        }

        if(device != nullptr)
        {
            device->SetEventNotification(nullptr);
            device->Unacquire();
            device->Release();
        }
        if(event_handle != nullptr)
        {
            CloseHandle(event_handle);
            any_buffered_removed = true;
        }

        auto device_change_callback = g_device_change_callback.load();
        if(device_change_callback != nullptr)
        {
            device_change_callback(di, DeviceActionType::Disconnected);
        }
    }

    if(any_buffered_removed && g_rebuild_event != nullptr)
    {
        SetEvent(g_rebuild_event);
    }
}

BOOL init()
{
    try
    {
        bool expected = false;
        if(!g_running.compare_exchange_strong(expected, true))
        {
            logger->warn("init() called while already running; ignoring");
            return TRUE;
        }

        logger->info("Initializing DILL v1.5");
        g_startup_failed = false;

        // Closes any control events created below unless dismissed on the
        // success path.
        ControlEventGuard control_event_guard;

        g_quit_event = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        g_rebuild_event = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        g_hotplug_event = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        g_startup_done_event = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        if(g_quit_event == nullptr ||
           g_rebuild_event == nullptr ||
           g_hotplug_event == nullptr ||
           g_startup_done_event == nullptr)
        {
            logger->critical(
                "Failed to create control events, {}",
                GetLastError()
            );
            g_running = false;
            return FALSE;
        }

        g_loop.thread = std::thread(event_loop_main);

        // Block until the event loop thread has completed the startup sequence.
        WaitForSingleObject(g_startup_done_event, INFINITE);

        if(g_startup_failed)
        {
            logger->critical("DILL startup failed");
            SetEvent(g_quit_event);
            if(g_loop.thread.joinable())
            {
                g_loop.thread.join();
            }
            g_running = false;
            return FALSE;
        }

        control_event_guard.dismiss();
        return TRUE;
    }
    catch(std::exception const& e)
    {
        logger->critical("init() failed: {}", e.what());
        g_running = false;
        return FALSE;
    }
    catch(...)
    {
        logger->critical("init() failed: unknown exception");
        g_running = false;
        return FALSE;
    }
}

BOOL shutdown()
{
    try
    {
        bool expected = true;
        if(!g_running.compare_exchange_strong(expected, false))
        {
            logger->info("shutdown() called while not running; ignoring");
            return FALSE;
        }

        logger->info("Shutting down DILL");

        SetEvent(g_quit_event);
        if(g_hwnd != nullptr)
        {
            PostMessage(g_hwnd, WM_NULL, 0, 0);
        }

        if(g_loop.thread.joinable())
        {
            g_loop.thread.join();
        }

        // Cohesive cleanup of all device/event handles.
        std::vector<LPDIRECTINPUTDEVICE8> devices_to_release;
        std::vector<HANDLE> events_to_close;
        {
            std::lock_guard<std::mutex> lock(g_data_store_mutex);
            devices_to_release.reserve(g_data_store.device_map.size());
            for(auto& [guid, device] : g_data_store.device_map)
            {
                devices_to_release.push_back(device);
            }
            events_to_close.reserve(g_data_store.event_handles.size());
            for(auto& [guid, event] : g_data_store.event_handles)
            {
                events_to_close.push_back(event);
            }
            g_data_store.device_map.clear();
            g_data_store.event_handles.clear();
            g_data_store.is_buffered.clear();
            g_data_store.cache.clear();
            g_data_store.state.clear();
            g_data_store.is_ready.clear();
            g_data_store.active_guids.clear();
        }
        for(auto device : devices_to_release)
        {
            device->Unacquire();
            device->SetEventNotification(nullptr);
            device->Release();
        }
        for(auto event_handle : events_to_close)
        {
            CloseHandle(event_handle);
        }

        if(g_device_notify != nullptr)
        {
            UnregisterDeviceNotification(g_device_notify);
            g_device_notify = nullptr;
        }
        if(g_hwnd != nullptr)
        {
            DestroyWindow(g_hwnd);
            g_hwnd = nullptr;
        }
        // Ensure a second call to init() can succeed.
        UnregisterClass(CLS_NAME, GetModuleHandle(0));

        if(g_direct_input != nullptr)
        {
            g_direct_input->Release();
            g_direct_input = nullptr;
        }

        close_control_events();
        g_startup_failed = false;

        logger->info("Shutdown complete");
        return TRUE;
    }
    catch(std::exception const& e)
    {
        logger->critical("shutdown() failed: {}", e.what());
        return FALSE;
    }
    catch(...)
    {
        logger->critical("shutdown() failed: unknown exception");
        return FALSE;
    }
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
    try
    {
        std::lock_guard<std::mutex> lock(g_data_store_mutex);
        if(index < 0 || index >= g_data_store.active_guids.size())
        {
            logger->warn(
                "Attempting to retireve device summary for invalid index {}",
                index
            );
            return DeviceSummary();
        }
        auto guid = g_data_store.active_guids[index];
        const auto it = g_data_store.cache.find(guid);
        if(it == g_data_store.cache.end())
        {
            return DeviceSummary();
        }
        return it->second;
    }
    catch(...)
    {
        return DeviceSummary();
    }
}

DeviceSummary get_device_information_by_guid(GUID guid)
{
    try
    {
        std::lock_guard<std::mutex> lock(g_data_store_mutex);
        const auto it = g_data_store.cache.find(guid);
        if(it == g_data_store.cache.end())
        {
            logger->warn(
                "Attempting to retireve device summary for invalid GUID {}",
                guid_to_string(guid)
            );
            return DeviceSummary();
        }
        return it->second;
    }
    catch(...)
    {
        return DeviceSummary();
    }
}


size_t get_device_count()
{
    try
    {
        std::lock_guard<std::mutex> lock(g_data_store_mutex);
        return g_data_store.active_guids.size();
    }
    catch(...)
    {
        return 0;
    }
}

bool device_exists(GUID guid)
{
    try
    {
        std::lock_guard<std::mutex> lock(g_data_store_mutex);
        for(auto const& dev_guid : g_data_store.active_guids)
        {
            if(dev_guid == guid)
            {
                return true;
            }
        }
        return false;
    }
    catch(...)
    {
        return false;
    }
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

    try
    {
        std::lock_guard<std::mutex> lock(g_data_store_mutex);
        const auto it = g_data_store.state.find(guid);
        if(it == g_data_store.state.end())
        {
            return 0;
        }
        return it->second.axis[index];
    }
    catch(...)
    {
        return 0;
    }
}

bool get_button(GUID guid, DWORD index)
{
    if(index < 1 || index > 128)
    {
        logger->error(
            "{}: Requested invalid button index {}",
            guid_to_string(guid),
            index
        );
        return false;
    }

    try
    {
        std::lock_guard<std::mutex> lock(g_data_store_mutex);
        const auto it = g_data_store.state.find(guid);
        if(it == g_data_store.state.end())
        {
            return false;
        }
        return it->second.button[index];
    }
    catch(...)
    {
        return false;
    }
}

LONG get_hat(GUID guid, DWORD index)
{
    if(index < 1 || index > 4)
    {
        logger->error(
            "{}: Requested invalid hat index {}",
            guid_to_string(guid),
            index
        );
        return -1;
    }

    try
    {
        std::lock_guard<std::mutex> lock(g_data_store_mutex);
        const auto it = g_data_store.state.find(guid);
        if(it == g_data_store.state.end())
        {
            return -1;
        }
        return it->second.hat[index];
    }
    catch(...)
    {
        return -1;
    }
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
