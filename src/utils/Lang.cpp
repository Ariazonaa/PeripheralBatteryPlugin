#include "utils/Lang.h"

#include <atomic>
#include <iterator>

namespace lang
{
    namespace
    {
        bool ChineseFor(LANGID id)
        {
            if (id == 0)
                id = GetUserDefaultUILanguage();
            return PRIMARYLANGID(id) == LANG_CHINESE;
        }

        std::atomic<bool> g_chinese{ ChineseFor(0) };

        struct Entry
        {
            const wchar_t* en;
            const wchar_t* zh;
        };

        // Same order as lang::Text.
        constexpr Entry kTexts[] = {
            { L"Peripheral Battery", L"外设电量" },
            { L"Shows the battery level of peripheral devices (headphones, mouse) in the taskbar.",
              L"在任务栏显示耳机和鼠标的电量。" },
            { L"Headphones Battery", L"耳机电量" },
            { L"Mouse Battery", L"鼠标电量" },
            { L"Mouse ", L"鼠标 " },
            { L"Mouse", L"鼠标" },
            { L"Headphones", L"耳机" },
            { L"Mouse", L"鼠标" },
            { L": ", L"：" },
            { L" – ", L" · " },
            { L"not connected", L"未连接" },
            { L"no value", L"暂未获取电量" },
            { L"charging", L"充电中" },
            { L"charging, ", L"充电中，" },
            { L"full", L"已充满" },
            { L", as of ", L"，更新于 " },
            { L", currently unreachable", L"，暂时无法读取电量" },
            { L"Low battery: ", L"电量不足：" },
            { L"Windows Bluetooth", L"Windows 蓝牙" },
            { L"Logitech (HID++)", L"罗技（HID++）" },
            { L"(none)", L"（未设置）" },
            { L"The poll interval must be between 15 and 3600 seconds.", L"刷新间隔必须在 15～3600 秒之间。" },
            { L"The warning threshold must be between 0 and 100 percent (0 = no warning).",
              L"请输入 0～100 之间的整数。设为 0 时关闭低电量提醒。" },
            { L"The settings could not be saved.", L"无法保存设置。" },
            { L"The settings file is unexpectedly large.", L"配置文件过大。" },
            { L"No path known for the settings file.", L"未能确定配置文件的保存位置。" },
            { L"Error ", L"错误代码：" },
        };
        static_assert(std::size(kTexts) == static_cast<size_t>(Text::ErrorCode) + 1, "kTexts out of sync with lang::Text");
    }

    void SetLanguage(LANGID id)
    {
        g_chinese.store(ChineseFor(id));
    }

    bool IsChinese()
    {
        return g_chinese.load();
    }

    LANGID DialogLanguage()
    {
        return IsChinese() ? MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED)
                           : MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US);
    }

    const wchar_t* Tr(Text text)
    {
        const size_t index = static_cast<size_t>(text);
        if (index >= std::size(kTexts))
            return L"";
        return IsChinese() ? kTexts[index].zh : kTexts[index].en;
    }
}
