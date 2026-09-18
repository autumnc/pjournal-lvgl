#include "outline_model.h"

void outline_normalize_data(JsonValue &data) {
    if(data.isNull() || !data.has("nodes") || !data["nodes"].isArray()) {
        data = JsonValue::object();
        data.set("nodes", JsonValue::array());
        data.set("bookmarks", JsonValue::array());
        data.set("tags", JsonValue::array());
    }
    if(!data.has("bookmarks") || !data["bookmarks"].isArray()) data.set("bookmarks", JsonValue::array());
    if(!data.has("tags") || !data["tags"].isArray()) data.set("tags", JsonValue::array());
}

bool outline_has_children(const JsonValue &nodes, int idx) {
    if(idx < 0 || idx + 1 >= (int)nodes.size()) return false;
    return nodes[idx + 1]["level"].asInt(0) > nodes[idx]["level"].asInt(0);
}

bool outline_is_last_at_level(const JsonValue &nodes, int idx, int lvl) {
    for(int j = idx + 1; j < (int)nodes.size(); ++j) {
        int jl = nodes[j]["level"].asInt(0);
        if(jl == lvl) return false;
        if(jl < lvl) break;
    }
    return true;
}

std::string outline_tree_prefix(const JsonValue &nodes, int idx) {
    if(idx < 0 || idx >= (int)nodes.size()) return "";
    int lvl = nodes[idx]["level"].asInt(0);
    std::string prefix;
    for(int a = 0; a < lvl; ++a) {
        int anc = -1;
        for(int j = idx - 1; j >= 0; --j) {
            int jl = nodes[j]["level"].asInt(0);
            if(jl == a) { anc = j; break; }
            if(jl < a) break;
        }
        prefix += (anc >= 0 && !outline_is_last_at_level(nodes, anc, a)) ? "│ " : "  ";
    }
    if(lvl > 0) prefix += outline_is_last_at_level(nodes, idx, lvl) ? "└─ " : "├─ ";
    else prefix = "◆ ";
    return prefix;
}

std::vector<int> outline_filter_nodes(const JsonValue &nodes, const std::set<int> &folded,
                                      const std::vector<std::string> &filterTags,
                                      const std::string &filterText) {
    std::vector<int> out;
    int total = (int)nodes.size();
    std::set<int> hidden;
    for(int fi : folded) {
        if(fi < 0 || fi >= total) continue;
        int fl = nodes[fi]["level"].asInt(0);
        for(int j = fi + 1; j < total; ++j) {
            if(nodes[j]["level"].asInt(0) <= fl) break;
            hidden.insert(j);
        }
    }
    for(int i = 0; i < total; ++i) {
        if(hidden.count(i)) continue;
        if(!filterTags.empty()) {
            const JsonValue &tt = nodes[i]["tags"];
            bool match = false;
            if(tt.isArray())
                for(int j = 0; j < (int)tt.size() && !match; ++j)
                    for(const auto &ft : filterTags)
                        if(tt[j].asString() == ft) { match = true; break; }
            if(!match) continue;
        }
        if(!filterText.empty() && nodes[i]["title"].asString().find(filterText) == std::string::npos) continue;
        out.push_back(i);
    }
    return out;
}

std::string outline_safe_filename(const std::string &title) {
    std::string out;
    for(char c : title) {
        if(c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') out += '_';
        else out += c;
    }
    if(out.empty()) out = "untitled";
    if(out.size() > 40) {
        size_t cut = 40;
        while(cut > 0 && ((unsigned char)out[cut] & 0xC0) == 0x80) cut--;
        out = out.substr(0, cut);
    }
    if(out.empty()) out = "untitled";
    return out + ".txt";
}
