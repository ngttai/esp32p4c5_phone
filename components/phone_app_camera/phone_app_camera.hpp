#pragma once

#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

/** Camera: live preview of the CSI sensor and photos saved as JPEG on the microSD card. */
class PhoneCamera: public systems::phone::App {
public:
    static PhoneCamera *requestInstance();
    ~PhoneCamera();

protected:
    bool run() override;
    bool back() override;
    bool close() override;
    bool pause() override;
    bool resume() override;

private:
    PhoneCamera();

    inline static PhoneCamera *_instance = nullptr;
};

} // namespace esp_brookesia::apps
