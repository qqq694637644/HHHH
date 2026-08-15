// ClientC++/ScreenPlugin2/ScreenPlugin/ScreenManager.cpp
#include "ScreenManager.h"
#include "../Xor/Xor.h"
#include <new>
#include <climits>
#include <cwchar>

#if _DEBUG_PRINTF_OPEN_
#include <iostream>
#endif
#include <algorithm>
#include "../zlib/include/zconf.h"
#include "../zlib/include/zlib.h"

namespace
{
    constexpr uLong MAX_SCREEN2_COMPRESSED_SIZE = 64UL * 1024UL * 1024UL;
    constexpr auto FRAME_ACK_TIMEOUT = std::chrono::seconds(5);
    constexpr auto SEND_STATS_REPORT_INTERVAL = std::chrono::seconds(3);
    constexpr auto SATURATED_KEYFRAME_COOLDOWN = std::chrono::seconds(2);
    constexpr auto SATURATED_THROTTLE_DURATION = std::chrono::seconds(8);
    constexpr int SATURATED_DIFF_STREAK_TO_THROTTLE = 2;
    constexpr int SATURATED_THROTTLE_FRAME_MS = 1000;
    constexpr auto ACK_ADAPTIVE_DURATION = std::chrono::seconds(9);
    constexpr int ACK_ADAPTIVE_FRAME_MS = 1500;
    constexpr int ACK_TIMEOUT_FRAME_MS = 2000;
    constexpr unsigned int ACK_SLOW_AVG_MS = 1000;
    constexpr unsigned int ACK_SLOW_MAX_MS = 2000;
    constexpr unsigned int ACK_VERY_SLOW_AVG_MS = 1500;
    constexpr unsigned int ACK_VERY_SLOW_MAX_MS = 3000;
    constexpr unsigned int ACK_NORMAL_AVG_MS = 700;
    constexpr unsigned int ACK_NORMAL_MAX_MS = 1200;
    constexpr unsigned int ACK_LARGE_PACKET_BYTES = 400 * 1024;
    constexpr int ACK_ADAPTIVE_QUALITY = 70;
    constexpr int ACK_NORMAL_WINDOWS_TO_RESTORE = 3;

    bool XorEncryptToBuffer(const BYTE* input, size_t size, BYTE* output)
    {
        if (!input || !output || size == 0 || keyStr == nullptr)
        {
            return false;
        }

        const size_t keyLength = wcslen(keyStr) * sizeof(wchar_t);
        if (keyLength == 0)
        {
            return false;
        }

        const BYTE* key = reinterpret_cast<const BYTE*>(keyStr);
        for (size_t i = 0; i < size; ++i)
        {
            output[i] = input[i] ^ key[i % keyLength];
        }
        return true;
    }
}
// ============================================================
// 构造函数
// ============================================================

ScreenManager::ScreenManager()
    : m_capture(nullptr)
    , m_pClient(nullptr)
    , m_captureThreadHandle(nullptr)
    , m_captureThreadId(0)
    , m_isCapturing(false)
    , m_shouldStop(false)
    , m_fps(DEFAULT_FPS)
    , m_frameDuration(1000 / DEFAULT_FPS)
    , m_frameInFlight(false)
    , m_frameInFlightSince(std::chrono::steady_clock::now())
    , m_frameInFlightBytes(0)
    , m_frameInFlightIsFirst(false)
    , m_frameInFlightRectCount(0)
    , m_nextFrameSequence(1)
    , m_lastCommittedSequence(0)
    , m_forceKeyframe(false)
    , m_lastSendStatsReport()
    , m_lastSaturatedKeyframeAt()
    , m_adaptiveThrottleUntil()
    , m_adaptiveFrameDuration(0)
    , m_saturatedDiffStreak(0)
    , m_ackAdaptiveThrottleActive(false)
    , m_ackAdaptiveQualityActive(false)
    , m_adaptiveNormalAckWindows(0)
    , m_quality(DEFAULT_JPEG_QUALITY)
    , m_configuredQuality(DEFAULT_JPEG_QUALITY)
    , m_initialized(false)
{
    ResetSendStats();
#if _DEBUG_PRINTF_OPEN_
    std::cout << "[ScreenManager] Constructing..." << std::endl;
#endif
}

// ============================================================
// 析构函数
// ============================================================

ScreenManager::~ScreenManager()
{
#if _DEBUG_PRINTF_OPEN_
    std::cout << "[ScreenManager] Destructing..." << std::endl;
#endif

    StopCapture();
    m_capture.reset();
    m_pClient.reset();
}

// ============================================================
// 发送打开消息
// ============================================================

bool ScreenManager::SEND_OPEN_MSG(std::shared_ptr<ITcpPackClient> pClient)
{
    if (!pClient)
    {
#if _DEBUG_PRINTF_OPEN_
        std::cout << "[ScreenManager] ERROR: pClient is null" << std::endl;
#endif
        return false;
    }

    m_pClient = pClient;

    int bitDepth = 32;
    std::unique_ptr<ScreenCapture> tempCapture(new (std::nothrow) ScreenCapture(bitDepth, m_quality.load()));
    if (!tempCapture || tempCapture->GetScreenWidth() <= 0 || tempCapture->GetScreenHeight() <= 0)
    {
#if _DEBUG_PRINTF_OPEN_
        std::cout << "[ScreenManager] ERROR: Failed to get screen info" << std::endl;
#endif
        return false;
    }

    SCREEN_OPEN_MSG msg = { 0 };
    wcscpy_s(msg.mark, L"SCREEN2_RET_OPEN");
    msg.screenWidth = tempCapture->GetScreenWidth();
    msg.screenHeight = tempCapture->GetScreenHeight();
    msg.bitDepth = tempCapture->GetBitDepth();

    bool result = SendPacket(&msg, sizeof(SCREEN_OPEN_MSG));

#if _DEBUG_PRINTF_OPEN_
    if (result)
    {
        std::cout << "[ScreenManager] OPEN message sent: " << msg.screenWidth
            << "x" << msg.screenHeight << std::endl;
    }
    else
    {
        std::cout << "[ScreenManager] ERROR: Failed to send OPEN message" << std::endl;
    }
#endif

    m_initialized.store(result);
    return result;
}

// ============================================================
// 开始捕获
// ============================================================

bool ScreenManager::StartCapture(std::shared_ptr<ITcpPackClient> pClient)
{
    HANDLE staleThread = nullptr;
    std::unique_lock<std::mutex> lock(m_mutex);

    if (m_isCapturing.load())
    {
#if _DEBUG_PRINTF_OPEN_
        std::cout << "[ScreenManager] Already capturing" << std::endl;
#endif
        return true;
    }

    if (m_captureThreadHandle)
    {
        m_shouldStop.store(true);
        m_cv.notify_all();
        staleThread = m_captureThreadHandle;
        m_captureThreadHandle = nullptr;
        m_captureThreadId = 0;
        lock.unlock();
        WaitForSingleObject(staleThread, INFINITE);
        CloseHandle(staleThread);
        lock.lock();
        if (m_isCapturing.load()) return true;
    }

    if (!pClient)
    {
#if _DEBUG_PRINTF_OPEN_
        std::cout << "[ScreenManager] ERROR: pClient is null" << std::endl;
#endif
        return false;
    }

    m_pClient = pClient;
    if (!m_capture)
    {
        std::unique_ptr<ScreenCapture> capture(new (std::nothrow) ScreenCapture(32, m_quality.load()));
        if (!capture || capture->GetScreenWidth() <= 0 || capture->GetScreenHeight() <= 0)
        {
#if _DEBUG_PRINTF_OPEN_
            std::cout << "[ScreenManager] ERROR: Failed to create ScreenCapture" << std::endl;
#endif
            return false;
        }
        m_capture = std::move(capture);
#if _DEBUG_PRINTF_OPEN_
        std::cout << "[ScreenManager] ScreenCapture created with quality: " << m_quality.load() << std::endl;
#endif
    }

    m_shouldStop.store(false);
    ClearFrameInFlight();
    m_nextFrameSequence = 1;
    m_lastCommittedSequence = 0;
    m_lastSaturatedKeyframeAt = std::chrono::steady_clock::time_point();
    m_adaptiveThrottleUntil = std::chrono::steady_clock::time_point();
    m_adaptiveFrameDuration = std::chrono::milliseconds(0);
    m_saturatedDiffStreak = 0;
    m_ackAdaptiveThrottleActive = false;
    m_ackAdaptiveQualityActive = false;
    m_adaptiveNormalAckWindows = 0;
    m_quality.store(m_configuredQuality);
    if (m_capture)
    {
        m_capture->SetQuality(m_configuredQuality);
    }
    m_forceKeyframe = false;
    m_isCapturing.store(true);

    HANDLE threadHandle = CreateThread(nullptr, 0, &ScreenManager::CaptureThreadProc, this, 0, &m_captureThreadId);
    if (!threadHandle)
    {
#if _DEBUG_PRINTF_OPEN_
        std::cout << "[ScreenManager] ERROR: Failed to create capture thread" << std::endl;
#endif
        m_shouldStop.store(true);
        m_isCapturing.store(false);
        m_captureThreadId = 0;
        return false;
    }
    m_captureThreadHandle = threadHandle;

#if _DEBUG_PRINTF_OPEN_
    std::cout << "[ScreenManager] Capture started" << std::endl;
#endif
    return true;
}

// ============================================================
// 停止捕获
// ============================================================

void ScreenManager::StopCapture()
{
    HANDLE threadToWait = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_isCapturing.load() && !m_captureThreadHandle)
        {
            return;
        }
#if _DEBUG_PRINTF_OPEN_
        std::cout << "[ScreenManager] Stopping capture..." << std::endl;
#endif
        m_shouldStop.store(true);
        m_isCapturing.store(false);
        ClearFrameInFlight();
        m_cv.notify_all();
        threadToWait = m_captureThreadHandle;
        m_captureThreadHandle = nullptr;
        m_captureThreadId = 0;
    }

    if (threadToWait)
    {
        WaitForSingleObject(threadToWait, INFINITE);
        CloseHandle(threadToWait);
    }
    MaybeReportSendStats(true);
#if _DEBUG_PRINTF_OPEN_
    std::cout << "[ScreenManager] Capture stopped" << std::endl;
#endif
}

// ============================================================
// 改变质量
// ============================================================

void ScreenManager::ChangeQuality(int quality)
{
    const int newQuality = std::clamp(quality, 1, 100);
    m_quality.store(newQuality);

    std::lock_guard<std::mutex> lock(m_mutex);
    m_configuredQuality = newQuality;
    m_ackAdaptiveQualityActive = false;
    m_adaptiveNormalAckWindows = 0;
    if (m_capture)
    {
        m_capture->SetQuality(newQuality);
    }

#if _DEBUG_PRINTF_OPEN_
    std::cout << "[ScreenManager] Quality changed to: " << newQuality << std::endl;
#endif
}

// ============================================================
// 改变帧率
// ============================================================

void ScreenManager::ChangeFPS(int fps)
{
    fps = std::clamp(fps, 1, MAX_FPS);

    std::lock_guard<std::mutex> lock(m_mutex);  // 加锁保护
    m_fps = fps;
    m_frameDuration = std::chrono::milliseconds(1000 / fps);
    m_adaptiveThrottleUntil = std::chrono::steady_clock::time_point();
    m_adaptiveFrameDuration = std::chrono::milliseconds(0);
    m_saturatedDiffStreak = 0;
    m_ackAdaptiveThrottleActive = false;
    m_adaptiveNormalAckWindows = 0;

#if _DEBUG_PRINTF_OPEN_
    std::cout << "[ScreenManager] FPS changed to: " << fps << std::endl;
#endif

    // 通知条件变量，立即应用新的帧率
    m_cv.notify_all();
}

// ============================================================
// 重置屏幕
// ============================================================

bool ScreenManager::ResetScreen(std::shared_ptr<ITcpPackClient> pClient, int bitDepth)
{
    bitDepth = 32;
    const bool wasCapturing = m_isCapturing.load();
    StopCapture();

    std::unique_ptr<ScreenCapture> newCapture(new (std::nothrow) ScreenCapture(bitDepth, m_quality.load()));
    if (!newCapture || newCapture->GetScreenWidth() <= 0 || newCapture->GetScreenHeight() <= 0)
    {
#if _DEBUG_PRINTF_OPEN_
        std::cout << "[ScreenManager] ERROR: Failed to reset" << std::endl;
#endif
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_capture = std::move(newCapture);
    }

    if (wasCapturing)
    {
        return StartCapture(pClient);
    }
    return true;
}

// ============================================================
// zlib 压缩
// ============================================================

std::unique_ptr<BYTE[]> ScreenManager::CompressData(const BYTE* input, DWORD inputSize, DWORD& outputSize)
{
    outputSize = 0;
    if (!input || inputSize == 0)
    {
        return nullptr;
    }

    uLongf destLen = compressBound(static_cast<uLong>(inputSize));
    if (destLen == 0 || destLen > MAX_SCREEN2_COMPRESSED_SIZE)
    {
#if _DEBUG_PRINTF_OPEN_
        std::cout << "[ScreenManager] ERROR: zlib bound too large: " << destLen << std::endl;
#endif
        return nullptr;
    }

    std::unique_ptr<BYTE[]> compressed(new (std::nothrow) BYTE[destLen]);
    if (!compressed)
    {
#if _DEBUG_PRINTF_OPEN_
        std::cout << "[ScreenManager] ERROR: zlib output allocation failed" << std::endl;
#endif
        return nullptr;
    }

    int result = compress(compressed.get(), &destLen, input, inputSize);
    if (result != Z_OK || destLen == 0 || destLen > MAX_SCREEN2_COMPRESSED_SIZE)
    {
#if _DEBUG_PRINTF_OPEN_
        std::cout << "[ScreenManager] ERROR: zlib compress failed: " << result << std::endl;
#endif
        return nullptr;
    }

    outputSize = static_cast<DWORD>(destLen);

#if _DEBUG_PRINTF_OPEN_
    double ratio = (double)inputSize / (double)destLen;
    std::cout << "[ScreenManager] Compressed: " << inputSize << " -> " << destLen
        << " (ratio: " << ratio << ")" << std::endl;
#endif

    return compressed;
}

// ============================================================
// 发送数据包
// ============================================================

bool ScreenManager::SendPacket(const void* data, size_t size)
{
    if (!m_pClient || !data || size == 0 || size > static_cast<size_t>(INT_MAX))
    {
        return false;
    }

    std::unique_ptr<BYTE[]> encrypted(new (std::nothrow) BYTE[size]);
    if (!encrypted)
    {
        return false;
    }

    if (!XorEncryptToBuffer(static_cast<const BYTE*>(data), size, encrypted.get()))
    {
        return false;
    }

    return m_pClient->Send(encrypted.get(), static_cast<int>(size));
}

void ScreenManager::OnFrameAck()
{
    unsigned int ackMs = 0;
    bool hadInFlight = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_frameInFlight.load())
        {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - m_frameInFlightSince);
            ackMs = elapsed.count() > 0 ? static_cast<unsigned int>(elapsed.count()) : 0;
            m_frameInFlight.store(false);
            hadInFlight = true;
        }
    }

    if (hadInFlight)
    {
        RecordFrameAckStats(ackMs);
    }
    m_cv.notify_all();
    MaybeReportSendStats(false);
}

void ScreenManager::MarkFrameInFlight(size_t totalBytes, bool isFirstFrame, int rectCount)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_frameInFlightSince = std::chrono::steady_clock::now();
    m_frameInFlightBytes = totalBytes;
    m_frameInFlightIsFirst = isFirstFrame;
    m_frameInFlightRectCount = rectCount;
    m_frameInFlight.store(true);
}

bool ScreenManager::CommitSentFrameBaseline(bool isFirstFrame)
{
    bool committed = m_capture && m_capture->CommitCurrentFrame();
    if (committed)
    {
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_forceKeyframe = true;
    }

    wchar_t message[192] = { 0 };
    swprintf_s(message,
        L"[SCREEN2][WARN] %ls frame baseline commit failed after send, forcing keyframe",
        isFirstFrame ? L"first" : L"next");
    SendOperationMessage(message);
    return false;
}

bool ScreenManager::CanSendFrame()
{
    if (!m_frameInFlight.load())
    {
        return true;
    }

    bool timedOut = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_frameInFlight.load())
        {
            return true;
        }

        if (std::chrono::steady_clock::now() - m_frameInFlightSince >= FRAME_ACK_TIMEOUT)
        {
            m_frameInFlight.store(false);
            m_forceKeyframe = true;
            m_adaptiveFrameDuration = std::chrono::milliseconds(ACK_TIMEOUT_FRAME_MS);
            m_adaptiveThrottleUntil = std::chrono::steady_clock::now() + ACK_ADAPTIVE_DURATION;
            m_ackAdaptiveThrottleActive = true;
            m_adaptiveNormalAckWindows = 0;
            ++m_sendStats.ackTimeouts;
            timedOut = true;
        }
    }

    if (timedOut)
    {
        SendOperationMessage(L"[SCREEN2][WARN] frame ACK timeout, forcing keyframe and throttling capture interval to 2000 ms");
        return true;
    }

    return false;
}

void ScreenManager::ClearFrameInFlight()
{
    m_frameInFlight.store(false);
}

void ScreenManager::ResetSendStats()
{
    ZeroMemory(&m_sendStats, sizeof(m_sendStats));
    m_lastSendStatsReport = std::chrono::steady_clock::now();
}

unsigned long long ScreenManager::ReserveFrameSequence() const
{
    return m_nextFrameSequence;
}

void ScreenManager::AdvanceFrameSequence(unsigned long long sequence)
{
    if (sequence == 0)
    {
        return;
    }
    if (m_nextFrameSequence <= sequence)
    {
        m_nextFrameSequence = sequence + 1;
    }
}

void ScreenManager::CommitFrameSequence(unsigned long long sequence)
{
    if (sequence == 0)
    {
        return;
    }
    AdvanceFrameSequence(sequence);
    m_lastCommittedSequence = sequence;
}

void ScreenManager::RecordFrameSendStats(bool isFirstFrame, DWORD originalSize, DWORD compressedSize,
    size_t totalBytes, int rectCount, bool sent, unsigned int sendMs)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (isFirstFrame)
    {
        ++m_sendStats.firstFrames;
    }
    else
    {
        ++m_sendStats.nextFrames;
    }

    m_sendStats.totalBytes += static_cast<unsigned long long>(totalBytes);
    m_sendStats.originalBytes += originalSize;
    m_sendStats.compressedBytes += compressedSize;
    m_sendStats.sendMs += sendMs;
    if (sendMs > m_sendStats.maxSendMs)
    {
        m_sendStats.maxSendMs = sendMs;
    }
    if (totalBytes > m_sendStats.maxPacketBytes)
    {
        const size_t cappedBytes = totalBytes > static_cast<size_t>(MAXDWORD) ? static_cast<size_t>(MAXDWORD) : totalBytes;
        m_sendStats.maxPacketBytes = static_cast<unsigned int>(cappedBytes);
    }
    if (rectCount > m_sendStats.maxRectCount)
    {
        m_sendStats.maxRectCount = rectCount;
    }
    if (!sent)
    {
        ++m_sendStats.sendFailures;
    }
    if (totalBytes >= MAX_FRAME_PACKET_SIZE)
    {
        ++m_sendStats.budgetWarnings;
    }
}

void ScreenManager::RecordFrameAckStats(unsigned int ackMs)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_sendStats.ackCount;
    m_sendStats.ackMs += ackMs;
    if (ackMs > m_sendStats.maxAckMs)
    {
        m_sendStats.maxAckMs = ackMs;
    }
}

void ScreenManager::MaybeReportSendStats(bool force)
{
    SendStats snapshot = {};
    bool shouldReport = false;
    unsigned long long intervalMs = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto now = std::chrono::steady_clock::now();
        if (force || now - m_lastSendStatsReport >= SEND_STATS_REPORT_INTERVAL)
        {
            if (m_sendStats.firstFrames > 0 || m_sendStats.nextFrames > 0 ||
                m_sendStats.ackCount > 0 || m_sendStats.ackTimeouts > 0 ||
                m_sendStats.sendFailures > 0 || m_sendStats.saturatedDiffs > 0 ||
                m_sendStats.keyframeCooldowns > 0 || m_sendStats.fpsThrottles > 0 ||
                m_sendStats.qualityDrops > 0)
            {
                snapshot = m_sendStats;
                ZeroMemory(&m_sendStats, sizeof(m_sendStats));
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_lastSendStatsReport);
                intervalMs = elapsed.count() > 0 ? static_cast<unsigned long long>(elapsed.count()) : 1;
                shouldReport = true;
            }
            m_lastSendStatsReport = now;
        }
    }

    if (!shouldReport)
    {
        return;
    }

    const unsigned long long frames = snapshot.firstFrames + snapshot.nextFrames;
    const unsigned long long avgSendMs = frames > 0 ? snapshot.sendMs / frames : 0;
    const unsigned long long avgAckMs = snapshot.ackCount > 0 ? snapshot.ackMs / snapshot.ackCount : 0;
    const unsigned long long bytesPerSec = intervalMs > 0 ? (snapshot.totalBytes * 1000ULL) / intervalMs : 0;
    unsigned int adaptiveFrameMs = 0;
    int currentQuality = m_quality.load();
    int normalAckWindows = 0;
    bool ackSlow = false;
    bool ackAdaptiveThrottle = false;
    bool ackQualityDrop = false;
    bool adaptiveRestore = false;
    ApplyAckAdaptivePolicy(snapshot, avgAckMs, adaptiveFrameMs, currentQuality, normalAckWindows,
        ackSlow, ackAdaptiveThrottle, ackQualityDrop, adaptiveRestore);
    wchar_t message[1024] = { 0 };
    swprintf_s(message,
        L"[SCREEN2_STATS][client] first=%llu next=%llu bytes=%llu bps=%llu raw=%llu comp=%llu avgSendMs=%llu maxSendMs=%u ackCount=%llu avgAckMs=%llu maxAckMs=%u ackTimeouts=%llu sendFail=%llu budgetWarn=%llu satDiff=%llu kfCooldown=%llu fpsThrottle=%llu qualityDrop=%llu ackSlow=%u ackAdaptive=%u ackQDrop=%u adaptiveRestore=%u adaptiveMs=%u quality=%d normalAckWin=%d maxPacket=%u maxRects=%d packetBudget=%lu",
        snapshot.firstFrames,
        snapshot.nextFrames,
        snapshot.totalBytes,
        bytesPerSec,
        snapshot.originalBytes,
        snapshot.compressedBytes,
        avgSendMs,
        snapshot.maxSendMs,
        snapshot.ackCount,
        avgAckMs,
        snapshot.maxAckMs,
        snapshot.ackTimeouts,
        snapshot.sendFailures,
        snapshot.budgetWarnings,
        snapshot.saturatedDiffs,
        snapshot.keyframeCooldowns,
        snapshot.fpsThrottles,
        snapshot.qualityDrops,
        ackSlow ? 1U : 0U,
        ackAdaptiveThrottle ? 1U : 0U,
        ackQualityDrop ? 1U : 0U,
        adaptiveRestore ? 1U : 0U,
        adaptiveFrameMs,
        currentQuality,
        normalAckWindows,
        snapshot.maxPacketBytes,
        snapshot.maxRectCount,
        static_cast<unsigned long>(MAX_FRAME_PACKET_SIZE));
    SendOperationMessage(message);
}

void ScreenManager::ApplyAckAdaptivePolicy(const SendStats& snapshot, unsigned long long avgAckMs,
    unsigned int& adaptiveFrameMs, int& currentQuality, int& normalAckWindows,
    bool& ackSlow, bool& ackAdaptiveThrottle, bool& ackQualityDrop, bool& adaptiveRestore)
{
    bool sendThrottleMessage = false;
    bool sendQualityMessage = false;
    bool sendRestoreMessage = false;
    int oldQuality = 0;
    int newQuality = 0;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto now = std::chrono::steady_clock::now();
        const bool hasAckSignal = snapshot.ackCount > 0 || snapshot.ackTimeouts > 0;
        const bool singleVerySlowAck = snapshot.ackCount == 1 && snapshot.maxAckMs > ACK_VERY_SLOW_MAX_MS;
        const bool slowByAck = snapshot.ackTimeouts > 0 || singleVerySlowAck ||
            (snapshot.ackCount >= 2 && (avgAckMs > ACK_SLOW_AVG_MS || snapshot.maxAckMs > ACK_SLOW_MAX_MS));
        const bool verySlowByAck = snapshot.ackTimeouts > 0 || singleVerySlowAck ||
            (snapshot.ackCount >= 2 && (avgAckMs > ACK_VERY_SLOW_AVG_MS || snapshot.maxAckMs > ACK_VERY_SLOW_MAX_MS));
        const bool largePacketSlow = snapshot.ackCount > 0 && slowByAck &&
            snapshot.maxPacketBytes > ACK_LARGE_PACKET_BYTES;
        const bool normalAck = snapshot.ackCount > 0 && snapshot.ackTimeouts == 0 &&
            avgAckMs <= ACK_NORMAL_AVG_MS && snapshot.maxAckMs <= ACK_NORMAL_MAX_MS;

        ackSlow = slowByAck;
        if (slowByAck)
        {
            std::chrono::milliseconds targetDuration(
                verySlowByAck ? ACK_TIMEOUT_FRAME_MS : ACK_ADAPTIVE_FRAME_MS);
            if (m_adaptiveFrameDuration < targetDuration)
            {
                m_adaptiveFrameDuration = targetDuration;
            }
            m_adaptiveThrottleUntil = now + ACK_ADAPTIVE_DURATION;
            m_ackAdaptiveThrottleActive = true;
            m_adaptiveNormalAckWindows = 0;
            ackAdaptiveThrottle = true;
            sendThrottleMessage = true;
            if (snapshot.ackTimeouts > 0)
            {
                m_forceKeyframe = true;
            }
        }
        else if (normalAck)
        {
            ++m_adaptiveNormalAckWindows;
        }
        else if (hasAckSignal)
        {
            m_adaptiveNormalAckWindows = 0;
        }

        if (largePacketSlow && m_quality.load() > ACK_ADAPTIVE_QUALITY)
        {
            oldQuality = m_quality.load();
            newQuality = (std::max)(MIN_ADAPTIVE_JPEG_QUALITY, ACK_ADAPTIVE_QUALITY);
            if (newQuality < oldQuality)
            {
                m_quality.store(newQuality);
                if (m_capture)
                {
                    m_capture->SetQuality(newQuality);
                }
                m_ackAdaptiveQualityActive = true;
                ackQualityDrop = true;
                sendQualityMessage = true;
            }
        }

        if (m_adaptiveNormalAckWindows >= ACK_NORMAL_WINDOWS_TO_RESTORE)
        {
            bool restored = false;
            if (m_ackAdaptiveThrottleActive)
            {
                m_adaptiveThrottleUntil = std::chrono::steady_clock::time_point();
                m_adaptiveFrameDuration = std::chrono::milliseconds(0);
                m_ackAdaptiveThrottleActive = false;
                restored = true;
            }
            if (m_ackAdaptiveQualityActive && m_quality.load() != m_configuredQuality)
            {
                m_quality.store(m_configuredQuality);
                if (m_capture)
                {
                    m_capture->SetQuality(m_configuredQuality);
                }
                m_ackAdaptiveQualityActive = false;
                restored = true;
            }
            if (restored)
            {
                adaptiveRestore = true;
                sendRestoreMessage = true;
                m_adaptiveNormalAckWindows = 0;
            }
        }

        if (m_adaptiveThrottleUntil.time_since_epoch().count() > 0 && now < m_adaptiveThrottleUntil)
        {
            adaptiveFrameMs = static_cast<unsigned int>(m_adaptiveFrameDuration.count());
        }
        currentQuality = m_quality.load();
        normalAckWindows = m_adaptiveNormalAckWindows;
    }

    if (sendThrottleMessage)
    {
        wchar_t message[192] = { 0 };
        swprintf_s(message,
            L"[SCREEN2][WARN] slow ACK detected, throttling capture interval to %u ms: avgAckMs=%llu maxAckMs=%u ackTimeouts=%llu",
            adaptiveFrameMs,
            avgAckMs,
            snapshot.maxAckMs,
            snapshot.ackTimeouts);
        SendOperationMessage(message);
    }
    if (sendQualityMessage)
    {
        wchar_t message[192] = { 0 };
        swprintf_s(message,
            L"[SCREEN2][WARN] large packet with slow ACK, lowering quality: %d->%d maxPacket=%u avgAckMs=%llu",
            oldQuality,
            newQuality,
            snapshot.maxPacketBytes,
            avgAckMs);
        SendOperationMessage(message);
    }
    if (sendRestoreMessage)
    {
        SendOperationMessage(L"[SCREEN2][OK] ACK latency recovered, restoring adaptive FPS/quality limits");
    }
}

bool ScreenManager::PrepareSaturatedKeyframe()
{
    bool throttled = false;
    bool cooledDown = false;
    unsigned int cooldownMs = 0;
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        ++m_sendStats.saturatedDiffs;
        ++m_saturatedDiffStreak;

        auto now = std::chrono::steady_clock::now();
        if (m_saturatedDiffStreak >= SATURATED_DIFF_STREAK_TO_THROTTLE)
        {
            std::chrono::milliseconds throttleDuration(SATURATED_THROTTLE_FRAME_MS);
            if (m_adaptiveFrameDuration < throttleDuration)
            {
                m_adaptiveFrameDuration = throttleDuration;
                throttled = true;
                ++m_sendStats.fpsThrottles;
            }
            m_adaptiveThrottleUntil = now + SATURATED_THROTTLE_DURATION;
        }

        if (m_lastSaturatedKeyframeAt.time_since_epoch().count() > 0)
        {
            auto nextAllowed = m_lastSaturatedKeyframeAt + SATURATED_KEYFRAME_COOLDOWN;
            if (now < nextAllowed)
            {
                auto waitDuration = nextAllowed - now;
                auto waitMs = std::chrono::duration_cast<std::chrono::milliseconds>(waitDuration);
                cooldownMs = waitMs.count() > 0 ? static_cast<unsigned int>(waitMs.count()) : 0;
                cooledDown = true;
                ++m_sendStats.keyframeCooldowns;
                if (m_cv.wait_for(lock, waitDuration, [this]() { return m_shouldStop.load(); }))
                {
                    return false;
                }
            }
        }

        m_lastSaturatedKeyframeAt = std::chrono::steady_clock::now();
    }

    if (cooledDown)
    {
        wchar_t message[160] = { 0 };
        swprintf_s(message, L"[SCREEN2][WARN] saturated diff keyframe cooldown waited %u ms", cooldownMs);
        SendOperationMessage(message);
    }
    if (throttled)
    {
        wchar_t message[192] = { 0 };
        swprintf_s(message, L"[SCREEN2][WARN] saturated diff throttling capture interval to %d ms", SATURATED_THROTTLE_FRAME_MS);
        SendOperationMessage(message);
    }
    MaybeReportSendStats(false);
    return !m_shouldStop.load();
}

void ScreenManager::ResetSaturatedDiffStreak()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_saturatedDiffStreak = 0;
}

std::chrono::milliseconds ScreenManager::GetEffectiveFrameDuration()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto now = std::chrono::steady_clock::now();
    if (m_adaptiveThrottleUntil.time_since_epoch().count() > 0 && now >= m_adaptiveThrottleUntil)
    {
        m_adaptiveThrottleUntil = std::chrono::steady_clock::time_point();
        m_adaptiveFrameDuration = std::chrono::milliseconds(0);
        m_saturatedDiffStreak = 0;
        m_ackAdaptiveThrottleActive = false;
    }

    if (m_adaptiveFrameDuration > m_frameDuration &&
        m_adaptiveThrottleUntil.time_since_epoch().count() > 0 && now < m_adaptiveThrottleUntil)
    {
        return m_adaptiveFrameDuration;
    }

    return m_frameDuration;
}

bool ScreenManager::SendOperationMessage(const wchar_t* message)
{
    if (!message)
    {
        return false;
    }

    SCREEN_OPERATION_MSG msg = { 0 };
    wcscpy_s(msg.mark, L"SCREEN_RET_OPERACTION");
    wcsncpy_s(msg.msg, message, _TRUNCATE);
    return SendPacket(&msg, sizeof(SCREEN_OPERATION_MSG));
}

void ScreenManager::SendCaptureFailureMessage(ScreenCaptureError error, DWORD win32Error)
{
    const wchar_t* stage = L"Unknown";
    switch (error)
    {
    case ScreenCaptureError::NotInitialized:
        stage = L"NotInitialized";
        break;
    case ScreenCaptureError::SwitchDesktopFailed:
        stage = L"SwitchToInputDesktop";
        break;
    case ScreenCaptureError::RefreshDesktopDcFailed:
        stage = L"RefreshDesktopDC";
        break;
    case ScreenCaptureError::BitBltFailed:
        stage = L"BitBlt";
        break;
    case ScreenCaptureError::JpegCompressFailed:
        stage = L"JpegCompress";
        break;
    case ScreenCaptureError::OutputAllocationFailed:
        stage = L"OutputAllocation";
        break;
    case ScreenCaptureError::None:
    default:
        break;
    }

    wchar_t message[256] = { 0 };
    if (win32Error != 0)
    {
        swprintf_s(message, L"[SCREEN2][ERROR] SendFirstFrame: CaptureFirstFrame failed at %ls, win32=%lu", stage, win32Error);
    }
    else
    {
        swprintf_s(message, L"[SCREEN2][ERROR] SendFirstFrame: CaptureFirstFrame failed at %ls", stage);
    }
    SendOperationMessage(message);
}

// ============================================================
// 发送屏幕尺寸
// ============================================================
bool ScreenManager::SendScreenSize()
{
    if (!m_capture)
    {
        return false;
    }

    SCREEN_SIZE_MSG msg = { 0 };
    wcscpy_s(msg.mark, L"SCREEN2_RET_SIZE");
    msg.virtualX = m_capture->GetVirtualX();
    msg.virtualY = m_capture->GetVirtualY();
    msg.width = m_capture->GetScreenWidth();
    msg.height = m_capture->GetScreenHeight();

    bool result = SendPacket(&msg, sizeof(SCREEN_SIZE_MSG));

#if _DEBUG_PRINTF_OPEN_
    if (result)
    {
        std::cout << "[ScreenManager] Size sent: " << msg.width << "x" << msg.height << std::endl;
    }
#endif

    return result;
}

// ============================================================
// 发送第一帧
// ============================================================

bool ScreenManager::SendFirstFrame()
{
    if (!m_capture)
    {
        SendOperationMessage(L"[SCREEN2][ERROR] SendFirstFrame: capture object is null");
        return false;
    }

    DWORD jpegSize = 0;
    DWORD compressedSize = 0;
    size_t totalSize = 0;
    std::unique_ptr<BYTE[]> compressed;

    for (;;)
    {
        // 捕获第一帧
        auto jpegData = m_capture->CaptureFirstFrame(jpegSize);

        if (!jpegData || jpegSize == 0)
        {
#if _DEBUG_PRINTF_OPEN_
            std::cout << "[ScreenManager] ERROR: Failed to capture first frame" << std::endl;
#endif
            SendCaptureFailureMessage(m_capture->GetLastCaptureError(), m_capture->GetLastWin32Error());
            return false;
        }

        // zlib 压缩
        compressedSize = 0;
        compressed = CompressData(jpegData.get(), jpegSize, compressedSize);

        if (!compressed || compressedSize == 0)
        {
#if _DEBUG_PRINTF_OPEN_
            std::cout << "[ScreenManager] ERROR: Failed to compress first frame" << std::endl;
#endif
            SendOperationMessage(L"[SCREEN2][ERROR] SendFirstFrame: zlib compression failed");
            return false;
        }

        totalSize = sizeof(SCREEN_FRAME_V2) + compressedSize;
        if (compressedSize <= MAX_COMPRESSED_FIRST_SIZE && totalSize <= MAX_FRAME_PACKET_SIZE)
        {
            break;
        }

        const int currentQuality = m_quality.load();
        const int nextQuality = (std::max)(MIN_ADAPTIVE_JPEG_QUALITY, currentQuality - ADAPTIVE_JPEG_QUALITY_STEP);
        if (nextQuality >= currentQuality || currentQuality <= MIN_ADAPTIVE_JPEG_QUALITY)
        {
            RecordFrameSendStats(true, jpegSize, compressedSize, totalSize, 0, false, 0);
            MaybeReportSendStats(false);

            wchar_t message[256] = { 0 };
            swprintf_s(message,
                L"[SCREEN2][WARN] first frame packet exceeds budget at min quality, skip send: quality=%d compressed=%lu total=%llu packetBudget=%lu",
                currentQuality,
                static_cast<unsigned long>(compressedSize),
                static_cast<unsigned long long>(totalSize),
                static_cast<unsigned long>(MAX_FRAME_PACKET_SIZE));
            SendOperationMessage(message);
            return false;
        }

        m_quality.store(nextQuality);
        m_capture->SetQuality(nextQuality);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            ++m_sendStats.qualityDrops;
        }
        MaybeReportSendStats(false);

        wchar_t message[256] = { 0 };
        swprintf_s(message,
            L"[SCREEN2][WARN] first frame packet exceeds budget, lower quality: %d->%d compressed=%lu total=%llu packetBudget=%lu",
            currentQuality,
            nextQuality,
            static_cast<unsigned long>(compressedSize),
            static_cast<unsigned long long>(totalSize),
            static_cast<unsigned long>(MAX_FRAME_PACKET_SIZE));
        SendOperationMessage(message);
    }

    std::unique_ptr<BYTE[]> packet(new (std::nothrow) BYTE[totalSize]);
    if (!packet)
    {
        SendOperationMessage(L"[SCREEN2][ERROR] SendFirstFrame: packet allocation failed");
        return false;
    }

    const unsigned long long sequence = ReserveFrameSequence();

    // 填充 V2 统一帧头
    SCREEN_FRAME_V2* pHeader = reinterpret_cast<SCREEN_FRAME_V2*>(packet.get());
    ZeroMemory(pHeader, sizeof(SCREEN_FRAME_V2));
    wcscpy_s(pHeader->mark, L"SCREEN2_RET_FRAME_V2");
    pHeader->version = SCREEN2_PROTOCOL_VERSION_V2;
    pHeader->headerSize = static_cast<unsigned int>(sizeof(SCREEN_FRAME_V2));
    pHeader->frameType = SCREEN2_FRAME_TYPE_FULL;
    pHeader->sequence = sequence;
    pHeader->baseSequence = 0;
    pHeader->originalSize = static_cast<int>(jpegSize);
    pHeader->payloadSize = static_cast<int>(compressedSize);
    pHeader->rectCount = 0;
    pHeader->compression = SCREEN2_COMPRESSION_ZLIB;
    pHeader->flags = SCREEN2_FRAME_FLAG_KEYFRAME;

    // 复制压缩数据
    std::memcpy(packet.get() + sizeof(SCREEN_FRAME_V2), compressed.get(), compressedSize);

    // 发送
    auto sendStart = std::chrono::steady_clock::now();
    bool result = SendPacket(packet.get(), totalSize);
    auto sendElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - sendStart);
    unsigned int sendMs = sendElapsed.count() > 0 ? static_cast<unsigned int>(sendElapsed.count()) : 0;
    RecordFrameSendStats(true, jpegSize, compressedSize, totalSize, 0, result, sendMs);
    MaybeReportSendStats(false);

#if _DEBUG_PRINTF_OPEN_
    if (result)
    {
        std::cout << "[ScreenManager] First frame sent: " << totalSize << " bytes" << std::endl;
    }
    else
    {
        std::cout << "[ScreenManager] ERROR: Failed to send first frame" << std::endl;
    }
#endif

    if (result)
    {
        if (CommitSentFrameBaseline(true))
        {
            CommitFrameSequence(sequence);
        }
        else
        {
            AdvanceFrameSequence(sequence);
        }
        MarkFrameInFlight(totalSize, true, 0);
    }
    else
    {
        SendOperationMessage(L"[SCREEN2][ERROR] SendFirstFrame: SendPacket failed");
    }
    return result;
}

// ============================================================
// 发送后续帧
// ============================================================
bool ScreenManager::SendNextFrame()
{
    if (!m_capture)
    {
        return false;
    }

    if (!CanSendFrame())
    {
        return true;
    }

    bool forceKeyframe = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        forceKeyframe = m_forceKeyframe;
        m_forceKeyframe = false;
    }
    if (forceKeyframe)
    {
        SendOperationMessage(L"[SCREEN2][WARN] sending forced keyframe to restore baseline");
        bool sent = SendFirstFrame();
        if (!sent)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_forceKeyframe = true;
        }
        return sent;
    }

    // 捕获后续帧
    DWORD diffDataSize = 0;
    int rectCount = 0;
    bool saturated = false;
    auto diffData = m_capture->CaptureNextFrame(diffDataSize, rectCount, saturated);

    if (saturated)
    {
        SendOperationMessage(L"[SCREEN2][WARN] diff frame saturated or too large before compression, sending full frame");
        if (!PrepareSaturatedKeyframe())
        {
            return false;
        }
        return SendFirstFrame();
    }

    if (!diffData || diffDataSize == 0 || rectCount == 0)
    {
        // 没有变化，跳过
        return true;
    }

    if (diffDataSize > MAX_DIFF_FRAME_SIZE)
    {
        SendOperationMessage(L"[SCREEN2][WARN] diff frame saturated or too large before compression, sending full frame");
        if (!PrepareSaturatedKeyframe())
        {
            return false;
        }
        return SendFirstFrame();
    }

    ResetSaturatedDiffStreak();

    // zlib 压缩
    DWORD compressedSize = 0;
    auto compressed = CompressData(diffData.get(), diffDataSize, compressedSize);

    if (!compressed || compressedSize == 0)
    {
#if _DEBUG_PRINTF_OPEN_
        std::cout << "[ScreenManager] ERROR: Failed to compress next frame" << std::endl;
#endif
        return false;
    }

    if (compressedSize > MAX_COMPRESSED_DIFF_SIZE)
    {
        SendOperationMessage(L"[SCREEN2][WARN] compressed diff frame too large, sending full frame");
        if (!PrepareSaturatedKeyframe())
        {
            return false;
        }
        return SendFirstFrame();
    }

    // 构建完整数据包：SCREEN_FRAME_V2 + 压缩数据
    size_t totalSize = sizeof(SCREEN_FRAME_V2) + compressedSize;
    std::unique_ptr<BYTE[]> packet(new (std::nothrow) BYTE[totalSize]);
    if (!packet)
    {
        return false;
    }

    const unsigned long long sequence = ReserveFrameSequence();
    const unsigned long long baseSequence = m_lastCommittedSequence;

    // 填充 V2 统一帧头
    SCREEN_FRAME_V2* pHeader = reinterpret_cast<SCREEN_FRAME_V2*>(packet.get());
    ZeroMemory(pHeader, sizeof(SCREEN_FRAME_V2));
    wcscpy_s(pHeader->mark, L"SCREEN2_RET_FRAME_V2");
    pHeader->version = SCREEN2_PROTOCOL_VERSION_V2;
    pHeader->headerSize = static_cast<unsigned int>(sizeof(SCREEN_FRAME_V2));
    pHeader->frameType = SCREEN2_FRAME_TYPE_DIFF;
    pHeader->sequence = sequence;
    pHeader->baseSequence = baseSequence;
    pHeader->originalSize = static_cast<int>(diffDataSize);
    pHeader->payloadSize = static_cast<int>(compressedSize);
    pHeader->rectCount = rectCount;
    pHeader->compression = SCREEN2_COMPRESSION_ZLIB;
    pHeader->flags = 0;

    // 复制压缩数据
    std::memcpy(packet.get() + sizeof(SCREEN_FRAME_V2), compressed.get(), compressedSize);

    // 发送
    auto sendStart = std::chrono::steady_clock::now();
    bool result = SendPacket(packet.get(), totalSize);
    auto sendElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - sendStart);
    unsigned int sendMs = sendElapsed.count() > 0 ? static_cast<unsigned int>(sendElapsed.count()) : 0;
    RecordFrameSendStats(false, diffDataSize, compressedSize, totalSize, rectCount, result, sendMs);
    MaybeReportSendStats(false);
    if (result)
    {
        if (CommitSentFrameBaseline(false))
        {
            CommitFrameSequence(sequence);
        }
        else
        {
            AdvanceFrameSequence(sequence);
        }
        MarkFrameInFlight(totalSize, false, rectCount);
    }

#if _DEBUG_PRINTF_OPEN_
    if (result)
    {
        std::cout << "[ScreenManager] Next frame sent: " << rectCount << " rects, "
            << totalSize << " bytes" << std::endl;
    }
#endif

    return result;
}

// ============================================================
// 捕获线程函数
// ============================================================

DWORD WINAPI ScreenManager::CaptureThreadProc(LPVOID param)
{
    ScreenManager* manager = static_cast<ScreenManager*>(param);
    if (!manager)
    {
        return 1;
    }

    manager->CaptureThreadFunc();
    return 0;
}

void ScreenManager::CaptureThreadFunc()
{
#if _DEBUG_PRINTF_OPEN_
    std::cout << "[ScreenManager] Capture thread started" << std::endl;
#endif

    SendScreenSize();

    constexpr int FIRST_FRAME_MAX_ATTEMPTS = 5;
    bool firstFrameSent = false;
    for (int attempt = 1; attempt <= FIRST_FRAME_MAX_ATTEMPTS && !m_shouldStop.load(); ++attempt)
    {
        if (SendFirstFrame())
        {
            firstFrameSent = true;
            if (attempt > 1)
            {
                SendOperationMessage(L"[SCREEN2][OK] First frame sent after retry");
            }
            break;
        }

        if (attempt < FIRST_FRAME_MAX_ATTEMPTS)
        {
            SendOperationMessage(L"[SCREEN2][WARN] First frame failed, retrying");
            Sleep(100);
        }
    }

    if (!firstFrameSent)
    {
#if _DEBUG_PRINTF_OPEN_
        std::cout << "[ScreenManager] ERROR: Failed to send first frame, exiting thread" << std::endl;
#endif
        SendOperationMessage(L"[SCREEN2][ERROR] First frame failed after retries, capture thread exits");
        m_isCapturing.store(false);
        return;
    }

    while (!m_shouldStop.load())
    {
        auto frameStart = std::chrono::steady_clock::now();
        SendNextFrame();

        auto frameEnd = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(frameEnd - frameStart);

        std::chrono::milliseconds frameDuration = GetEffectiveFrameDuration();

        auto sleepTime = frameDuration - elapsed;
        if (sleepTime.count() > 0)
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait_for(lock, sleepTime, [this]() {
                return m_shouldStop.load();
                });
        }
    }

#if _DEBUG_PRINTF_OPEN_
    std::cout << "[ScreenManager] Capture thread exited" << std::endl;
#endif

    m_isCapturing.store(false);
}