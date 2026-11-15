#pragma once

#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

/** Gallery: thumbnails of the JPEG photos in /sdcard/DCIM, a viewer and delete. */
class PhoneGallery: public systems::phone::App {
public:
    static PhoneGallery *requestInstance();
    ~PhoneGallery();

protected:
    bool run() override;
    bool back() override;
    bool close() override;
    bool resume() override;

private:
    PhoneGallery();

    inline static PhoneGallery *_instance = nullptr;
};

} // namespace esp_brookesia::apps
