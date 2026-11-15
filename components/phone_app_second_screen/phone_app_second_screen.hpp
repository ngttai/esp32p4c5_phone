#pragma once

#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

/** Second screen: the board as a USB display with touch for a PC (xfz1986 "udisp" protocol). */
class PhoneSecondScreen: public systems::phone::App {
public:
    static PhoneSecondScreen *requestInstance();
    ~PhoneSecondScreen();

protected:
    bool run() override;
    bool back() override;
    bool close() override;
    bool pause() override;

private:
    PhoneSecondScreen();

    inline static PhoneSecondScreen *_instance = nullptr;
};

} // namespace esp_brookesia::apps
