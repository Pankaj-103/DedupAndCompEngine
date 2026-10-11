#pragma once

#include <filesystem>
#include <string>
#include <vector>

int cmd_init(const std::filesystem::path& repoPath);
int cmd_ingest(const std::filesystem::path& repoPath,
               const std::vector<std::filesystem::path>& inputs, int level);
int cmd_restore(const std::filesystem::path& repoPath, const std::string& spec,
                const std::filesystem::path& outDir);
int cmd_list(const std::filesystem::path& repoPath);
int cmd_stats(const std::filesystem::path& repoPath);
