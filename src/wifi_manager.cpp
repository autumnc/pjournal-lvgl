#include "wifi_manager.h"

#include "process_utils.h"
#include "settings.h"

#include <cstdio>
#include <cstdlib>
#include <net/if.h>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

WifiManager g_wifi;

static std::string trim(std::string s) {
    while(!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t start = s.find_first_not_of(" \t\r\n");
    return start == std::string::npos ? "" : s.substr(start);
}

// rkwifi 是 Rockchip 封装的 bcmdhd 控制口:1 上电,0 给整颗芯片断电。
static const char *kRkwifiDriver = "/sys/class/rkwifi/driver";

static bool read_int_file(const char *path, int &out) {
    FILE *f = fopen(path, "r");
    if(!f) return false;
    char buf[32] = {};
    const bool ok = fgets(buf, sizeof(buf), f) != nullptr;
    fclose(f);
    if(!ok) return false;
    out = atoi(buf);
    return true;
}

static bool write_int_file(const char *path, int value) {
    FILE *f = fopen(path, "w");
    if(!f) return false;
    const bool ok = fprintf(f, "%d\n", value) > 0 && fflush(f) == 0;
    if(fclose(f) != 0) return false;
    return ok;
}

WifiManager::WifiManager(std::string iface) : iface_(std::move(iface)) {}

void WifiManager::set_interface(const std::string &iface) {
    iface_ = iface;
}

std::string WifiManager::cli(const std::vector<std::string> &args) const {
    std::string iface = iface_.empty() ? g_settings.wlan_interface() : iface_;
    std::vector<std::string> cmd = {"wpa_cli", "-i", iface};
    cmd.insert(cmd.end(), args.begin(), args.end());
    return process::run_capture(cmd);
}

WifiStatus WifiManager::status() const {
    WifiStatus st;
    std::istringstream in(cli({"status"}));
    std::string line;
    while(std::getline(in, line)) {
        size_t eq = line.find('=');
        if(eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        if(key == "wpa_state") st.state = val;
        else if(key == "ssid") st.ssid = val;
        else if(key == "ip_address") st.ip = val;
        else if(key == "bssid") st.bssid = val;
    }
    st.connected = st.state == "COMPLETED";
    return st;
}

bool WifiManager::power_control_available() const {
    struct stat st {};
    return stat(kRkwifiDriver, &st) == 0;
}

bool WifiManager::powered() const {
    int v = 0;
    if(read_int_file(kRkwifiDriver, v)) return v != 0;
    // 没有 rkwifi 的机器按接口 IFF_UP 判断,和状态栏的图标口径一致
    const std::string iface = iface_.empty() ? g_settings.wlan_interface() : iface_;
    int flags = 0;
    if(read_int_file(("/sys/class/net/" + iface + "/flags").c_str(), flags))
        return (flags & IFF_UP) != 0;
    return false;
}

bool WifiManager::set_power(bool on, std::string &message) {
    if(!power_control_available()) {
        message = "此设备不支持 WiFi 电源开关";
        return false;
    }
    const std::string iface = iface_.empty() ? g_settings.wlan_interface() : iface_;

    if(on) {
        if(!write_int_file(kRkwifiDriver, 1)) {
            message = "开启 WiFi 电源失败";
            return false;
        }
        // 驱动重新注册 wlan0 需要一点时间;等它出现再拉起链路,最多等 3 秒
        for(int i = 0; i < 30; ++i) {
            std::string sys = "/sys/class/net/" + iface;
            struct stat st {};
            if(stat(sys.c_str(), &st) == 0) break;
            usleep(100000);
        }
        process::run_capture({"ip", "link", "set", iface, "up"}, 5000);
        // 和 wifi-tui 共用同一个状态标记,别让两边显示打架
        FILE *flag = fopen("/tmp/wifion", "w");
        if(flag) fclose(flag);
        // 常驻 wpa_supplicant 会自己关联;顺手催一次,省得等它下轮扫描
        cli({"reassociate"});
        message = "WiFi 已开启";
        return true;
    }

    // 顺序照 wifi-tui:先停关联、链路 down,最后才给射频断电
    process::run_capture({"killall", "wpa_supplicant"}, 3000);
    process::run_capture({"ip", "link", "set", iface, "down"}, 5000);
    remove("/tmp/wifion");
    // AP6210 是蓝牙/WiFi 二合一;蓝牙在用时整颗芯片不能断电
    struct stat st {};
    if(stat("/tmp/bton", &st) != 0 && !write_int_file(kRkwifiDriver, 0)) {
        message = "关闭 WiFi 电源失败";
        return false;
    }
    message = "WiFi 已关闭";
    return true;
}

std::vector<WifiNetwork> WifiManager::scan() {
    cli({"scan"});
    std::string out = cli({"scan_results"});
    std::istringstream in(out);
    std::string line;
    std::vector<WifiNetwork> nets;
    bool header = true;
    while(std::getline(in, line)) {
        if(header) { header = false; continue; }
        std::vector<std::string> parts;
        size_t start = 0;
        for(int i = 0; i < 4; ++i) {
            size_t tab = line.find('\t', start);
            if(tab == std::string::npos) break;
            parts.push_back(line.substr(start, tab - start));
            start = tab + 1;
        }
        if(parts.size() == 4 && start <= line.size()) {
            WifiNetwork n{parts[0], parts[1], parts[2], parts[3], line.substr(start)};
            if(!n.ssid.empty()) nets.push_back(n);
        }
    }
    return nets;
}

std::vector<WifiSavedNetwork> WifiManager::list_networks() {
    std::string out = cli({"list_networks"});
    std::istringstream in(out);
    std::string line;
    std::vector<WifiSavedNetwork> nets;
    bool header = true;
    while(std::getline(in, line)) {
        if(header) { header = false; continue; }
        std::vector<std::string> parts;
        size_t start = 0;
        for(int i = 0; i < 3; ++i) {
            size_t tab = line.find('\t', start);
            if(tab == std::string::npos) break;
            parts.push_back(line.substr(start, tab - start));
            start = tab + 1;
        }
        if(parts.size() == 3 && start <= line.size()) {
            WifiSavedNetwork n{parts[0], parts[1], parts[2], line.substr(start)};
            if(!n.id.empty()) nets.push_back(n);
        }
    }
    return nets;
}

bool WifiManager::connect_psk(const std::string &ssid, const std::string &password, std::string &message) {
    std::string id = trim(cli({"add_network"}));
    if(id.empty() || id.find("FAIL") != std::string::npos) {
        message = "添加网络失败";
        return false;
    }
    if(cli({"set_network", id, "ssid", "\"" + ssid + "\""}).find("OK") == std::string::npos) {
        message = "设置 SSID 失败";
        return false;
    }
    std::vector<std::string> psk_args = password.empty()
        ? std::vector<std::string>{"set_network", id, "key_mgmt", "NONE"}
        : std::vector<std::string>{"set_network", id, "psk", "\"" + password + "\""};
    if(cli(psk_args).find("OK") == std::string::npos) {
        message = "设置密码失败";
        return false;
    }
    cli({"enable_network", id});
    cli({"select_network", id});
    cli({"reassociate"});
    save_config(message);

    std::string iface = iface_.empty() ? g_settings.wlan_interface() : iface_;
    // dhcpcd -n 会一直等到拿到地址(或者它自己那套很长的重试超时用完)。这里必须封顶:
    // 它是从主线程调过来的,无限等就是界面冻住。到点没拿到就当作「正在连接」,让状态
    // 栏自己慢慢刷。
    process::run_capture({"dhcpcd", "-n", iface}, 45000);

    WifiStatus st = status();
    message = st.connected ? ("已连接 " + st.ssid + " " + st.ip) : "正在连接";
    return true;
}

bool WifiManager::disconnect(std::string &message) {
    bool ok = cli({"disconnect"}).find("OK") != std::string::npos;
    message = ok ? "已断开" : "断开失败";
    return ok;
}

bool WifiManager::select_network(const std::string &id, std::string &message) {
    bool ok = cli({"select_network", id}).find("OK") != std::string::npos;
    if(ok) {
        cli({"reassociate"});
        save_config(message);
    }
    message = ok ? "已选择网络" : "选择网络失败";
    return ok;
}

bool WifiManager::remove_network(const std::string &id, std::string &message) {
    bool ok = cli({"remove_network", id}).find("OK") != std::string::npos;
    if(ok) save_config(message);
    message = ok ? "已移除保存的网络" : "移除网络失败";
    return ok;
}

bool WifiManager::reconnect(std::string &message) {
    bool ok = cli({"reconnect"}).find("OK") != std::string::npos;
    message = ok ? "已请求重连" : "重连失败";
    return ok;
}

bool WifiManager::save_config(std::string &message) {
    bool ok = cli({"save_config"}).find("OK") != std::string::npos;
    message = ok ? "WiFi 配置已保存" : "保存 WiFi 配置失败";
    return ok;
}
