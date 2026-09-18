#pragma once

#include <ctime>
#include <string>
#include <utility>
#include <vector>

struct JournalEntry {
    std::string filename;
    std::string date;
    std::string title;
    std::string preview;
    std::string full_text;
};

struct JournalHistoryVersion {
    std::string filename;
    time_t mtime = 0;
    size_t size = 0;
};

class JournalStorage {
public:
    bool begin();
    bool save_entry(const std::string &text);
    bool save_entry_raw(const std::string &filename, const std::string &content, bool create_history = true);
    std::vector<JournalEntry> list_entries();
    std::vector<std::pair<std::string, time_t>> list_file_mtimes();
    std::string read_entry(const std::string &filename);
    bool delete_entry(const std::string &filename);
    bool has_entry(const std::string &date);
    int count_today();
    int total_entries();
    int streak();

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
};

extern JournalStorage g_journal;
