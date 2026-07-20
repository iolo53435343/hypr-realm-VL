#pragma once

#include "../render/decorations/IHyprWindowDecoration.hpp"
#include "Realm.hpp"

#include <functional>
#include <optional>
#include <vector>

namespace Render {
    class ITexture;
}

namespace Realm {
    enum class eRealmDecorationAction : uint8_t {
        TAKEOVER = 0,
        RELEASE,
        PAUSE,
        RESUME,
        STOP,
    };

    struct SRealmDecorationControl {
        eRealmDecorationAction action = eRealmDecorationAction::TAKEOVER;
        std::string            label;
    };

    std::vector<SRealmDecorationControl> realmDecorationControls(const CRealm& realm);

    class CRealmDecoration : public IHyprWindowDecoration {
      public:
        CRealmDecoration(PHLWINDOW window, SP<CRealm> realm, std::function<void(eRealmDecorationAction)> actionCallback);
        ~CRealmDecoration() override = default;

        SDecorationPositioningInfo getPositioningInfo() override;
        void                       onPositioningReply(const SDecorationPositioningReply& reply) override;
        void                       draw(PHLMONITOR monitor, float const& alpha) override;
        eDecorationType            getDecorationType() override;
        void                       updateWindow(PHLWINDOW window) override;
        void                       damageEntire() override;
        eDecorationLayer           getDecorationLayer() override;
        uint64_t                   getDecorationFlags() override;
        std::string                getDisplayName() override;
        bool                       onInputOnDeco(const eInputType type, const Vector2D& mouseCoords, std::any data = {}) override;

      private:
        struct SControlTexture {
            eRealmDecorationAction action = eRealmDecorationAction::TAKEOVER;
            std::string            label;
            SP<Render::ITexture>   texture;
        };

        CBox                                                 assignedBoxGlobal() const;
        std::vector<std::pair<eRealmDecorationAction, CBox>> controlBoxes(const CBox& bar, double scale = 1.0) const;
        std::optional<eRealmDecorationAction>                actionAt(const Vector2D& position) const;

        PHLWINDOWREF                                         m_window;
        SP<CRealm>                                           m_realm;
        std::function<void(eRealmDecorationAction)>          m_actionCallback;
        CBox                                                 m_assignedBox = {};
        SP<Render::ITexture>                                 m_labelTexture;
        std::vector<SControlTexture>                         m_controlTextures;
        std::optional<eRealmDecorationAction>                m_pressedAction;
        std::string                                          m_textureLabel;
        float                                                m_textureScale = 0.F;
    };
}
