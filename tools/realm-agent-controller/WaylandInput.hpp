#pragma once

#include "../../src/realm/RealmInputProtocol.hpp"

#include <expected>
#include <memory>
#include <string>
#include <vector>

struct SWaylandResult {
    SWaylandResult() = default;
    ~SWaylandResult();
    SWaylandResult(const SWaylandResult&)            = delete;
    SWaylandResult& operator=(const SWaylandResult&) = delete;
    SWaylandResult(SWaylandResult&& other) noexcept;
    SWaylandResult&           operator=(SWaylandResult&& other) noexcept;

    int                       releaseFrameFD();

    uint32_t                  sequence = 0;
    Realm::SRealmInputMessage message;
    int                       frameFD = -1;
    std::string               error;
};

class CWaylandInput {
  public:
    explicit CWaylandInput(int waylandFD);
    ~CWaylandInput();

    std::expected<void, std::string> initialize();
    std::expected<void, std::string> handle(const Realm::SRealmInputMessage& message);
    std::expected<void, std::string> dispatch();
    std::expected<void, std::string> flush();
    void                             releaseAll();
    std::vector<SWaylandResult>      takeResults();
    int                              displayFD() const;

  private:
    struct SImpl;
    std::unique_ptr<SImpl> m_impl;
};
