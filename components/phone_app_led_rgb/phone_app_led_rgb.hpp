#pragma once

#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

/** The RGB LED of the board as a small lamp: effect, color, speed and brightness, while the app runs. */
class PhoneLedRgb: public systems::phone::App {
public:
    static PhoneLedRgb *requestInstance();
    ~PhoneLedRgb();

protected:
    bool run() override;
    bool back() override;
    bool close() override;
    bool keepRunning() const override;     // lit: not closed at the running app limit

private:
    PhoneLedRgb();

    inline static PhoneLedRgb *_instance = nullptr;
};

} // namespace esp_brookesia::apps
