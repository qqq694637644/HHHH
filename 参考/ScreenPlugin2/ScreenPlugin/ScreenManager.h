#pragma once
#include "../HPSocket/SocketInterface.h"
#include "ScreenCapture.h"
#include "ScreenProtocol.h"
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <new>

/// <summary>
/// 屏幕管理器：负责屏幕捕获、压缩、发送和线程生命周期管理。
/// </summary>
class ScreenManager
{
public:
    ScreenManager();
    ~ScreenManager();

    // 禁止拷贝和赋值。
    ScreenManager(const ScreenManager&) = delete;
    ScreenManager& operator=(const ScreenManager&) = delete;

    /// <summary>
    /// 发送窗口打开后的屏幕基础信息。
    /// </summary>
    bool SEND_OPEN_MSG(std::shared_ptr<ITcpPackClient> pClient);

    /// <summary>
    /// 开始屏幕捕获。
    /// </summary>
    bool StartCapture(std::shared_ptr<ITcpPackClient> pClient);

    /// <summary>
    /// 停止屏幕捕获。
    /// </summary>
    void StopCapture();

    /// <summary>
    /// 改变 JPEG 质量。
    /// </summary>
    void ChangeQuality(int quality);

    /// <summary>
    /// 改变帧率。
    /// </summary>
    void ChangeFPS(int fps);

    /// <summary>
    /// 重置屏幕捕获并重新初始化资源。
    /// </summary>
    bool ResetScreen(std::shared_ptr<ITcpPackClient> pClient, int bitDepth = 32);

    /// <summary>
    /// 当前是否正在捕获。
    /// </summary>
    bool IsCapturing() const { return m_isCapturing.load(); }

    /// <summary>
    /// 服务端处理完一帧后的确认，用于发送侧背压。
    /// </summary>
    void OnFrameAck();

private:
    struct SendStats
    {
        unsigned long long firstFrames;
        unsigned long long nextFrames;
        unsigned long long totalBytes;
        unsigned long long originalBytes;
        unsigned long long compressedBytes;
        unsigned long long sendMs;
        unsigned long long ackMs;
        unsigned long long ackCount;
        unsigned long long ackTimeouts;
        unsigned long long sendFailures;
        unsigned long long budgetWarnings;
        unsigned long long saturatedDiffs;
        unsigned long long keyframeCooldowns;
        unsigned long long fpsThrottles;
        unsigned long long qualityDrops;
        unsigned int maxPacketBytes;
        unsigned int maxSendMs;
        unsigned int maxAckMs;
        int maxRectCount;
    };

    /// <summary>
    /// 捕获线程入口函数。
    /// </summary>
    static DWORD WINAPI CaptureThreadProc(LPVOID param);
    void CaptureThreadFunc();

    /// <summary>
    /// 发送屏幕尺寸信息。
    /// </summary>
    bool SendScreenSize();

    /// <summary>
    /// 发送第一帧完整图像。
    /// </summary>
    bool SendFirstFrame();

    /// <summary>
    /// 发送后续差异帧。
    /// </summary>
    bool SendNextFrame();

    /// <summary>
    /// 使用 zlib 压缩数据。
    /// </summary>
    std::unique_ptr<BYTE[]> CompressData(const BYTE* input, DWORD inputSize, DWORD& outputSize);

    /// <summary>
    /// 发送数据包，发送前会进行 XOR 加密。
    /// </summary>
    bool SendPacket(const void* data, size_t size);

    /// <summary>
    /// 发送高速屏幕诊断日志到服务端。
    /// </summary>
    bool SendOperationMessage(const wchar_t* message);

    void SendCaptureFailureMessage(ScreenCaptureError error, DWORD win32Error);
    bool CommitSentFrameBaseline(bool isFirstFrame);
    void MarkFrameInFlight(size_t totalBytes, bool isFirstFrame, int rectCount);
    bool CanSendFrame();
    void ClearFrameInFlight();
    void ResetSendStats();
    unsigned long long ReserveFrameSequence() const;
    void AdvanceFrameSequence(unsigned long long sequence);
    void CommitFrameSequence(unsigned long long sequence);
    void RecordFrameSendStats(bool isFirstFrame, DWORD originalSize, DWORD compressedSize,
        size_t totalBytes, int rectCount, bool sent, unsigned int sendMs);
    void RecordFrameAckStats(unsigned int ackMs);
    void MaybeReportSendStats(bool force);
    bool PrepareSaturatedKeyframe();
    void ResetSaturatedDiffStreak();
    std::chrono::milliseconds GetEffectiveFrameDuration();
    void ApplyAckAdaptivePolicy(const SendStats& snapshot, unsigned long long avgAckMs,
        unsigned int& adaptiveFrameMs, int& currentQuality, int& normalAckWindows,
        bool& ackSlow, bool& ackAdaptiveThrottle, bool& ackQualityDrop, bool& adaptiveRestore);

    // ========== 成员变量 ==========

    // 屏幕捕获对象。
    std::unique_ptr<ScreenCapture> m_capture;

    // 当前客户端连接。
    std::shared_ptr<ITcpPackClient> m_pClient;

    // 捕获线程句柄和状态。
    HANDLE m_captureThreadHandle;
    DWORD m_captureThreadId;
    std::atomic<bool> m_isCapturing;
    std::atomic<bool> m_shouldStop;

    // 帧率控制。
    int m_fps;
    std::chrono::milliseconds m_frameDuration;

    // 发送侧背压：最多允许 1 帧未被服务端 ACK。
    std::atomic<bool> m_frameInFlight;
    std::chrono::steady_clock::time_point m_frameInFlightSince;
    size_t m_frameInFlightBytes;
    bool m_frameInFlightIsFirst;
    int m_frameInFlightRectCount;
    unsigned long long m_nextFrameSequence;
    unsigned long long m_lastCommittedSequence;
    bool m_forceKeyframe;
    SendStats m_sendStats;
    std::chrono::steady_clock::time_point m_lastSendStatsReport;
    std::chrono::steady_clock::time_point m_lastSaturatedKeyframeAt;
    std::chrono::steady_clock::time_point m_adaptiveThrottleUntil;
    std::chrono::milliseconds m_adaptiveFrameDuration;
    int m_saturatedDiffStreak;
    bool m_ackAdaptiveThrottleActive;
    bool m_ackAdaptiveQualityActive;
    int m_adaptiveNormalAckWindows;

    // JPEG 质量。
    std::atomic<int> m_quality;
    int m_configuredQuality;

    // 同步对象。
    std::mutex m_mutex;
    std::condition_variable m_cv;

    // 初始化标志。
    std::atomic<bool> m_initialized;
};
