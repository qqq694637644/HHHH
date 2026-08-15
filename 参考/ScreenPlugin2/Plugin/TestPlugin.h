#include "../Plugin/PluginBase.h"
#include "../ScreenPlugin/ScreenManager.h"
#include <windows.h>
#include <string>
#include <memory>

#if _DEBUG_PRINTF_OPEN_
#include <iostream>
#endif // _DEBUG_PRINTF_OPEN_

// 插件主类。
class TestPlugin : public PluginBase
{
public:
    TestPlugin();

    virtual ~TestPlugin();

    // 初始化插件。
    virtual bool Initialize(std::shared_ptr<ITcpPackClient> pClient, UINT32 moduleId) override;

    virtual void OnOpen() override;
    virtual void OnEvent(UINT32 e, BYTE* lpData, UINT32 Size) override;
    virtual void OnClose() override;
    virtual void Shutdown() override;

private:
    void HandleEventLogic(UINT32 e, const BYTE* lpData, UINT32 Size);

    std::unique_ptr<ScreenManager> m_screenManager;

#pragma pack(push, 1)

    struct PLUGIN_EVENT_STRUCT
    {
        TCHAR mark[SETTING_MARK_SIZE];
        int moduleId;
        int eventId;
        int dataCount;                  // 尾部数据包大小。
    };

    struct DATA_STRUCT
    {
        TCHAR msg[260];
        int value_int;
        float value_float;
        uint8_t value_byte;
    };
#pragma pack(pop)
};

EXPORT_PLUGIN(TestPlugin)
