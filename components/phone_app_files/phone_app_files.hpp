#pragma once

#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

/** Files: browse the microSD card, read text files, delete files and empty folders, lend the card to a PC over USB. */
class PhoneFiles: public systems::phone::App {
public:
    static PhoneFiles *requestInstance();
    ~PhoneFiles();

protected:
    bool run() override;
    bool back() override;
    bool resume() override;
    bool pause() override;
    bool close() override;

private:
    PhoneFiles();

    inline static PhoneFiles *_instance = nullptr;
};

} // namespace esp_brookesia::apps
