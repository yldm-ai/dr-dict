#include "copy_hotkey.h"
namespace dr
{
namespace
{
CopyHotkey *owner = nullptr; // a low-level hook procedure carries no context of its own

bool pressed(int vk)
{
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}
bool modifier(DWORD vk)
{
    switch (vk)
    {
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
    case VK_LWIN:
    case VK_RWIN:
        return true;
    default:
        return false;
    }
}
} // namespace

CopyHotkey::~CopyHotkey()
{
    uninstall();
}

bool CopyHotkey::install(std::function<void()> on_trigger)
{
    on_trigger_ = std::move(on_trigger);
    if (hook_)
        return true;
    hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, procedure, GetModuleHandleW(nullptr), 0);
    if (hook_)
        owner = this;
    return hook_ != nullptr;
}

void CopyHotkey::uninstall()
{
    if (hook_)
        UnhookWindowsHookEx(hook_);
    hook_ = nullptr;
    if (owner == this)
        owner = nullptr;
    waiting_ = swallow_up_ = false;
}

LRESULT CALLBACK CopyHotkey::procedure(int code, WPARAM w, LPARAM l)
{
    if (code == HC_ACTION && owner)
    {
        const auto *key = reinterpret_cast<const KBDLLHOOKSTRUCT *>(l);
        bool swallow = false;
        if (w == WM_KEYDOWN || w == WM_SYSKEYDOWN)
            swallow = owner->key_down(key->vkCode, key->time);
        else if (w == WM_KEYUP || w == WM_SYSKEYUP)
            swallow = owner->key_up(key->vkCode);
        if (swallow)
            return 1;
    }
    return CallNextHookEx(nullptr, code, w, l);
}

bool CopyHotkey::key_down(DWORD vk, DWORD time)
{
    // Exactly Ctrl+C: Shift, Alt or Win with it is some other shortcut.
    const bool ctrl_c = vk == 'C' && pressed(VK_CONTROL) && !pressed(VK_SHIFT) && !pressed(VK_MENU) &&
                        !pressed(VK_LWIN) && !pressed(VK_RWIN);
    if (!ctrl_c)
    {
        // Holding or re-pressing Ctrl keeps the sequence alive; any other key ends it.
        if (!modifier(vk))
            waiting_ = false;
        return false;
    }
    // Auto-repeat while C is held is not a second press.
    if (c_held_)
        return false;
    c_held_ = true;
    if (waiting_ && time - first_time_ <= interval_ms)
    {
        waiting_ = false;
        swallow_up_ = true;
        if (on_trigger_)
            on_trigger_();
        return true; // the second Ctrl+C is ours
    }
    // First press (or a late second one, which starts over): let the application copy.
    waiting_ = true;
    first_time_ = time;
    return false;
}

bool CopyHotkey::key_up(DWORD vk)
{
    if (vk == 'C')
        c_held_ = false;
    if (swallow_up_ && vk == 'C')
    {
        swallow_up_ = false;
        return true;
    }
    return false;
}
} // namespace dr
