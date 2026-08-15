#pragma once
#ifndef TEST_PLUGIN_IPLUGIN_H
#define TEST_PLUGIN_IPLUGIN_H

#include "../HPSocket/SocketInterface.h"
#include <memory>
#include <functional>
#include <new>

// 插件主接口，由DLL导出的类需要实现此接口
class IPlugin
{
public:
    virtual ~IPlugin() {}

    // 初始化插件，提供必要的环境信息
    virtual bool Initialize(std::shared_ptr<ITcpPackClient> pClient, UINT32 moduleId) = 0;

    virtual void OnOpen() = 0;
    virtual void OnEvent(UINT32 e, BYTE* lpData, UINT32 Size) = 0;
    virtual void OnClose() = 0;

    // 卸载插件
    virtual void Shutdown() = 0;

    // 在线程池中执行任务
    virtual void ExecuteAsync(std::function<void()> task) = 0;
};

// 插件创建函数类型定义
typedef IPlugin* (*CreatePluginFunc)();

// 导出函数名称
#define PLUGIN_CREATE_FUNC_NAME "CreatePlugin"

// 导出函数宏定义
#define EXPORT_PLUGIN(PluginClass) \
    extern "C" __declspec(dllexport) IPlugin* CreatePlugin() { \
        return new (std::nothrow) PluginClass(); \
    }

#endif // TEST_PLUGIN_IPLUGIN_H