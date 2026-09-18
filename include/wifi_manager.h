#pragma once

#include <string>
#include <vector>

struct WifiNetwork {
    std::string bssid;
    std::string frequency;
    std::string signal;
    std::string flags;
    std::string ssid;
};

struct WifiSavedNetwork {
    std::string id;
    std::string ssid;
    std::string bssid;
    std::string flags;
};

struct WifiStatus {
    std::string state;
    std::string ssid;
    std::string ip;
    std::string bssid;
    bool connected = false;
};

class WifiManager {
public:
    explicit WifiManager(std::string iface = "");
    void set_interface(const std::string &iface);
    std::string interface() const { return iface_; }

    WifiStatus status() const;
    std::vector<WifiNetwork> scan();
    std::vector<WifiSavedNetwork> list_networks();
    bool connect_psk(const std::string &ssid, const std::string &password, std::string &message);
    bool select_network(const std::string &id, std::string &message);
    bool remove_network(const std::string &id, std::string &message);
    bool disconnect(std::string &message);
    bool reconnect(std::string &message);
    bool save_config(std::string &message);

private:
    std::string cli(const std::string &args) const;
    std::string iface_;
};

extern WifiManager g_wifi;
