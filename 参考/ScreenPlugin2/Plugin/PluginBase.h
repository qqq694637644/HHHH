#pragma once
#include "IPlugin.h"
#include <atomic>

// 插件基类。ScreenPlugin2 运行在 manual map 插件环境中，避免通用线程池和异常式任务调度。
class PluginBase : public IPlugin
{
public:
    PluginBase()
        : m_pClient(nullptr),
        m_moduleId(0),
        m_initialized(false)
    {
    }

    virtual ~PluginBase()
    {
        Shutdown();
    }

    virtual bool Initialize(std::shared_ptr<ITcpPackClient> pClient, UINT32 moduleId) override;
    virtual void Shutdown() override;
    virtual void ExecuteAsync(std::function<void()> task) override;

    virtual void OnOpen() override;
    virtual void OnEvent(UINT32 e, BYTE* lpData, UINT32 Size) override;
    virtual void OnClose() override;

protected:
    std::shared_ptr<ITcpPackClient>     m_pClient;
    UINT32                              m_moduleId;
    std::atomic<bool>                   m_initialized;
};