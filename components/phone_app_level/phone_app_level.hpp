#pragma once

#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

/** Spirit level: a bubble driven by the accelerometer of the IMU. */
class PhoneLevel: public systems::phone::App {
public:
    static PhoneLevel *requestInstance();
    ~PhoneLevel();

protected:
    bool run() override;
    bool back() override;

private:
    PhoneLevel();

    inline static PhoneLevel *_instance = nullptr;
};

} // namespace esp_brookesia::apps
