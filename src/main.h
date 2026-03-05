#pragma once

enum class AppState {
    BOOT,
    MSC_MODE,
    WIFI_SYNC,
    BLE_SCANNING,
    BLE_CONNECTING,
    CONNECTED_IDLE,
    SESSION_ACTIVE,
    SESSION_END
};
