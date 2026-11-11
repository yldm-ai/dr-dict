#pragma once
#include <functional>
#include <windows.h>
namespace dr
{
// Detects Ctrl+C pressed twice in quick succession anywhere in Windows.
//
// Reference: GoldenDict-ng src/hotkey/winhotkeywrapper.cc watches its pass-through hotkeys with a
// WH_KEYBOARD_LL hook instead of RegisterHotKey, because RegisterHotKey swallows the key and
// re-injecting it with SendInput is blocked by UIPI when an elevated window has focus. This
// independent implementation follows the same idea: the first Ctrl+C reaches the focused
// application untouched (so it copies the selection), and only a second Ctrl+C within the
// interval is swallowed and reported.
class CopyHotkey
{
  public:
    ~CopyHotkey();
    // Installs the hook on the calling thread, which must pump messages. `on_trigger` runs on that
    // thread from inside the hook, so it must only queue work.
    bool install(std::function<void()> on_trigger);
    void uninstall();
    bool installed() const { return hook_ != nullptr; }

    static constexpr DWORD interval_ms = 500;

  private:
    static LRESULT CALLBACK procedure(int code, WPARAM w, LPARAM l);
    bool key_down(DWORD vk, DWORD time);
    bool key_up(DWORD vk);

    HHOOK hook_ = nullptr;
    std::function<void()> on_trigger_;
    bool waiting_ = false;    // the first Ctrl+C passed; waiting for the second
    DWORD first_time_ = 0;    // its event time (GetTickCount clock)
    bool swallow_up_ = false; // the second C's key-up must not reach the application either
    bool c_held_ = false;     // C is down: further key-downs are auto-repeat
};
} // namespace dr
