#pragma once

#include "../../src/realm/RealmInputProtocol.hpp"

#include <expected>
#include <memory>
#include <string>

class CWaylandInput {
  public:
    explicit CWaylandInput(int waylandFD);
    ~CWaylandInput();

    std::expected<void, std::string> initialize();
    std::expected<void, std::string> handle(const Realm::SRealmInputMessage& message);
    std::expected<void, std::string> dispatch();
    std::expected<void, std::string> flush();
    void                             releaseAll();
    int                              displayFD() const;

  private:
    struct SImpl;
    std::unique_ptr<SImpl> m_impl;
};
