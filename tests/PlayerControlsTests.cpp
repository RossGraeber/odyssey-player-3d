#include "app/PlayerControls.h"

#include <windows.h>

#include <gtest/gtest.h>

#include <array>
#include <limits>
#include <string>

namespace {

using odyssey::PlayerAction;
using odyssey::PlayerCommand;
using odyssey::PlayerControlState;
using odyssey::PlayerControls;

PlayerControlState playingState() {
    PlayerControlState state;
    state.playing = true;
    state.positionSeconds = 30.0;
    state.durationSeconds = 100.0;
    state.volume = 0.5;
    state.filename = L"Am\u00e9lie 3D.mkv";
    state.audioLabel = L"Fran\u00e7ais";
    state.captionsLabel = L"\u65e5\u672c\u8a9e";
    state.layoutLabel = L"Full SBS";
    state.status = L"3D active";
    return state;
}

void expectCommand(const PlayerAction& action, PlayerCommand command) {
    EXPECT_EQ(action.command, command);
}

} // namespace

TEST(PlayerControls, MapsPhysicalButtonCoordinatesWithoutEyeMirroring) {
    auto state = playingState();
    struct Hit {
        int x;
        int y;
        PlayerCommand command;
    };
    constexpr std::array hits{
        Hit{84, 1016, PlayerCommand::Open},
        Hit{268, 1016, PlayerCommand::PlayPause},
        Hit{454, 1016, PlayerCommand::Stop},
        Hit{1220, 1016, PlayerCommand::ToggleMute},
        Hit{202, 1058, PlayerCommand::AudioMenu},
        Hit{586, 1058, PlayerCommand::CaptionsMenu},
        Hit{916, 1058, PlayerCommand::LayoutMenu},
        Hit{1166, 1058, PlayerCommand::SwapEyes},
        Hit{1572, 860, PlayerCommand::Minimize},
        Hit{1712, 860, PlayerCommand::Fullscreen},
        Hit{1848, 860, PlayerCommand::Close},
    };

    for (const Hit& hit : hits) {
        PlayerControls controls;
        expectCommand(controls.pointerDown(hit.x, hit.y, 1920, 1080, state, 0), PlayerCommand::None);
        EXPECT_TRUE(controls.captureNeeded());
        expectCommand(controls.pointerUp(hit.x, hit.y, 1920, 1080, state, 1), hit.command);
        EXPECT_FALSE(controls.captureNeeded());
    }
}

TEST(PlayerControls, ButtonReleaseOutsideAndCancellationDoNotActivate) {
    PlayerControls controls;
    auto state = playingState();

    controls.pointerDown(84, 1016, 1920, 1080, state, 0);
    EXPECT_TRUE(controls.captureNeeded());
    expectCommand(controls.pointerUp(400, 1016, 1920, 1080, state, 1), PlayerCommand::None);

    controls.pointerDown(1848, 860, 1920, 1080, state, 2);
    EXPECT_TRUE(controls.captureNeeded());
    controls.cancelInteraction();
    EXPECT_FALSE(controls.captureNeeded());
    expectCommand(controls.pointerUp(1848, 860, 1920, 1080, state, 3), PlayerCommand::None);
}

TEST(PlayerControls, SeekDragPreviewsAndEmitsOnlyOnRelease) {
    PlayerControls controls;
    auto state = playingState();

    expectCommand(controls.pointerDown(500, 950, 1920, 1080, state, 10), PlayerCommand::None);
    EXPECT_TRUE(controls.captureNeeded());
    expectCommand(controls.pointerMove(1450, 950, 1920, 1080, state, 20), PlayerCommand::None);
    EXPECT_GT(controls.seekPreviewSeconds(state), 70.0);

    const PlayerAction released = controls.pointerUp(1450, 950, 1920, 1080, state, 30);
    expectCommand(released, PlayerCommand::Seek);
    EXPECT_GT(released.value, 70.0);
    EXPECT_FALSE(controls.captureNeeded());
}

TEST(PlayerControls, CancelledSeekDoesNotEmitAndReleasesCapture) {
    PlayerControls controls;
    auto state = playingState();

    controls.pointerDown(800, 950, 1920, 1080, state, 10);
    ASSERT_TRUE(controls.captureNeeded());
    controls.cancelInteraction();

    EXPECT_FALSE(controls.captureNeeded());
    expectCommand(controls.pointerUp(1400, 950, 1920, 1080, state, 20), PlayerCommand::None);
}

TEST(PlayerControls, VolumeChangesDuringCapturedDragAndClamps) {
    PlayerControls controls;
    auto state = playingState();

    PlayerAction action = controls.pointerDown(1600, 1000, 1920, 1080, state, 10);
    expectCommand(action, PlayerCommand::SetVolume);
    EXPECT_TRUE(controls.captureNeeded());

    action = controls.pointerMove(2500, 1000, 1920, 1080, state, 20);
    expectCommand(action, PlayerCommand::SetVolume);
    EXPECT_DOUBLE_EQ(action.value, 1.0);
}

TEST(PlayerControls, AutoHideRequiresPlayingAndNoActiveReasonToRemainVisible) {
    PlayerControls controls;
    auto state = playingState();

    controls.pointerMove(100, 100, 1920, 1080, state, 100);
    EXPECT_FALSE(controls.visible(state, 2599));
    EXPECT_FALSE(controls.visible(state, 2600));

    state.playing = false;
    EXPECT_TRUE(controls.visible(state, 9000));
    state.playing = true;
    state.error = L"Audio device unavailable";
    EXPECT_TRUE(controls.visible(state, 9000));
    state.error.clear();
    state.menuOpen = true;
    EXPECT_TRUE(controls.visible(state, 9000));

    state.menuOpen = false;
    state.forceVisible = true;
    EXPECT_TRUE(controls.visible(state, 9000));
}

TEST(PlayerControls, OnlyRecentMotionOverPanelRevealsItDuringPlayback) {
    PlayerControls controls;
    auto state = playingState();

    controls.pointerMove(50, 100, 1920, 1080, state, 100);
    EXPECT_FALSE(controls.visible(state, 100));
    EXPECT_FALSE(controls.visible(state, 5000));

    controls.pointerMove(600, 950, 1920, 1080, state, 5000);
    EXPECT_TRUE(controls.visible(state, 5000));
    EXPECT_TRUE(controls.visible(state, 7499));
    EXPECT_FALSE(controls.visible(state, 7500));

    controls.pointerDown(600, 950, 1920, 1080, state, 7600);
    EXPECT_TRUE(controls.visible(state, 9000));
}

TEST(PlayerControls, IsoTitleButtonExistsOnlyForIsoState) {
    PlayerControls controls;
    auto state = playingState();

    controls.pointerDown(1400, 1050, 1920, 1080, state, 0);
    expectCommand(controls.pointerUp(1400, 1050, 1920, 1080, state, 1), PlayerCommand::None);
    state.isIso = true;
    controls.pointerDown(1400, 1050, 1920, 1080, state, 2);
    expectCommand(controls.pointerUp(1400, 1050, 1920, 1080, state, 3), PlayerCommand::IsoTitleMenu);
}

TEST(PlayerControls, DisabledMenusHaveNoActiveHitTargets) {
    PlayerControls controls;
    auto state = playingState();
    state.isIso = true;
    state.audioMenuEnabled = false;
    state.captionsMenuEnabled = false;
    state.isoTitleMenuEnabled = false;

    constexpr std::array points{
        std::array{202, 1058},
        std::array{586, 1058},
        std::array{1400, 1050},
    };
    for (const auto& point : points) {
        expectCommand(controls.pointerDown(point[0], point[1], 1920, 1080, state, 0),
                      PlayerCommand::None);
        EXPECT_FALSE(controls.captureNeeded());
        expectCommand(controls.pointerUp(point[0], point[1], 1920, 1080, state, 1),
                      PlayerCommand::None);
    }
}

TEST(PlayerControls, RejectsClientsBelowTheDocumentedMinimum) {
    PlayerControls controls;
    auto state = playingState();

    EXPECT_FALSE(controls.render(state, 959, 120, 0).visible);
    EXPECT_FALSE(controls.render(state, 960, 119, 0).visible);
    expectCommand(controls.pointerDown(10, 10, 959, 120, state, 0), PlayerCommand::None);
    EXPECT_FALSE(controls.captureNeeded());
}

TEST(PlayerControls, RendersCompactUnicodeBgraPanel) {
    PlayerControls controls;
    auto state = playingState();
    state.playing = false;

    const auto surface = controls.render(state, 1920, 1080, 0);
    EXPECT_TRUE(surface.visible);
    EXPECT_EQ(surface.scale, 2);
    EXPECT_EQ(surface.width, 1920);
    EXPECT_LT(surface.height, 1080);
    EXPECT_EQ(surface.bgra.size(),
              static_cast<size_t>(surface.width) * surface.height * 4);

    state.filename = L"Different title";
    const auto differentText = controls.render(state, 1920, 1080, 0);
    EXPECT_NE(surface.bgra, differentText.bgra);
}

TEST(PlayerControls, SanitizesNonFiniteTimelineAndVolumeValues) {
    PlayerControls controls;
    auto state = playingState();
    state.positionSeconds = (std::numeric_limits<double>::infinity)();
    state.durationSeconds = (std::numeric_limits<double>::quiet_NaN)();
    state.volume = (std::numeric_limits<double>::infinity)();
    state.error = L"The selected audio device became unavailable while playback was active.";

    const auto surface = controls.render(state, 1920, 1080, 0);
    EXPECT_TRUE(surface.visible);
    EXPECT_FALSE(surface.bgra.empty());
    expectCommand(controls.pointerDown(500, 950, 1920, 1080, state, 1), PlayerCommand::None);
    EXPECT_FALSE(controls.captureNeeded());
}

TEST(PlayerControls, KeyboardMapsTransportKeys) {
    auto state = playingState(); // position 30, duration 100, volume 0.5

    expectCommand(PlayerControls::keyAction(VK_SPACE, false, false, state), PlayerCommand::PlayPause);
    expectCommand(PlayerControls::keyAction('M', false, false, state), PlayerCommand::ToggleMute);

    const PlayerAction right = PlayerControls::keyAction(VK_RIGHT, false, false, state);
    expectCommand(right, PlayerCommand::Seek);
    EXPECT_DOUBLE_EQ(right.value, 35.0);

    const PlayerAction shiftRight = PlayerControls::keyAction(VK_RIGHT, false, true, state);
    expectCommand(shiftRight, PlayerCommand::Seek);
    EXPECT_DOUBLE_EQ(shiftRight.value, 60.0);

    const PlayerAction ctrlLeft = PlayerControls::keyAction(VK_LEFT, true, false, state);
    expectCommand(ctrlLeft, PlayerCommand::Seek);
    EXPECT_DOUBLE_EQ(ctrlLeft.value, 0.0);

    const PlayerAction home = PlayerControls::keyAction(VK_HOME, false, false, state);
    expectCommand(home, PlayerCommand::Seek);
    EXPECT_DOUBLE_EQ(home.value, 0.0);

    const PlayerAction up = PlayerControls::keyAction(VK_UP, false, false, state);
    expectCommand(up, PlayerCommand::SetVolume);
    EXPECT_DOUBLE_EQ(up.value, 0.55);

    auto silent = state;
    silent.volume = 0.0;
    const PlayerAction down = PlayerControls::keyAction(VK_DOWN, false, false, silent);
    expectCommand(down, PlayerCommand::SetVolume);
    EXPECT_DOUBLE_EQ(down.value, 0.0);

    auto noDuration = state;
    noDuration.durationSeconds = 0.0;
    expectCommand(PlayerControls::keyAction(VK_RIGHT, false, false, noDuration), PlayerCommand::None);

    expectCommand(PlayerControls::keyAction(VK_ESCAPE, false, false, state), PlayerCommand::Close);
    auto fullscreenState = state;
    fullscreenState.fullscreen = true;
    expectCommand(PlayerControls::keyAction(VK_ESCAPE, false, false, fullscreenState), PlayerCommand::Fullscreen);

    expectCommand(PlayerControls::keyAction('Q', false, false, state), PlayerCommand::None);
    expectCommand(PlayerControls::keyAction('Q', true, false, state), PlayerCommand::Close);

    auto noAudio = state;
    noAudio.audioMenuEnabled = false;
    expectCommand(PlayerControls::keyAction('A', false, false, noAudio), PlayerCommand::None);
}

TEST(PlayerControls, KeyboardActivityRevealsPanelBriefly) {
    PlayerControls controls;
    auto state = playingState();

    EXPECT_FALSE(controls.visible(state, 100));
    controls.keyboardActivity(100);
    EXPECT_TRUE(controls.visible(state, 2599));
    EXPECT_FALSE(controls.visible(state, 2600));
}

TEST(PlayerControls, HoverHintDescribesControlUnderPointer) {
    PlayerControls controls;
    auto state = playingState();

    controls.pointerMove(268, 1016, 1920, 1080, state, 0);
    EXPECT_NE(controls.hoverHint(state, 1920, 1080).find(L"Space"), std::wstring::npos);

    controls.pointerMove(500, 950, 1920, 1080, state, 1);
    EXPECT_EQ(controls.hoverHint(state, 1920, 1080).rfind(L"Seek to ", 0), 0u);

    controls.pointerMove(1600, 1000, 1920, 1080, state, 2);
    EXPECT_NE(controls.hoverHint(state, 1920, 1080).find(L"Volume"), std::wstring::npos);

    controls.pointerMove(100, 100, 1920, 1080, state, 3);
    const std::wstring idle = controls.hoverHint(state, 1920, 1080);
    EXPECT_NE(idle.find(L"M mute"), std::wstring::npos);

    controls.pointerLeave(4);
    EXPECT_EQ(controls.hoverHint(state, 1920, 1080), idle);
}

TEST(PlayerControls, HoverHintFollowsSeekDragOutsidePanel) {
    PlayerControls controls;
    auto state = playingState();

    controls.pointerDown(500, 950, 1920, 1080, state, 0);
    controls.pointerMove(1450, 100, 1920, 1080, state, 1);
    EXPECT_EQ(controls.hoverHint(state, 1920, 1080).rfind(L"Seek to ", 0), 0u);
}

TEST(PlayerControls, PanelContainsMatchesGeometry) {
    PlayerControls controls;
    EXPECT_TRUE(controls.panelContains(500, 950, 1920, 1080));
    EXPECT_FALSE(controls.panelContains(100, 100, 1920, 1080));
    EXPECT_FALSE(controls.panelContains(10, 10, 959, 120));
}
