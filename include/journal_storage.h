#pragma once

#include <atomic>
#include <cstdint>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

struct JournalEntry {
    std::string filename;
    std::string date;
    std::string title;
    std::string preview;
};

struct JournalHistoryVersion {
    std::string filename;
    time_t mtime = 0;
    size_t size = 0;
};

// 主页一次要画整月的落笔标记 + 总计/今日/连续三项。这些都只看文件名,
// 合成一次目录扫描,别再每个日期单独扫一遍。
struct JournalStats {
    int total = 0;
    int today = 0;
    int streak = 0;
    std::vector<std::string> dates;  // 去重后的 YYYY-MM-DD,升序
};

class JournalStorage {
public:
    bool begin();
    // filename_out 非空时回传新建的文件名,调用方后续保存才能落到同一篇里
    bool save_entry(const std::string &text, std::string *filename_out = nullptr);
    bool save_entry_raw(const std::string &filename, const std::string &content, bool create_history = true);
    std::vector<JournalEntry> list_entries();
    std::vector<std::pair<std::string, time_t>> list_file_mtimes();
    std::string read_entry(const std::string &filename);

    // 在单篇日记正文中做 ASCII 大小写不敏感的子串搜索(中文按字节精确匹配)。
    // 分块读取,不把整篇正文载入内存。needle_lower 需事先转成小写 ASCII。
    bool search_entry_content(const std::string &filename, const std::string &needle_lower);

    // 日记内容修订号:保存/改写/删除/恢复历史/重挂载目录都会递增。全文搜索结果
    // 带上扫描起点的修订号,之后与当前值比对即可发现正文已过期。
    uint32_t content_revision() const { return m_contentRevision.load(std::memory_order_relaxed); }

    bool delete_entry(const std::string &filename);
    JournalStats stats();

    bool save_recovery_draft(const std::string &content, const std::string &meta);
    bool load_recovery_draft(std::string &content, std::string &meta);
    void clear_recovery_draft();

    std::vector<JournalHistoryVersion> list_history_versions(const std::string &filename);
    std::string read_history_version(const std::string &filename, const std::string &history_filename);
    bool restore_history_version(const std::string &filename, const std::string &history_filename);
    bool delete_history_version(const std::string &filename, const std::string &history_filename);

    std::string base_path() const;

private:
    void ensure_dir();
    std::atomic<uint32_t> m_contentRevision{0};
};

extern JournalStorage g_journal;
