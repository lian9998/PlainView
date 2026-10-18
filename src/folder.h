// folder.h — background directory listing with natural (human) filename order.
//
// Enumeration starts only after the first image is on screen (or has failed), and
// it only reads directory entries: no file is opened or decoded.
#pragma once

#include "common.h"

#include <condition_variable>
#include <mutex>
#include <thread>

struct FolderList {
    UINT generation = 0;
    std::wstring directory;
    std::vector<std::wstring> names;  // file names only, natural order
    int index = -1;                   // index of the current file, -1 if absent
};

class FolderScanner {
public:
    FolderScanner(HWND notify, UINT message);
    ~FolderScanner();

    void Start();
    void Stop();

    // Requests a listing; any earlier request is superseded.
    void Scan(const std::wstring& directory, const std::wstring& currentFile, UINT generation);

private:
    struct Request {
        std::wstring directory;
        std::wstring currentFile;
        UINT generation = 0;
    };

    void ThreadMain();
    bool IsCancelled(UINT generation) const;

    HWND notify_ = nullptr;
    UINT message_ = 0;

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool hasRequest_ = false;
    bool stopping_ = false;
    Request request_;

    std::atomic<UINT> generation_{0};
};
