#pragma once

#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

class PhoneSettings: public systems::phone::App {
public:
    static PhoneSettings *requestInstance();
    ~PhoneSettings();

protected:
    bool run() override;
    bool back() override;

private:
    PhoneSettings();

    inline static PhoneSettings *_instance = nullptr;
};

} // namespace esp_brookesia::apps
