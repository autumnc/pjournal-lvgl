#pragma once

#include <string>
#include <vector>

extern bool g_fileEdit;

struct FileEditEntry {
    std::string name;
    size_t size = 0;
};

bool fileEditInit();
std::string fileEditDir();
std::string fileEditLastName();
void fileEditSetLastName(const std::string &name);
std::string fileEditPath(const std::string &name);
std::vector<FileEditEntry> fileEditList();
std::string fileEditLoad(const std::string &name);
bool fileEditSave(const std::string &name, const std::string &text);
bool fileEditCreate(const std::string &name);
bool fileEditRemove(const std::string &name);
bool fileEditRename(const std::string &oldName, const std::string &newName);
std::string fileEditSanitizeName(const std::string &name);
