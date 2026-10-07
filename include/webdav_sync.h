#pragma once

#include <string>

struct WebdavSyncResult {
    bool success = false;
    std::string message;
};

WebdavSyncResult webdav_sync_journal();

// Remote root (URL + directory, unencoded) for UI display.
std::string webdav_remote_base();
