#include "PluginBase.h"

bool PluginBase::Initialize(std::shared_ptr<ITcpPackClient> pClient, UINT32 moduleId)
{
    if (m_initialized)
    {
        return false;
    }

    m_pClient = pClient;
    m_moduleId = moduleId;
    m_initialized = true;

    return true;
}

void PluginBase::Shutdown()
{
    if (m_initialized.exchange(false))
    {
        this->OnClose();
        m_pClient = nullptr;
    }
}

void PluginBase::ExecuteAsync(std::function<void()> task)
{
    if (m_initialized && task)
    {
        // Manual-map plugin path: run lightweight control events inline instead of
        // dispatching through a generic thread pool that may allocate or fail unexpectedly.
        task();
    }
}

void PluginBase::OnOpen()
{
}

void PluginBase::OnEvent(UINT32 e, BYTE* lpData, UINT32 Size)
{
}

void PluginBase::OnClose()
{
}