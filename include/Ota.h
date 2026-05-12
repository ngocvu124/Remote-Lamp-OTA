#ifndef OTA_H
#define OTA_H

#include <Arduino.h>

struct OtaVersion {
    char name[32];
    char url[128];
    char sha256[65];
};

class OtaLogic {
public:
    OtaVersion versions[10];
    int versionCount = 0;

    bool fetchVersions();
    void begin(const char* firmwareUrl);
};

extern OtaLogic ota;

#endif
