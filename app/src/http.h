#pragma once
#include "dr/online.h"
#include <memory>
#include <mutex>
#include <set>
#include <string>
namespace dr
{
// Shared by the online fetchers: cancel() aborts their requests in flight and fails later ones at
// once, so the workers joined on exit never wait for a slow server.
class HttpCancel
{
  public:
    void cancel();
    bool cancelled() const;
    bool track(void *handle);   // false (and nothing tracked) once cancelled
    bool untrack(void *handle); // true when the caller still owns the handle and must close it

  private:
    mutable std::mutex mutex_;
    bool cancelled_ = false;
    std::set<void *> open_;
};

// WinHTTP over https, for the online sources. Only `hosts` are contacted, redirects are never
// followed, responses are size-capped and the system proxy is used. Adapted from DR Music's
// native/http.cpp (the same author's client) without its streaming and upload paths.
HttpFetch winhttp_fetch(std::set<std::string> hosts, std::shared_ptr<HttpCancel> cancel);
} // namespace dr
