# Direct Input Listener Library - DILL

## Overview

The purpose of DILL is to provide joystick device information, including:
* device addition and removal
* device specification
* change in device state

This information is exposed via a simple C style API such that other languages, such as Python, can easily interface with it. The desired information is obtained from the DirectInput system as well as Windows' event system.

## Usage

The main way of interacting with DILL occurs via the two callbacks exposed by the library.

The `set_input_event_callback` function allows setting the callback responsible to inform the the using code about the addition or removal of a device. The callback has the following form `void device_change_callback(DeviceSummary info, DeviceActionType action)`.

The `set_device_change_callback` is execute whenever a device changes its state and takes a callback of the following form `void event_callback(JoystickInputData data)`.
