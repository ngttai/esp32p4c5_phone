#pragma once

#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

/** Voice recorder: WAV files on the memory card, played back in the app. */
class PhoneRecorder: public systems::phone::App {
public:
    static PhoneRecorder *requestInstance();
    ~PhoneRecorder();

protected:
    bool run() override;
    bool back() override;
    bool close() override;
    bool keepRunning() const override;     // recording or playing: not closed at the running app limit

private:
    PhoneRecorder();

    inline static PhoneRecorder *_instance = nullptr;
};

} // namespace esp_brookesia::apps
