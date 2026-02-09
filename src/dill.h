#pragma once

#define WIN32_LEAN_AND_MEAN
#define DIRECTINPUT_VERSION 0x0800

#include <windows.h>
#include <Dbt.h>
#include <dinput.h>

#include <mutex>
#include <string>
#include <stdexcept>
#include <unordered_map>

#define FMT_UNICODE 0

namespace std
{
    template<> struct hash<GUID>
    {
        /**
         * \brief Hash computation for GUID instances.
         *
         * This has is intended for use with std::unordered_map and as such
         * is not required to maintain the uniqueness of the GUID.
         *
         * \param guid GUID instance to compute the hash of
         * \return hash value for the provided GUID instance
         */
        std::size_t operator()(GUID const& guid) const
        {
            return std::hash<DWORD>()(
                guid.Data1 ^
                ((guid.Data2 << 16) | guid.Data3) ^
                *(DWORD*)(guid.Data4) ^
                *(DWORD*)(guid.Data4 + 4)
            );
        }
    };
}

/**
 * \brief Physical input types available on joysticks.
 */
enum class JoystickInputType : UINT8
{
    Axis = 1,
    Button = 2,
    Hat = 3
};

/**
 * \brief Device state change types.
 */
enum class DeviceActionType : UINT8
{
    Connected = 1,
    Disconnected = 2
};

/**
 * \brief Joystick input event data.
 *
 * Stores information about a single joystick input event.
 */
struct JoystickInputData
{
    GUID                                device_guid;
    JoystickInputType                   input_type;
    // In case of an axis this is the axis_index and not the linear_index.
    UINT8                               input_index;
    LONG                                value;
};

/**
 * \brief Stores axis information.
 *
 * Stores the linear and axis index of a single axis.
 */
struct AxisMap
{
    DWORD                               linear_index;
    DWORD                               axis_index;
};

/**
 * \brief Holds information about the configuration of a single
 *        joystick device.
 *
 * All data required to handle future data from the particular
 * joystick device.
 */
struct DeviceSummary
{
    GUID                                device_guid;
    DWORD                               vendor_id;
    DWORD                               product_id;
    DWORD                               joystick_id;
    char                                name[MAX_PATH];
    DWORD                               axis_count;
    DWORD                               button_count;
    DWORD                               hat_count;
    AxisMap                             axis_map[8];
};

/**
 * \brief Represents the current state of a device.
 */
struct DeviceState
{
    DeviceState();

    std::vector<LONG>                   axis;
    std::vector<bool>                   button;
    std::vector<LONG>                   hat;
};

/**
 * \brief Holds information about devices and their state.
 */
struct DeviceMetaDataStore
{
    //! Indicates whether or not a device is buffered.
    std::unordered_map<GUID, bool> is_buffered;
    //! Flag indicating whether or not the device is fully operational.
    std::unordered_map<GUID, bool> is_ready;
    //! List of active GUIDs.
    std::vector<GUID> active_guids;
    //! Maps from GUID to DirectInput device instance.
    std::unordered_map<GUID, LPDIRECTINPUTDEVICE8> device_map;
};


//! Callback for joystick value change events.
typedef void (*JoystickInputEventCallback)(JoystickInputData);
//! Callback for device change events.
typedef void (*DeviceChangeCallback)(DeviceSummary, DeviceActionType);


/**
 * \brief Returns the string representation of the provided GUID.
 *
 * \param guid GUID instance to convert to string
 * \return string representation of the provided GUID instance
 */
__declspec(dllexport)
std::string guid_to_string(GUID guid);

/**
 * \brief Returns the string representation of a given error id.
 *
 * \param message_id error message id to translate
 * \return textual representation of the provided error code
 */
std::string error_to_string(DWORD message_id);

/**
 * \brief Callback for window creation.
 *
 * Subscribes toe device change events.
 */
BOOL on_create_window(HWND window_hdl, LPARAM l_param);

/**
 * \brief Callback for device change notifications.
 *
 * Handles processing device change notifications if they relate to joystick
 * like devices.
 */
BOOL on_device_change(LPARAM l_param, WPARAM w_param);

/**
 * \brief Handles messages from Windows' messaging system.
 */
LRESULT window_proc(
    HWND                                window_hdl,
    UINT                                msg_type,
    WPARAM                              w_param,
    LPARAM                              l_param
);

/**
 * \brief Aggregates the data about a single DirectInput message event.
 *
 * \param data message content holding information about the event
 * \param guid identifier of the device who caused the event
 */
void emit_joystick_input_event(
    DIDEVICEOBJECTDATA const&           data,
    GUID const&                         guid
);

/**
 * \brief Aggregates the data about a single DirectInput device.
 *
 * \param instance device instance holding the information
 * \param guid identifier of the device being updated
 * \return true if the device requires polling, false otherwise
 */
bool process_buffered_events(LPDIRECTINPUTDEVICE8 instance, GUID const& guid);

/**
 * \brief Thread function handling joystick messages.
 */
DWORD WINAPI joystick_update_thread(LPVOID l_param);

/**
 * \brief Handles general windows messages.
 */
DWORD WINAPI message_handler_thread(LPVOID l_param);

/**
 * \brief Creates the "window" infrastructure needed to receive messages.
 */
HWND create_window();

/**
 * \brief Callback processing a single DirectInput device.
 */
BOOL CALLBACK handle_device_cb(LPCDIDEVICEINSTANCE instance, LPVOID data);

/**
 * \brief Fill struct with correct axis data.
 */
BOOL CALLBACK set_axis_range(LPCDIDEVICEOBJECTINSTANCE lpddoi, LPVOID pvRef);

/**
 * \brief Enumerates all DirectInput devices present on the system.
 */
void enumerate_devices();

/**
 * \brief Performs device initialization.
 *
 * \param guid GUID of the device to initialize
 * \param name name of the device being initialized
 */
void initialize_device(GUID guid, std::string name);

/**
 * \brief Returns the indices of axes used by the device.
 *
 * \param device pointer to the device to query
 * \return list of used axes indices
 */
std::vector<int> used_axis_indices(LPDIRECTINPUTDEVICE8 device);

/**
 * \brief Returns the vendor id of the HID device.
 *
 * \param device pointer to the device
 * \param guid GUID of the device
 * \return vendor usb id
 */
DWORD get_vendor_id(LPDIRECTINPUTDEVICE8 device, GUID guid);

/**
 * \brief Returns the product id of the HID device.
 *
 * \param device pointer to the device
 * \param guid GUID of the device
 * \return product usb id
 */
DWORD get_product_id(LPDIRECTINPUTDEVICE8 device, GUID guid);

/**
 * \brief Returns the joystick id assigned by Windows to the device.
 *
 * \param device pointer to the device
 * \param guid GUID of the device
 * \return windows joystick id
 */
DWORD get_joystick_id(LPDIRECTINPUTDEVICE8 device, GUID guid);


extern "C"
{
    /**
     * \brief Initializes the library.
     */
    __declspec(dllexport)
    BOOL init();

    /**
     * \brief Sets the callback for input events.
     *
     * \param cb callback to use from now on
     */
    __declspec(dllexport)
    void set_input_event_callback(JoystickInputEventCallback cb);

    /**
     * \brief Sets the callback for device change events.
     *
     * \param cb callback to use from now on
     */
    __declspec(dllexport)
    void set_device_change_callback(DeviceChangeCallback cb);

    /**
     * \brief Returns the DeviceSummary for the device with the provided index.
     *
     * \param index index of the device to return
     * \param DeviceSummary of the device with the provided index
     */
    __declspec(dllexport)
    DeviceSummary get_device_information_by_index(size_t index);

    __declspec(dllexport)
    DeviceSummary get_device_information_by_guid(GUID guid);

    /**
     * \brief Returns the number of available devices.
     *
     * \return number of available devices
     */
    __declspec(dllexport)
    size_t get_device_count();

    __declspec(dllexport)
    bool device_exists(GUID guid);

    /**
     * \brief Returns the current axis value.
     *
     * The provided index is an "axis_index", i.e. not a linear one but the
     * index specifying a particular axis with gaps.
     *
     * \param guid GUID of the device to query
     * \param index axis index to query
     * \return current axis value of the provided device and axis
     */
    __declspec(dllexport)
    LONG get_axis(GUID guid, DWORD index);

    /**
     * \brief Returns the state of a button on a given device.
     *
     * \param guid GUID of the device to query
     * \param index the index of the button to query
     * \return current state of the queried button
     */
    __declspec(dllexport)
    bool get_button(GUID guid, DWORD index);

    /**
     * \brief Returns the state of a hat on a given device.
     *
     * \param guid GUID of the device to query
     * \param index the index of the hat to query
     * \return current state of the queried hat
     */
    __declspec(dllexport)
    LONG get_hat(GUID guid, DWORD index);
}
