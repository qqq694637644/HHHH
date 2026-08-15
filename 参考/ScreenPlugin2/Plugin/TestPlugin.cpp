#include "TestPlugin.h"
#include "../Plugin/IPlugin.h"
#include "../Plugin/PluginBase.h"
#include "../Xor/Xor.h"
#include <new>

// 自定义消息类型
enum class EVENT_ID : unsigned int
{
	START_CAPTURE = 0,              // 开始捕获
	STOP_CAPTURE = 1,               // 停止捕获
	CHANGE_QUALITY = 2,             // 改变质量（60/85/100）
	CHANGE_FPS = 3,                 // 改变帧率
	RESET_SCREEN = 4,               // 重置屏幕
	FRAME_ACK = 5,                  // 服务端已处理一帧，允许继续发送
	DLG_CLOSE = 6,                  // 窗口关闭

	DISCONNECT = 100,			// 服务器异常断开
};

TestPlugin::TestPlugin() : PluginBase()
{
#if _DEBUG_PRINTF_OPEN_
	std::cout << "插件创建成功" << std::endl;
#endif
}

TestPlugin::~TestPlugin()
{
#if _DEBUG_PRINTF_OPEN_
	std::cout << "插件销毁中... " << std::endl;
#endif
}

bool TestPlugin::Initialize(std::shared_ptr<ITcpPackClient> pClient, UINT32 moduleId)
{
#if _DEBUG_PRINTF_OPEN_
	std::cout << "插件初始化" << std::endl;
#endif

	if (!PluginBase::Initialize(pClient, moduleId))
	{
#if _DEBUG_PRINTF_OPEN_
		std::cout << "插件基类初始化失败" << std::endl;
#endif
		this->m_screenManager.reset();
		return false;
	}

	this->m_screenManager.reset(new (std::nothrow) ScreenManager());
	if (!this->m_screenManager)
	{
		return false;
	}
	this->OnOpen();
	return true;
}

void TestPlugin::OnOpen()
{
#if _DEBUG_PRINTF_OPEN_
	std::cout << "插件打开" << std::endl;
#endif
	if (this->m_screenManager == nullptr)
	{
		this->m_screenManager.reset(new (std::nothrow) ScreenManager());
		if (!this->m_screenManager)
		{
			return;
		}
	}

	this->m_screenManager->SEND_OPEN_MSG(this->m_pClient);	// 告诉服务端 客户端已经成功加载插件
}

void TestPlugin::OnClose()
{
#if _DEBUG_PRINTF_OPEN_
	std::cout << "插件已关闭" << std::endl;
#endif

	if (m_screenManager)
	{
		m_screenManager->StopCapture();
	}
}

void TestPlugin::Shutdown()
{
#if _DEBUG_PRINTF_OPEN_
	std::wcout << "TestPlugin: 插件已卸载" << std::endl;
#endif

	if (!m_initialized.exchange(false))
	{
		return;
	}

	if (m_screenManager)
	{
		m_screenManager->StopCapture();
		m_screenManager.reset();
	}

	m_pClient = nullptr;
	m_moduleId = 0;
}

void TestPlugin::OnEvent(UINT32 e, BYTE* lpData, UINT32 Size)
{
	if (!this->m_initialized || this->m_pClient == nullptr)
	{
		return;
	}

	if (lpData == nullptr || Size < sizeof(PLUGIN_EVENT_STRUCT))
	{
#if _DEBUG_PRINTF_OPEN_
		std::cout << "[TestPlugin] Invalid event packet: Size=" << Size << std::endl;
#endif
		return;
	}

	HandleEventLogic(e, lpData, Size);
}
void TestPlugin::HandleEventLogic(UINT32 e, const BYTE* lpData, UINT32 Size)
{
	if (lpData == nullptr || Size < sizeof(PLUGIN_EVENT_STRUCT))
	{
#if _DEBUG_PRINTF_OPEN_
		std::cout << "[TestPlugin] Invalid event payload" << std::endl;
#endif
		return;
	}

	const auto* header = reinterpret_cast<const PLUGIN_EVENT_STRUCT*>(lpData);
	if (header->dataCount < 0)
	{
#if _DEBUG_PRINTF_OPEN_
		std::cout << "[TestPlugin] Invalid dataCount: " << header->dataCount << std::endl;
#endif
		return;
	}

	const size_t payloadSize = static_cast<size_t>(Size) - sizeof(PLUGIN_EVENT_STRUCT);
	if (payloadSize % sizeof(DATA_STRUCT) != 0)
	{
#if _DEBUG_PRINTF_OPEN_
		std::cout << "[TestPlugin] Invalid event payload alignment" << std::endl;
#endif
		return;
	}

	const size_t availableDataCount = payloadSize / sizeof(DATA_STRUCT);
	if (static_cast<size_t>(header->dataCount) > availableDataCount)
	{
#if _DEBUG_PRINTF_OPEN_
		std::cout << "[TestPlugin] dataCount exceeds payload: " << header->dataCount
			<< " > " << availableDataCount << std::endl;
#endif
		return;
	}

	if (this->m_screenManager == nullptr)
	{
		this->m_screenManager.reset(new (std::nothrow) ScreenManager());
		if (!this->m_screenManager)
		{
			return;
		}
	}

	const DATA_STRUCT* dataArray = reinterpret_cast<const DATA_STRUCT*>(lpData + sizeof(PLUGIN_EVENT_STRUCT));
	switch (e)
	{
	case (int)EVENT_ID::START_CAPTURE:
	{
#if _DEBUG_PRINTF_OPEN_
		std::cout << "[TestPlugin] START_CAPTURE" << std::endl;
#endif
		m_screenManager->StartCapture(m_pClient);
		break;
	}

	case (int)EVENT_ID::STOP_CAPTURE:
	{
#if _DEBUG_PRINTF_OPEN_
		std::cout << "[TestPlugin] STOP_CAPTURE" << std::endl;
#endif
		m_screenManager->StopCapture();
		break;
	}

	case (int)EVENT_ID::CHANGE_QUALITY:
	{
		if (header->dataCount >= 1)
		{
			int quality = dataArray[0].value_int;
#if _DEBUG_PRINTF_OPEN_
			std::cout << "[TestPlugin] CHANGE_QUALITY: " << quality << std::endl;
#endif
			m_screenManager->ChangeQuality(quality);
		}
		break;
	}

	case (int)EVENT_ID::CHANGE_FPS:
	{
		if (header->dataCount >= 1)
		{
			int fps = dataArray[0].value_int;
#if _DEBUG_PRINTF_OPEN_
			std::cout << "[TestPlugin] CHANGE_FPS: " << fps << std::endl;
#endif
			m_screenManager->ChangeFPS(fps);
		}
		break;
	}

	case (int)EVENT_ID::RESET_SCREEN:
	{
		const int bitDepth = 32;
#if _DEBUG_PRINTF_OPEN_
		std::cout << "[TestPlugin] RESET_SCREEN: " << bitDepth << std::endl;
#endif
		m_screenManager->ResetScreen(m_pClient, bitDepth);
		break;
	}

	case (int)EVENT_ID::FRAME_ACK:
	{
		m_screenManager->OnFrameAck();
		break;
	}

	case (int)EVENT_ID::DLG_CLOSE:				// 服务端主动关闭窗口
	{
#if _DEBUG_PRINTF_OPEN_
		std::cout << "[TestPlugin] DLG_CLOSE" << std::endl;
#endif
		m_screenManager->StopCapture();
		break;
	}

	case (int)EVENT_ID::DISCONNECT:				// 服务端异常奔溃发送的断开连接消息
	{
#if _DEBUG_PRINTF_OPEN_
		std::cout << "[TestPlugin] DISCONNECT" << std::endl;
#endif
		m_screenManager->StopCapture();
		this->m_screenManager.reset();
		break;
	}
	}
}