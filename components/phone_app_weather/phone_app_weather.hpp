#pragma once

#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

/** Weather: current conditions and a 7-day forecast from open-meteo.com for a chosen city. */
class PhoneWeather: public systems::phone::App {
public:
    static PhoneWeather *requestInstance();
    ~PhoneWeather();

protected:
    bool run() override;
    bool back() override;
    bool resume() override;

private:
    PhoneWeather();

    inline static PhoneWeather *_instance = nullptr;
};

} // namespace esp_brookesia::apps
