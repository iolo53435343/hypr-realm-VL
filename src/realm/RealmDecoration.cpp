#include "RealmDecoration.hpp"

#include "RealmWindowManager.hpp"
#include "../Compositor.hpp"
#include "../render/Renderer.hpp"
#include "../render/Texture.hpp"
#include "../render/pass/RectPassElement.hpp"
#include "../render/pass/TexPassElement.hpp"

#include <algorithm>
#include <cmath>

using namespace Realm;
using namespace Render;

static constexpr double REALM_BAR_HEIGHT = 24.0;

static CHyprColor       realmStateColor(eRealmState state) {
    switch (state) {
        case eRealmState::CREATING: return {0.18F, 0.42F, 0.74F, 1.F};
        case eRealmState::RUNNING: return {0.12F, 0.52F, 0.31F, 1.F};
        case eRealmState::PAUSED: return {0.72F, 0.48F, 0.08F, 1.F};
        case eRealmState::STOPPING: return {0.78F, 0.30F, 0.08F, 1.F};
        case eRealmState::STOPPED: return {0.28F, 0.30F, 0.34F, 1.F};
        case eRealmState::FAILED: return {0.72F, 0.12F, 0.16F, 1.F};
    }

    return {0.28F, 0.30F, 0.34F, 1.F};
}

CRealmDecoration::CRealmDecoration(PHLWINDOW window, SP<CRealm> realm) : IHyprWindowDecoration(window), m_window(window), m_realm(std::move(realm)) {
    ;
}

SDecorationPositioningInfo CRealmDecoration::getPositioningInfo() {
    return {
        .policy         = DECORATION_POSITION_STICKY,
        .edges          = DECORATION_EDGE_TOP,
        .priority       = 11000,
        .desiredExtents = {Vector2D{0.0, REALM_BAR_HEIGHT}, Vector2D{0.0, 0.0}},
        .reserved       = true,
    };
}

void CRealmDecoration::onPositioningReply(const SDecorationPositioningReply& reply) {
    m_assignedBox = reply.assignedGeometry;
}

void CRealmDecoration::draw(PHLMONITOR monitor, float const& alpha) {
    const auto window = m_window.lock();
    if (!validMapped(window) || !monitor || !m_realm)
        return;

    auto box = assignedBoxGlobal();
    box.translate(window->m_floatingOffset - monitor->m_position);
    box.scale(monitor->m_scale).round();
    if (box.empty())
        return;

    auto color = realmStateColor(m_realm->state());
    color.a *= alpha;
    g_pHyprRenderer->addPassElement(makeUnique<CRectPassElement>(CRectPassElement::SRectData{
        .box   = box,
        .color = color,
    }));

    const auto label = realmWindowDecorationLabel(*m_realm);
    if (!m_labelTexture || label != m_textureLabel || monitor->m_scale != m_textureScale) {
        const auto maxWidth = std::max(1, sc<int>(std::floor(box.w - 16.0 * monitor->m_scale)));
        m_labelTexture      = g_pHyprRenderer->renderText(label, CHyprColor{1.F, 1.F, 1.F, 1.F}, std::max(1, sc<int>(std::round(11.F * monitor->m_scale))), false, "", maxWidth);
        m_textureLabel      = label;
        m_textureScale      = monitor->m_scale;
    }

    if (!m_labelTexture || !m_labelTexture->ok())
        return;

    const auto padding = std::round(8.F * monitor->m_scale);
    CBox       textBox = {
        box.x + padding,
        box.y + std::round((box.h - m_labelTexture->m_size.y) / 2.0),
        std::min(m_labelTexture->m_size.x, std::max(0.0, box.w - 2.0 * padding)),
        m_labelTexture->m_size.y,
    };

    g_pHyprRenderer->addPassElement(makeUnique<CTexPassElement>(CTexPassElement::SRenderData{
        .tex     = m_labelTexture,
        .box     = textBox,
        .a       = alpha,
        .clipBox = box,
    }));
}

eDecorationType CRealmDecoration::getDecorationType() {
    return DECORATION_CUSTOM;
}

void CRealmDecoration::updateWindow(PHLWINDOW) {
    const auto label = m_realm ? realmWindowDecorationLabel(*m_realm) : std::string{};
    if (label == m_textureLabel)
        return;

    damageEntire();
    m_labelTexture.reset();
    m_textureLabel.clear();
}

void CRealmDecoration::damageEntire() {
    const auto window = m_window.lock();
    if (!window || !g_pHyprRenderer)
        return;

    auto box = assignedBoxGlobal();
    box.translate(window->m_floatingOffset);
    g_pHyprRenderer->damageBox(box);
}

eDecorationLayer CRealmDecoration::getDecorationLayer() {
    return DECORATION_LAYER_OVER;
}

uint64_t CRealmDecoration::getDecorationFlags() {
    return DECORATION_PART_OF_MAIN_WINDOW;
}

std::string CRealmDecoration::getDisplayName() {
    return "RealmBar";
}

CBox CRealmDecoration::assignedBoxGlobal() const {
    CBox box = m_assignedBox;
    box.translate(g_pDecorationPositioner->getEdgeDefinedPoint(DECORATION_EDGE_TOP, m_window));

    const auto window    = m_window.lock();
    const auto workspace = window ? window->m_workspace : PHLWORKSPACE{};
    if (window && workspace && !window->m_pinned)
        box.translate(workspace->m_renderOffset->value());

    return box.round();
}
