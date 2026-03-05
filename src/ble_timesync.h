#pragma once

// BLE Current Time Service (CTS, UUID 0x1805) peripheral for automatic phone time sync.
//
// Usage:
//   1. bleTimeSync_start()  — init NimBLE GATT server + CTS, begin advertising "UroFlow"
//   2. Poll bleTimeSync_synced() during the boot window
//   3. bleTimeSync_stop()   — stop advertising before starting Acaia BLE client

void bleTimeSync_start();
bool bleTimeSync_synced();
void bleTimeSync_stop();
