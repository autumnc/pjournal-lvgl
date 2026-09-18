#pragma once

#include <string>

struct WebdavSyncResult {
    bool success = false;
    std::string message;
};

WebdavSyncResult webdav_sync_journal();
