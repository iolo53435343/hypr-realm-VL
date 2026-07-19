#pragma once

#include "../render/decorations/IHyprWindowDecoration.hpp"
#include "Realm.hpp"

namespace Render {
    class ITexture;
}

namespace Realm {
    class CRealmDecoration : public IHyprWindowDecoration {
      public:
        CRealmDecoration(PHLWINDOW window, SP<CRealm> realm);
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

      private:
        CBox                 assignedBoxGlobal() const;

        PHLWINDOWREF         m_window;
        SP<CRealm>           m_realm;
        CBox                 m_assignedBox = {};
        SP<Render::ITexture> m_labelTexture;
        std::string          m_textureLabel;
        float                m_textureScale = 0.F;
    };
}
