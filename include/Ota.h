#ifndef OTA_H
#define OTA_H

#include <Arduino.h>

struct OtaVersion {
    char name[32];
    char url[128];
};

class OtaLogic {
public:
    OtaVersion versions[10];
    int versionCount = 0;

    bool fetchVersions();
    void begin(const char* firmwareUrl);
    void loop();
};

extern OtaLogic ota;

#endif