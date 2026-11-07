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

    // rkwifi 驱动提供真正的射频电源开关(写 /sys/class/rkwifi/driver 0/1)。
    // 开发机没有这个文件,设置里就不显示开关。
    bool power_control_available() const;
    bool powered() const;
    bool set_power(bool on, std::string &message);

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
    std::string cli(const std::vector<std::string> &args) const;
    std::string iface_;
};

extern WifiManager g_wifi;
