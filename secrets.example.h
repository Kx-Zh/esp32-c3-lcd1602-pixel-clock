#pragma once

// Copy this file to secrets.h, then fill in only the profile you use.
// secrets.h is ignored by Git and must never be committed.

// WPA2-Enterprise PEAP/MSCHAPv2
static constexpr char ENTERPRISE_WIFI_SSID[] = "YOUR_ENTERPRISE_SSID";
static constexpr char EAP_SERVER_DOMAIN[] = "radius.example.edu";
static constexpr char EAP_IDENTITY[] = "YOUR_OUTER_IDENTITY";
static constexpr char EAP_USERNAME[] = "YOUR_INNER_USERNAME";
static constexpr char EAP_PASSWORD[] = "YOUR_EAP_PASSWORD";

// WPA2-Personal
static constexpr char PERSONAL_WIFI_SSID[] = "YOUR_WIFI_SSID";
static constexpr char PERSONAL_WIFI_PASSWORD[] = "YOUR_WIFI_PASSWORD";
