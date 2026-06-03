// MainForm — a tiny native Windows Forms front-end around Pexip Pulse.
//
// The Pulse call lifecycle mirrors the C demo in ../../src/main.cpp:
//
//   1. pulse_new()                     -> create a Pulse instance (in the ctor)
//   2. pulse_options_set_*()           -> register a few callbacks
//   3. pulse_connect_with_rest_async() -> Connect button
//   4. pulse_disconnect_async()        -> Hang up button
//   5. pulse_free()                    -> on form close
//
// Pulse delivers its callbacks on its own threads, so anything that touches the
// UI is marshalled back onto the WinForms thread with BeginInvoke.

using System.Runtime.InteropServices;

using Pexip.Pulse.NativeDelegates;
using Pexip.Pulse.NativeEnums;
using Pexip.Pulse.NativeStructs;

using static Pexip.Pulse.NativeMethods.PulseConnect;
using static Pexip.Pulse.NativeMethods.PulseOptions;

namespace DopplerWin;

internal sealed class MainForm : Form
{
    // --- Pulse handle + rooted callbacks -----------------------------------

    private IntPtr _pulse;

    // The managed wrapper hands raw function pointers for these delegates to
    // native code, so we MUST keep them rooted for as long as Pulse is alive —
    // otherwise the GC could collect them and Pulse would call into freed
    // memory. Instance fields keep them alive alongside the form.
    private PulseVersionCallback? _versionCb;
    private PulseConferenceStatusInfoCallback? _statusCb;
    private PulseConferenceEventRemoteDisconnectCallback? _remoteDisconnectCb;
    private PulseOperationProgressCallback? _progressCb;
    private PulseAsyncOperationResultCallback? _connectResultCb;
    private PulseAsyncOperationResultCallback? _disconnectResultCb;

    private bool _inCall;

    // --- controls ----------------------------------------------------------

    private readonly TextBox _server = new() { Text = "" };
    private readonly TextBox _conference = new() { Text = "" };
    private readonly TextBox _displayName = new() { Text = "Doppler Windows demo" };
    private readonly TextBox _pin = new() { UseSystemPasswordChar = true };
    private readonly Button _connectButton = new() { Text = "Connect" };
    private readonly Label _statusLabel = new() { Text = "Idle" };
    private readonly TextBox _log = new()
    {
        Multiline = true,
        ReadOnly = true,
        ScrollBars = ScrollBars.Vertical,
        WordWrap = false,
    };

    public MainForm()
    {
        BuildLayout();

        // 1. Create the Pulse instance and 2. register callbacks up-front.
        _pulse = pulse_new();
        if (_pulse == IntPtr.Zero)
        {
            MessageBox.Show(this, "pulse_new() returned NULL — the native Pulse " +
                                  "library could not be initialised.",
                            "Pulse error", MessageBoxButtons.OK, MessageBoxIcon.Error);
            _connectButton.Enabled = false;
            return;
        }

        ConfigureCallbacks();
        pulse_options_set_application_user_agent_string(_pulse, "doppler-win/0.1");
        Log("Pulse initialised. Enter a server + conference and press Connect.");
    }

    // --- UI ----------------------------------------------------------------

    private void BuildLayout()
    {
        Text = "Doppler — Windows Pulse demo";
        Font = new Font("Segoe UI", 9f);
        ClientSize = new Size(560, 420);
        MinimumSize = new Size(420, 320);

        var grid = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            Padding = new Padding(10),
            ColumnCount = 2,
            RowCount = 6,
        };
        grid.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 110));
        grid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        for (int i = 0; i < 4; i++)
            grid.RowStyles.Add(new RowStyle(SizeType.Absolute, 32));
        grid.RowStyles.Add(new RowStyle(SizeType.Absolute, 40)); // button + status
        grid.RowStyles.Add(new RowStyle(SizeType.Percent, 100)); // log

        static Label Caption(string text) => new()
        {
            Text = text,
            AutoSize = false,
            Dock = DockStyle.Fill,
            TextAlign = ContentAlignment.MiddleLeft,
        };

        foreach (var box in new[] { _server, _conference, _displayName, _pin })
            box.Dock = DockStyle.Fill;

        grid.Controls.Add(Caption("Server"), 0, 0);
        grid.Controls.Add(_server, 1, 0);
        grid.Controls.Add(Caption("Conference"), 0, 1);
        grid.Controls.Add(_conference, 1, 1);
        grid.Controls.Add(Caption("Display name"), 0, 2);
        grid.Controls.Add(_displayName, 1, 2);
        grid.Controls.Add(Caption("PIN (optional)"), 0, 3);
        grid.Controls.Add(_pin, 1, 3);

        var actionRow = new FlowLayoutPanel
        {
            Dock = DockStyle.Fill,
            FlowDirection = FlowDirection.LeftToRight,
            WrapContents = false,
            Margin = new Padding(0),
        };
        _connectButton.AutoSize = true;
        _connectButton.Click += OnConnectOrHangup;
        _statusLabel.AutoSize = true;
        _statusLabel.Margin = new Padding(12, 8, 0, 0);
        actionRow.Controls.Add(_connectButton);
        actionRow.Controls.Add(_statusLabel);
        grid.Controls.Add(actionRow, 0, 4);
        grid.SetColumnSpan(actionRow, 2);

        _log.Dock = DockStyle.Fill;
        _log.Font = new Font("Consolas", 9f);
        grid.Controls.Add(_log, 0, 5);
        grid.SetColumnSpan(_log, 2);

        Controls.Add(grid);
        AcceptButton = _connectButton;
    }

    // --- actions -----------------------------------------------------------

    private void OnConnectOrHangup(object? sender, EventArgs e)
    {
        if (_pulse == IntPtr.Zero)
            return;

        if (!_inCall)
            StartCall();
        else
            HangUp();
    }

    private void StartCall()
    {
        string server = _server.Text.Trim();
        string conference = _conference.Text.Trim();
        if (server.Length == 0 || conference.Length == 0)
        {
            MessageBox.Show(this, "Please fill in both Server and Conference.",
                            "Missing details", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return;
        }

        var cfg = new PulseRestConnectionConfig
        {
            server_address = server,
            conference_name = conference,
            display_name = _displayName.Text.Trim(),
            pin_code = _pin.Text, // may be empty
        };

        _progressCb = OnProgress;
        _connectResultCb = OnConnectResult;
        var progressCfg = new PulseOperationProgressCallbackConfig
        {
            func = _progressCb,
            user_context = IntPtr.Zero,
        };
        var connectResultCfg = new PulseAsyncOperationResultCallbackConfig
        {
            func = _connectResultCb,
            user_context = IntPtr.Zero,
        };

        Log($"Connecting to {server} / {conference} as \"{cfg.display_name}\" ...");
        // 3. Kick off the async connect.
        PulseErrorType err = pulse_connect_with_rest_async(_pulse, cfg, connectResultCfg, progressCfg);
        if (err != PulseErrorType.PULSE_SUCCESS)
        {
            Log($"pulse_connect_with_rest_async failed: {err}");
            MessageBox.Show(this, $"Connect failed: {err}", "Pulse error",
                            MessageBoxButtons.OK, MessageBoxIcon.Error);
            return;
        }

        _inCall = true;
        SetBusyUi("Connecting...");
    }

    private void HangUp()
    {
        _disconnectResultCb = OnDisconnectResult;
        _progressCb ??= OnProgress;
        var disconnectResultCfg = new PulseAsyncOperationResultCallbackConfig
        {
            func = _disconnectResultCb,
            user_context = IntPtr.Zero,
        };
        var progressCfg = new PulseOperationProgressCallbackConfig
        {
            func = _progressCb,
            user_context = IntPtr.Zero,
        };

        Log("Disconnecting ...");
        _connectButton.Enabled = false;
        _statusLabel.Text = "Disconnecting...";
        // 4. Async disconnect; the status callback resets the UI when done.
        PulseErrorType err = pulse_disconnect_async(_pulse, disconnectResultCfg, progressCfg);
        if (err != PulseErrorType.PULSE_SUCCESS)
        {
            Log($"pulse_disconnect_async failed: {err}");
            _connectButton.Enabled = true;
        }
    }

    private void SetBusyUi(string status)
    {
        _connectButton.Text = "Hang up";
        _statusLabel.Text = status;
        SetInputsEnabled(false);
    }

    private void SetIdleUi()
    {
        _inCall = false;
        _connectButton.Text = "Connect";
        _connectButton.Enabled = true;
        _statusLabel.Text = "Idle";
        SetInputsEnabled(true);
    }

    private void SetInputsEnabled(bool enabled)
    {
        _server.Enabled = enabled;
        _conference.Enabled = enabled;
        _displayName.Enabled = enabled;
        _pin.Enabled = enabled;
    }

    // --- Pulse callbacks (called on native threads) ------------------------

    private void ConfigureCallbacks()
    {
        _versionCb = OnVersion;
        pulse_options_set_version_callback(_pulse, new PulseVersionCallbackConfig
        {
            func = _versionCb,
            user_context = IntPtr.Zero,
        });

        _statusCb = OnStatus;
        pulse_options_set_conference_state_callback(_pulse, new PulseConferenceStatusCallbackConfig
        {
            func = _statusCb,
            user_context = IntPtr.Zero,
        });

        _remoteDisconnectCb = OnRemoteDisconnect;
        pulse_options_set_conference_event_remote_disconnect_callback(_pulse, _remoteDisconnectCb, IntPtr.Zero);
    }

    private bool OnVersion(ulong serverMajor, ulong serverMinor, IntPtr ctx)
    {
        Log($"Infinity server reports v{serverMajor}.{serverMinor}");
        return true; // proceed regardless of version.
    }

    private void OnStatus(PulseConferenceStatusInfo info, IntPtr ctx)
    {
        Log($"status: {info.status} (service={info.current_service_type}, blocked={info.is_blocked})");

        BeginInvoke(() =>
        {
            switch (info.status)
            {
                case PulseConnectionStatus.PULSE_CONNECTION_STATUS_CONNECTING:
                    SetBusyUi("Connecting...");
                    break;
                case PulseConnectionStatus.PULSE_CONNECTION_STATUS_RECONNECTING:
                    SetBusyUi("Reconnecting...");
                    break;
                case PulseConnectionStatus.PULSE_CONNECTION_STATUS_CONNECTED:
                    SetBusyUi("Connected");
                    break;
                case PulseConnectionStatus.PULSE_CONNECTION_STATUS_DISCONNECTING:
                    _statusLabel.Text = "Disconnecting...";
                    break;
                case PulseConnectionStatus.PULSE_CONNECTION_STATUS_DISCONNECTED:
                    SetIdleUi();
                    break;
            }
        });
    }

    private void OnRemoteDisconnect(string reason, IntPtr ctx)
    {
        Log($"disconnected by server: {reason}");
    }

    private void OnProgress(IntPtr progressInfo, IntPtr ctx)
    {
        if (progressInfo == IntPtr.Zero)
            return;

        var info = Marshal.PtrToStructure<PulseOperationProgressInfo>(progressInfo);
        Log($"progress: {info.progress,5:P0}  {info.desc}");
    }

    private void OnConnectResult(PulseErrorType err, IntPtr ctx)
    {
        if (err == PulseErrorType.PULSE_SUCCESS)
        {
            Log("connect: success");
        }
        else
        {
            Log($"connect: failed ({err})");
            BeginInvoke(SetIdleUi);
        }
    }

    private void OnDisconnectResult(PulseErrorType err, IntPtr ctx)
    {
        Log(err == PulseErrorType.PULSE_SUCCESS
            ? "disconnect: success"
            : $"disconnect: failed ({err})");
        BeginInvoke(SetIdleUi);
    }

    // --- helpers / teardown ------------------------------------------------

    private void Log(string message)
    {
        if (InvokeRequired)
        {
            BeginInvoke(() => Log(message));
            return;
        }

        _log.AppendText($"{DateTime.Now:HH:mm:ss}  {message}{Environment.NewLine}");
    }

    protected override void OnFormClosing(FormClosingEventArgs e)
    {
        // 5. Tear the call down (synchronously) and release the handle.
        if (_pulse != IntPtr.Zero)
        {
            if (_inCall)
            {
                _progressCb ??= OnProgress;
                var progressCfg = new PulseOperationProgressCallbackConfig
                {
                    func = _progressCb,
                    user_context = IntPtr.Zero,
                };
                pulse_disconnect(_pulse, progressCfg);
            }

            pulse_free(_pulse);
            _pulse = IntPtr.Zero;
        }

        base.OnFormClosing(e);
    }
}
