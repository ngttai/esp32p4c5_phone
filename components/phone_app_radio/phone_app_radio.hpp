#pragma once

#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

/**
 * Radio: HLS internet radio stations listed in /sdcard/radio/stations.json (no built-in list), with a
 * 12-band spectrum. Leaving the app (home or close) stops the sound.
 */
class PhoneRadio: public systems::phone::App {
public:
    static PhoneRadio *requestInstance();
    ~PhoneRadio();

protected:
    bool run() override;
    bool back() override;
    bool close() override;
    bool pause() override;

private:
    PhoneRadio();

    inline static PhoneRadio *_instance = nullptr;
};

} // namespace esp_brookesia::apps
