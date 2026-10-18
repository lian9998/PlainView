// folder.cpp
#include "folder.h"

#include <shlwapi.h>

#include <algorithm>

FolderScanner::FolderScanner(HWND notify, UINT message) : notify_(notify), message_(message) {}

FolderScanner::~FolderScanner() { Stop(); }

void FolderScanner::Start() {
    if (thread_.joinable()) return;
    stopping_ = false;
    thread_ = std::thread(&FolderScanner::ThreadMain, this);
}

void FolderScanner::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!thread_.joinable()) return;
        stopping_ = true;
        hasRequest_ = false;
    }
    cv_.notify_all();
    thread_.join();
}

void FolderScanner::Scan(const std::wstring& directory, const std::wstring& currentFile,
                         UINT generation) {
    generation_.store(generation);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        request_.directory = directory;
        request_.currentFile = currentFile;
        request_.generation = generation;
        hasRequest_ = true;
    }
    cv_.notify_one();
}

bool FolderScanner::IsCancelled(UINT generation) const {
    return stopping_ || generation_.load() != generation;
}

void FolderScanner::ThreadMain() {
    for (;;) {
        Request request;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stopping_ || hasRequest_; });
            if (stopping_) break;
            request = std::move(request_);
            hasRequest_ = false;
        }

        const UINT generation = request.generation;
        if (IsCancelled(generation)) continue;

        auto list = std::make_unique<FolderList>();
        list->generation = generation;
        list->directory = request.directory;

        WIN32_FIND_DATAW data{};
        const std::wstring pattern = JoinPath(request.directory, L"*");
        HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data,
                                       FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (find != INVALID_HANDLE_VALUE) {
            size_t counter = 0;
            do {
                if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
                list->names.emplace_back(data.cFileName);
                // Cancellation is polled, not per entry: this loop is cheap.
                if ((++counter & 0x3FF) == 0 && IsCancelled(generation)) break;
            } while (FindNextFileW(find, &data));
            FindClose(find);
        }

        if (IsCancelled(generation)) continue;

        // Natural order: image2 before image10.
        std::sort(list->names.begin(), list->names.end(),
                  [](const std::wstring& a, const std::wstring& b) {
                      const int cmp = StrCmpLogicalW(a.c_str(), b.c_str());
                      if (cmp != 0) return cmp < 0;
                      return a < b;
                  });

        const std::wstring currentName = FileNameOf(request.currentFile);
        for (size_t i = 0; i < list->names.size(); ++i) {
            if (StrCmpIW(list->names[i].c_str(), currentName.c_str()) == 0) {
                list->index = static_cast<int>(i);
                break;
            }
        }

        auto* payload = list.release();
        if (!PostMessageW(notify_, message_, 0, reinterpret_cast<LPARAM>(payload))) {
            delete payload;
        }
    }
}
