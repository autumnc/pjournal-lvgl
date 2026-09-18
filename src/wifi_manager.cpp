#include "wifi_manager.h"

#include "settings.h"

#include <cstdio>
#include <cstdlib>
#include <sstream>

WifiManager g_wifi;

static std::string shell_quote(const std::string &s) {
    std::string out = "'";
    for(char c : s) {
        if(c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

static std::string trim(std::string s) {
    while(!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t start = s.find_first_not_of(" \t\r\n");
    return start == std::string::npos ? "" : s.substr(start);
}

WifiManager::WifiManager(std::string iface) : iface_(std::move(iface)) {}

void WifiManager::set_interface(const std::string &iface) {
    iface_ = iface;
}

std::string WifiManager::cli(const std::string &args) const {
    std::string iface = iface_.empty() ? g_settings.wlan_interface() : iface_;
    std::string cmd = "wpa_cli -i " + shell_quote(iface) + " " + args + " 2>&1";
    FILE *p = popen(cmd.c_str(), "r");
    if(!p) return "";
    std::string out;
    char buf[512];
    while(fgets(buf, sizeof(buf), p)) out += buf;
    pclose(p);
    return out;
}

WifiStatus WifiManager::status() const {
    WifiStatus st;
    std::istringstream in(cli("status"));
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

std::vector<WifiNetwork> WifiManager::scan() {
    cli("scan");
    std::string out = cli("scan_results");
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
    std::string out = cli("list_networks");
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
    std::string id = trim(cli("add_network"));
    if(id.empty() || id.find("FAIL") != std::string::npos) {
        message = "添加网络失败";
        return false;
    }
    if(cli("set_network " + id + " ssid " + shell_quote("\"" + ssid + "\"")).find("OK") == std::string::npos) {
        message = "设置 SSID 失败";
        return false;
    }
    std::string psk_arg = password.empty() ? "key_mgmt NONE" : ("psk " + shell_quote("\"" + password + "\""));
    if(cli("set_network " + id + " " + psk_arg).find("OK") == std::string::npos) {
        message = "设置密码失败";
        return false;
    }
    cli("enable_network " + id);
    cli("select_network " + id);
    cli("reassociate");
    save_config(message);

    std::string iface = iface_.empty() ? g_settings.wlan_interface() : iface_;
    std::string dhcp = "dhcpcd -n " + shell_quote(iface) + " >/dev/null 2>&1";
    system(dhcp.c_str());

    WifiStatus st = status();
    message = st.connected ? ("已连接 " + st.ssid + " " + st.ip) : "正在连接";
    return true;
}

bool WifiManager::disconnect(std::string &message) {
    bool ok = cli("disconnect").find("OK") != std::string::npos;
    message = ok ? "已断开" : "断开失败";
    return ok;
}

bool WifiManager::select_network(const std::string &id, std::string &message) {
    bool ok = cli("select_network " + id).find("OK") != std::string::npos;
    if(ok) {
        cli("reassociate");
        save_config(message);
    }
    message = ok ? "已选择网络" : "选择网络失败";
    return ok;
}

bool WifiManager::remove_network(const std::string &id, std::string &message) {
    bool ok = cli("remove_network " + id).find("OK") != std::string::npos;
    if(ok) save_config(message);
    message = ok ? "已移除保存的网络" : "移除网络失败";
    return ok;
}

bool WifiManager::reconnect(std::string &message) {
    bool ok = cli("reconnect").find("OK") != std::string::npos;
    message = ok ? "已请求重连" : "重连失败";
    return ok;
}

bool WifiManager::save_config(std::string &message) {
    bool ok = cli("save_config").find("OK") != std::string::npos;
    message = ok ? "WiFi 配置已保存" : "保存 WiFi 配置失败";
    return ok;
}
