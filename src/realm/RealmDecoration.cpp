#include "RealmDecoration.hpp"

#include "RealmWindowManager.hpp"
#include "../Compositor.hpp"
#include "../render/Renderer.hpp"
#include "../render/Texture.hpp"
#include "../render/pass/RectPassElement.hpp"
#include "../render/pass/TexPassElement.hpp"
#include "../devices/IPointer.hpp"

#include <algorithm>
#include <cmath>
#include <linux/input-event-codes.h>

using namespace Realm;
using namespace Render;

static constexpr double REALM_BAR_HEIGHT     = 32.0;
static constexpr double REALM_BAR_PADDING    = 4.0;
static constexpr double REALM_CONTROL_GAP    = 4.0;
static constexpr double REALM_CONTROL_HEIGHT = 24.0;

static double           realmControlWidth(eRealmDecorationAction action) {
    switch (action) {
        case eRealmDecorationAction::TAKEOVER: return 88.0;
        case eRealmDecorationAction::RELEASE: return 76.0;
        case eRealmDecorationAction::PAUSE: return 64.0;
        case eRealmDecorationAction::RESUME: return 72.0;
        case eRealmDecorationAction::STOP: return 52.0;
    }

    return 64.0;
}

std::vector<SRealmDecorationControl> Realm::realmDecorationControls(const CRealm& realm) {
    std::vector<SRealmDecorationControl> controls;
    if (realm.state() == eRealmState::RUNNING) {
        controls.emplace_back(SRealmDecorationControl{
            .action = realm.inputOwner() == eRealmInputOwner::HUMAN ? eRealmDecorationAction::RELEASE : eRealmDecorationAction::TAKEOVER,
            .label  = realm.inputOwner() == eRealmInputOwner::HUMAN ? "Release" : "Take Over",
        });
        controls.emplace_back(SRealmDecorationControl{.action = eRealmDecorationAction::PAUSE, .label = "Pause"});
    } else if (realm.state() == eRealmState::PAUSED)
        controls.emplace_back(SRealmDecorationControl{.action = eRealmDecorationAction::RESUME, .label = "Resume"});

    if (realm.state() == eRealmState::CREATING || realm.state() == eRealmState::RUNNING || realm.state() == eRealmState::PAUSED)
        controls.emplace_back(SRealmDecorationControl{.action = eRealmDecorationAction::STOP, .label = "Stop"});
    return controls;
}

static CHyprColor realmStateColor(eRealmState state) {
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

CRealmDecoration::CRealmDecoration(PHLWINDOW window, SP<CRealm> realm, std::function<void(eRealmDecorationAction)> actionCallback) :
    IHyprWindowDecoration(window), m_window(window), m_realm(std::move(realm)), m_actionCallback(std::move(actionCallback)) {
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

    const auto controls    = realmDecorationControls(*m_realm);
    const auto buttonBoxes = controlBoxes(box, monitor->m_scale);
    const auto buttonWidth =
        std::ranges::fold_left(buttonBoxes, 0.0, [scale = monitor->m_scale](double width, const auto& entry) { return width + entry.second.w + REALM_CONTROL_GAP * scale; });
    const auto label = realmWindowDecorationLabel(*m_realm);
    if (!m_labelTexture || label != m_textureLabel || monitor->m_scale != m_textureScale) {
        const auto maxWidth = std::max(1, sc<int>(std::floor(box.w - buttonWidth - 16.0 * monitor->m_scale)));
        m_labelTexture      = g_pHyprRenderer->renderText(label, CHyprColor{1.F, 1.F, 1.F, 1.F}, std::max(1, sc<int>(std::round(11.F * monitor->m_scale))), false, "", maxWidth);
        m_textureLabel      = label;
        m_textureScale      = monitor->m_scale;
        m_controlTextures.clear();
        for (const auto& control : controls) {
            m_controlTextures.emplace_back(SControlTexture{
                .action  = control.action,
                .label   = control.label,
                .texture = g_pHyprRenderer->renderText(control.label, CHyprColor{1.F, 1.F, 1.F, 1.F}, std::max(1, sc<int>(std::round(11.F * monitor->m_scale)))),
            });
        }
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

    for (const auto& [action, buttonBox] : buttonBoxes) {
        const auto control = std::ranges::find(m_controlTextures, action, &SControlTexture::action);
        if (control == m_controlTextures.end())
            continue;

        auto buttonColor = action == eRealmDecorationAction::STOP ? CHyprColor{0.56F, 0.10F, 0.14F, alpha} : CHyprColor{0.07F, 0.08F, 0.11F, 0.72F * alpha};
        g_pHyprRenderer->addPassElement(makeUnique<CRectPassElement>(CRectPassElement::SRectData{
            .box   = buttonBox,
            .color = buttonColor,
            .round = std::round(4.F * monitor->m_scale),
        }));
        if (!control->texture || !control->texture->ok())
            continue;

        const CBox controlTextBox = {
            buttonBox.x + std::round((buttonBox.w - control->texture->m_size.x) / 2.0),
            buttonBox.y + std::round((buttonBox.h - control->texture->m_size.y) / 2.0),
            control->texture->m_size.x,
            control->texture->m_size.y,
        };
        g_pHyprRenderer->addPassElement(makeUnique<CTexPassElement>(CTexPassElement::SRenderData{
            .tex     = control->texture,
            .box     = controlTextBox,
            .a       = alpha,
            .clipBox = buttonBox,
        }));
    }
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
    m_controlTextures.clear();
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
    return DECORATION_ALLOWS_MOUSE_INPUT | DECORATION_PART_OF_MAIN_WINDOW;
}

std::string CRealmDecoration::getDisplayName() {
    return "RealmBar";
}

bool CRealmDecoration::onInputOnDeco(const eInputType type, const Vector2D& mouseCoords, std::any data) {
    if (type != INPUT_TYPE_BUTTON)
        return true;

    const auto& event = std::any_cast<const IPointer::SButtonEvent&>(data);
    if (event.button != BTN_LEFT)
        return true;

    const auto action = actionAt(mouseCoords);
    if (event.state == WL_POINTER_BUTTON_STATE_PRESSED) {
        m_pressedAction = action;
        return true;
    }

    if (event.state == WL_POINTER_BUTTON_STATE_RELEASED && action && action == m_pressedAction && m_actionCallback)
        m_actionCallback(*action);
    m_pressedAction.reset();
    return true;
}

std::vector<std::pair<eRealmDecorationAction, CBox>> CRealmDecoration::controlBoxes(const CBox& bar, double scale) const {
    std::vector<std::pair<eRealmDecorationAction, CBox>> boxes;
    if (!m_realm)
        return boxes;

    auto x = bar.x + bar.w - REALM_BAR_PADDING * scale;
    for (const auto& control : realmDecorationControls(*m_realm) | std::views::reverse) {
        const auto width = realmControlWidth(control.action) * scale;
        x -= width;
        boxes.emplace_back(control.action,
                           CBox{x, bar.y + REALM_BAR_PADDING * scale, width, std::min(REALM_CONTROL_HEIGHT * scale, bar.h - 2.0 * REALM_BAR_PADDING * scale)}.round());
        x -= REALM_CONTROL_GAP * scale;
    }
    return boxes;
}

std::optional<eRealmDecorationAction> CRealmDecoration::actionAt(const Vector2D& position) const {
    const auto boxes = controlBoxes(assignedBoxGlobal());
    const auto found = std::ranges::find_if(boxes, [&position](const auto& entry) { return entry.second.containsPoint(position); });
    return found == boxes.end() ? std::nullopt : std::optional<eRealmDecorationAction>{found->first};
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
