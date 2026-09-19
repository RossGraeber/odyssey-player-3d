#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace odyssey {

enum class PlayerCommand {
    None,
    Open,
    PlayPause,
    Stop,
    ToggleMute,
    AudioMenu,
    CaptionsMenu,
    LayoutMenu,
    SwapEyes,
    Fullscreen,
    Minimize,
    Close,
    IsoTitleMenu,
    Seek,
    SetVolume,
};

struct PlayerAction {
    PlayerCommand command{PlayerCommand::None};
    double value{0.0};
};

struct PlayerControlState {
    bool playing{false};
    bool muted{false};
    bool isIso{false};
    bool menuOpen{false};
    bool forceVisible{false};
    bool fullscreen{false};
    bool audioMenuEnabled{true};
    bool captionsMenuEnabled{true};
    bool isoTitleMenuEnabled{true};
    double positionSeconds{0.0};
    double durationSeconds{0.0};
    double volume{1.0};
    std::wstring filename;
    std::wstring audioLabel;
    std::wstring captionsLabel;
    std::wstring layoutLabel;
    std::wstring status;
    std::wstring error;
};

struct PlayerControlSurface {
    int left{0};
    int top{0};
    int width{0};
    int height{0};
    int scale{1};
    bool visible{false};
    std::vector<std::uint8_t> bgra;
};

class PlayerControls {
public:
    static constexpr int minimumClientWidth = 960;
    static constexpr int minimumClientHeight = 120;

    // Maps a Win32 virtual-key press to a player action. Seek and volume
    // actions carry absolute values derived from the state.
    static PlayerAction keyAction(unsigned virtualKey, bool ctrl, bool shift,
                                  const PlayerControlState& state) noexcept;

    // Mouse-over help for whatever the pointer is currently on; idle text
    // listing the main shortcuts when nothing is hovered.
    std::wstring hoverHint(const PlayerControlState& state,
                           int clientWidth, int clientHeight) const;

    bool panelContains(int x, int y, int clientWidth, int clientHeight) const noexcept;
    void keyboardActivity(std::uint64_t nowMilliseconds) noexcept;

    PlayerControlSurface render(const PlayerControlState& state,
                                int clientWidth,
                                int clientHeight,
                                std::uint64_t nowMilliseconds) const;

    PlayerAction pointerMove(int x, int y, int clientWidth, int clientHeight,
                             const PlayerControlState& state,
                             std::uint64_t nowMilliseconds);
    void pointerLeave(std::uint64_t nowMilliseconds) noexcept;
    PlayerAction pointerDown(int x, int y, int clientWidth, int clientHeight,
                             const PlayerControlState& state,
                             std::uint64_t nowMilliseconds);
    PlayerAction pointerUp(int x, int y, int clientWidth, int clientHeight,
                           const PlayerControlState& state,
                           std::uint64_t nowMilliseconds);
    void cancelInteraction() noexcept;

    bool captureNeeded() const noexcept;
    bool visible(const PlayerControlState& state, std::uint64_t nowMilliseconds) const noexcept;
    double seekPreviewSeconds(const PlayerControlState& state) const noexcept;

private:
    enum class Drag { None, Seek, Volume };

    Drag drag_{Drag::None};
    bool pointerInside_{false};
    double dragValue_{0.0};
    int pointerX_{0};
    int pointerY_{0};
    PlayerCommand pressed_{PlayerCommand::None};
    std::uint64_t lastMotionMilliseconds_{0};
    std::uint64_t revealUntilMilliseconds_{0};
};

} // namespace odyssey
