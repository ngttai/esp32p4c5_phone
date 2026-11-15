#pragma once

#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

/** Video: plays Motion JPEG .avi files (with PCM sound) from the microSD card. */
class PhoneVideo: public systems::phone::App {
public:
    static PhoneVideo *requestInstance();
    ~PhoneVideo();

protected:
    bool run() override;
    bool back() override;
    bool close() override;
    bool pause() override;

private:
    PhoneVideo();

    inline static PhoneVideo *_instance = nullptr;
};

} // namespace esp_brookesia::apps
