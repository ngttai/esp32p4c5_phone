#pragma once

#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

/** USB camera: live picture of a UVC webcam on the USB host socket, and photos on the microSD card. */
class PhoneUsbCamera: public systems::phone::App {
public:
    static PhoneUsbCamera *requestInstance();
    ~PhoneUsbCamera();

protected:
    bool run() override;
    bool back() override;
    bool close() override;
    bool pause() override;
    bool resume() override;

private:
    PhoneUsbCamera();

    inline static PhoneUsbCamera *_instance = nullptr;
};

} // namespace esp_brookesia::apps
