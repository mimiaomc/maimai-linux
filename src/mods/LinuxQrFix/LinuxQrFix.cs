using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using System.Reflection.Emit;
using System.Threading;
using HarmonyLib;
using MelonLoader;
using UnityEngine;

[assembly: MelonInfo(typeof(LinuxQrFix.LinuxQrFixMod), "LinuxQrFix", "1.7.0", "maimaiLinuxProject")]
[assembly: MelonGame("sega-interactive", "Sinmai")]

namespace LinuxQrFix
{
    public class LinuxQrFixMod : MelonMod
    {
        private static readonly FieldInfo TextureField =
            AccessTools.Field(typeof(Manager.ChimeDevice), "_texture");

        private static readonly FieldInfo CounterField =
            AccessTools.Field(typeof(Manager.ChimeDevice), "_counter");

        private static readonly FieldInfo StateMonitorsField =
            AccessTools.Field(AccessTools.TypeByName("Process.Entry.State.StateEntry"), "Monitors");

        private static readonly PropertyInfo MonitorIsDecideProp =
            AccessTools.Property(AccessTools.TypeByName("Monitor.Entry.EntryMonitor"), "IsDecide");

        private const float SameQrCooldownSec = 2.5f;
        private const int ClearFramesRequired = 8;

        private static readonly HashSet<string> _sessionLockedQrs = new HashSet<string>();
        private static string _pendingQr;
        private static string _blockedQr;
        private static float _blockedTime;
        private static int _decidedCountBeforePlay;
        private static int _clearFrames;
        private static bool _loggedBlock;

        private static readonly FieldInfo SerialPortNameField =
            AccessTools.Field(typeof(System.IO.Ports.SerialPort), "port_name");

        private const string HidrawKeepalivePath = "/dev/hidraw_maimai";
        private static bool _loggedSetSignalIgnored;
        private static readonly bool[] _loggedFirstHidButton = new bool[2];
        private static byte[,] _adxInputBuf;
        private static byte[,] _adxInputBufPending;

        private static readonly Dictionary<string, string> ConfiguredComPorts =
            new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);

        private static readonly string[] ConfiguredHidPaths = new string[2];
        private static readonly bool[] HidExplicitlyDisabled = new bool[2];
        private static string _configFilePath;

        private static readonly Dictionary<string, string> DefaultComPorts =
            new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
            {
                { "COM3", "/dev/ttyUSB_at32" },
                { "COM21", "/dev/ttyUSB_ch340" },
            };

        private static readonly Dictionary<string, KeyValuePair<string, string>> DataCaseFixes =
            new Dictionary<string, KeyValuePair<string, string>>
            {
                { "LoadPartner", new KeyValuePair<string, string>("partner.xml", "Partner.xml") },
                { "LoadPartnerCloseness", new KeyValuePair<string, string>("partnerCloseness.xml", "PartnerCloseness.xml") },
                { "LoadMapChallenges", new KeyValuePair<string, string>("Challenge", "challenge") },
                { "LoadPhotoFrame", new KeyValuePair<string, string>("photoframe", "photoFrame") },
            };

        public override void OnInitializeMelon()
        {
            LoadIoConfig();

            var transpiler = new HarmonyMethod(typeof(LinuxQrFixMod), nameof(DataCaseTranspiler));
            int patched = 0;
            foreach (var kv in DataCaseFixes)
            {
                var method = AccessTools.Method(typeof(Manager.DataManager), kv.Key);
                if (method == null)
                {
                    LoggerInstance.Warning($"DataManager.{kv.Key} not found");
                    continue;
                }
                HarmonyInstance.Patch(method, transpiler: transpiler);
                patched++;
            }

            int qrHooks = PatchQrHooks();
            int serialHooks = PatchSerialHooks();
            int activeHidCount = InitAdxHidBridge();
            if (activeHidCount == 0 && !HidExplicitlyDisabled[0])
            {
                // Keep 2e3c:5750 alive even if AdxHidInput mod is unavailable or disabled.
                StartHidrawThread(0);
            }
            LoggerInstance.Msg($"LinuxQrFix v1.7.0 initialized: {qrHooks} QR hooks, {serialHooks} serial hooks, {patched} DataManager Linux case fixes, AdxHidPlayers={activeHidCount}, Config={_configFilePath}");
        }

        private int PatchSerialHooks()
        {
            int count = 0;
            count += TryPatch(typeof(System.IO.Ports.SerialPort), "GetPortNames",
                postfix: nameof(GetPortNamesPostfix));
            count += TryPatch(typeof(System.IO.Ports.SerialPort), "Open",
                prefix: nameof(SerialPortOpenPrefix));
            count += TryPatch(AccessTools.TypeByName("System.IO.Ports.SerialPortStream"), "SetSignal",
                finalizer: nameof(SetSignalFinalizer));
            return count;
        }

        private int PatchQrHooks()
        {
            int count = 0;
            count += TryPatch(typeof(Manager.ChimeDevice), "GetDecodeStrings",
                prefix: nameof(DecodePrefix), postfix: nameof(DecodePostfix));
            count += TryPatch(typeof(Manager.ChimeDevice), "BeginScan",
                postfix: nameof(BeginScanPostfix));
            count += TryPatch(AccessTools.TypeByName("Process.EntryProcess"), "OnStart",
                postfix: nameof(ResetEntrySession));
            count += TryPatch(AccessTools.TypeByName("Process.EntryProcess"), "OnRelease",
                postfix: nameof(ResetEntrySession));
            count += TryPatch(AccessTools.TypeByName("Process.Entry.State.ConfirmPlay"), "Init",
                postfix: nameof(ConfirmPlayInitPostfix));
            count += TryPatch(AccessTools.TypeByName("Process.Entry.State.ConfirmEntry"), "Init",
                postfix: nameof(ConfirmEntryInitPostfix));
            return count;
        }

        private int TryPatch(Type targetType, string methodName, string prefix = null, string postfix = null, string finalizer = null)
        {
            var method = targetType != null ? AccessTools.Method(targetType, methodName) : null;
            if (method == null)
            {
                LoggerInstance.Warning($"[LinuxQrFix] Hook target not found: {targetType?.FullName ?? "<null>"}.{methodName}");
                return 0;
            }

            var pre = prefix != null ? new HarmonyMethod(typeof(LinuxQrFixMod), prefix) : null;
            var post = postfix != null ? new HarmonyMethod(typeof(LinuxQrFixMod), postfix) : null;
            var fin = finalizer != null ? new HarmonyMethod(typeof(LinuxQrFixMod), finalizer) : null;
            HarmonyInstance.Patch(method, prefix: pre, postfix: post, finalizer: fin);
            LoggerInstance.Msg($"[LinuxQrFix] Hooked {targetType.FullName}.{methodName}");
            return 1;
        }

        private static IEnumerable<CodeInstruction> DataCaseTranspiler(
            IEnumerable<CodeInstruction> instructions,
            MethodBase original)
        {
            KeyValuePair<string, string> rule;
            bool hasRule = DataCaseFixes.TryGetValue(original.Name, out rule);
            int count = 0;

            foreach (var instr in instructions)
            {
                if (hasRule && instr.opcode == OpCodes.Ldstr && (instr.operand as string) == rule.Key)
                {
                    instr.operand = rule.Value;
                    count++;
                }
                yield return instr;
            }

            MelonLogger.Msg($"[LinuxQrFix] DataManager.{original.Name}: \"{rule.Key}\" -> \"{rule.Value}\" ({count}x)");
        }

        /// <summary>
        /// Skips decode before a fresh WebCamTexture frame arrives and records whether
        /// ChimeDevice is past its 4-frame warmup (_counter &gt; 3).
        /// </summary>
        private static bool DecodePrefix(Manager.ChimeDevice __instance, ref string[] __result, out bool __state)
        {
            __state = false;
            if (TextureField == null)
            {
                __state = true;
                return true;
            }

            var tex = TextureField.GetValue(__instance) as WebCamTexture;
            if (tex == null || !tex.isPlaying || !tex.didUpdateThisFrame)
            {
                __result = null;
                return false;
            }

            int counter = CounterField != null ? (int)CounterField.GetValue(__instance) : 4;
            __state = counter > 3;
            return true;
        }

        /// <summary>
        /// Suppresses already-logged-in session QRs and same-QR rapid rescans across scan restarts.
        /// Warmup frames (__state == false) never count toward clearing _blockedQr.
        /// </summary>
        private static void DecodePostfix(ref string[] __result, bool __state)
        {
            if (!__state)
                return;

            float now = Time.realtimeSinceStartup;
            if (__result == null || __result.Length == 0)
            {
                HandleClearDecodedFrame(now);
                return;
            }

            bool lockedInSession;
            string blockedMatch = FindBlockedQr(__result, out lockedInSession);
            if (blockedMatch != null)
            {
                _clearFrames = 0;
                _blockedQr = blockedMatch;
                _blockedTime = now;
                if (!_loggedBlock)
                {
                    _loggedBlock = true;
                    string reason = lockedInSession ? "already active in current entry session" : "within cooldown";
                    MelonLogger.Msg($"[LinuxQrFix] Suppressed duplicate QR scan ({reason}, len={blockedMatch.Length})");
                }
                __result = null;
                return;
            }

            string accepted = FirstNonEmpty(__result);
            if (accepted == null)
                return;

            _pendingQr = accepted;
            _blockedQr = accepted;
            _blockedTime = now;
            _clearFrames = 0;
            _loggedBlock = false;
            MelonLogger.Msg($"[LinuxQrFix] Accepted QR scan (len={accepted.Length})");
        }

        private static void HandleClearDecodedFrame(float now)
        {
            if (_blockedQr == null)
                return;

            _clearFrames++;
            if (_clearFrames >= ClearFramesRequired && (now - _blockedTime) >= SameQrCooldownSec)
            {
                _blockedQr = null;
                _clearFrames = 0;
                _loggedBlock = false;
            }
        }

        private static string FindBlockedQr(string[] results, out bool lockedInSession)
        {
            lockedInSession = false;
            for (int i = 0; i < results.Length; i++)
            {
                string val = results[i];
                if (string.IsNullOrEmpty(val))
                    continue;
                if (_sessionLockedQrs.Contains(val))
                {
                    lockedInSession = true;
                    return val;
                }
                if (_blockedQr != null && val == _blockedQr)
                    return val;
            }
            return null;
        }

        private static string FirstNonEmpty(string[] results)
        {
            for (int i = 0; i < results.Length; i++)
            {
                if (!string.IsNullOrEmpty(results[i]))
                    return results[i];
            }
            return null;
        }

        private static void BeginScanPostfix()
        {
            _clearFrames = 0;
            _loggedBlock = false;
        }

        private static void ResetEntrySession()
        {
            _sessionLockedQrs.Clear();
            _pendingQr = null;
            _blockedQr = null;
            _decidedCountBeforePlay = 0;
            _clearFrames = 0;
            _loggedBlock = false;
        }

        private static void ConfirmPlayInitPostfix(object __instance)
        {
            _decidedCountBeforePlay = CountDecidedMonitors(__instance);
            if (!string.IsNullOrEmpty(_pendingQr))
                _sessionLockedQrs.Add(_pendingQr);
        }

        private static void ConfirmEntryInitPostfix(object __instance)
        {
            if (CountDecidedMonitors(__instance) <= _decidedCountBeforePlay && !string.IsNullOrEmpty(_pendingQr))
                _sessionLockedQrs.Remove(_pendingQr);
            _pendingQr = null;
        }

        private static int CountDecidedMonitors(object stateEntry)
        {
            if (stateEntry == null || StateMonitorsField == null || MonitorIsDecideProp == null)
                return 0;

            var monitors = StateMonitorsField.GetValue(stateEntry) as System.Collections.IEnumerable;
            if (monitors == null)
                return 0;

            int count = 0;
            foreach (object mon in monitors)
            {
                if (mon != null && MonitorIsDecideProp.GetValue(mon, null) is bool decided && decided)
                    count++;
            }
            return count;
        }

        /// <summary>
        /// Loads or initializes LinuxQrFix.ini in the game root directory for 1P/2P Touch, LED serial and HID device paths.
        /// </summary>
        private static void LoadIoConfig()
        {
            ConfiguredComPorts.Clear();
            ConfiguredHidPaths[0] = null;
            ConfiguredHidPaths[1] = null;
            HidExplicitlyDisabled[0] = false;
            HidExplicitlyDisabled[1] = false;

            try
            {
                string overridePath = Environment.GetEnvironmentVariable("MAIMAI_LINUX_CONFIG");
                string gameRootDir = !string.IsNullOrEmpty(MelonLoader.Utils.MelonEnvironment.GameRootDirectory)
                    ? MelonLoader.Utils.MelonEnvironment.GameRootDirectory
                    : AppDomain.CurrentDomain.BaseDirectory;

                _configFilePath = !string.IsNullOrEmpty(overridePath)
                    ? overridePath
                    : Path.Combine(gameRootDir, "LinuxQrFix.ini");

                if (!File.Exists(_configFilePath))
                {
                    string dir = Path.GetDirectoryName(_configFilePath);
                    if (!string.IsNullOrEmpty(dir))
                        Directory.CreateDirectory(dir);
                    File.WriteAllText(_configFilePath, BuildDefaultIniText());
                    MelonLogger.Msg($"[LinuxQrFix] Created default IO config: {_configFilePath}");
                }

                string section = string.Empty;
                foreach (string rawLine in File.ReadAllLines(_configFilePath))
                {
                    string line = rawLine.Trim();
                    if (line.Length == 0 || line.StartsWith("#") || line.StartsWith(";"))
                        continue;

                    if (line.StartsWith("[") && line.EndsWith("]"))
                    {
                        section = line.Substring(1, line.Length - 2).Trim();
                        continue;
                    }

                    int eq = line.IndexOf('=');
                    if (eq <= 0)
                        continue;

                    string key = line.Substring(0, eq).Trim();
                    string val = UnquoteConfigValue(line.Substring(eq + 1));

                    if (section.Equals("Serial", StringComparison.OrdinalIgnoreCase) || string.IsNullOrEmpty(section))
                        ApplySerialConfigEntry(key, val);
                    if (section.Equals("Hid", StringComparison.OrdinalIgnoreCase) || string.IsNullOrEmpty(section))
                        ApplyHidConfigEntry(key, val);
                }
            }
            catch (Exception ex)
            {
                MelonLogger.Warning($"[LinuxQrFix] LoadIoConfig failed: {ex.Message}");
            }
        }

        private static string UnquoteConfigValue(string raw)
        {
            string val = raw.Trim();
            foreach (string marker in new[] { " #", "\t#", " ;", "\t;" })
            {
                int commentIdx = val.IndexOf(marker, StringComparison.Ordinal);
                if (commentIdx >= 0)
                    val = val.Substring(0, commentIdx).Trim();
            }

            if (val.Length >= 2 && ((val[0] == '"' && val[val.Length - 1] == '"') || (val[0] == '\'' && val[val.Length - 1] == '\'')))
                val = val.Substring(1, val.Length - 2).Trim();

            return val;
        }

        private static void ApplySerialConfigEntry(string key, string val)
        {
            string comKey;
            if (key.Equals("Touch1P", StringComparison.OrdinalIgnoreCase) || key.Equals("COM3", StringComparison.OrdinalIgnoreCase))
                comKey = "COM3";
            else if (key.Equals("Touch2P", StringComparison.OrdinalIgnoreCase) || key.Equals("COM4", StringComparison.OrdinalIgnoreCase))
                comKey = "COM4";
            else if (key.Equals("Led1P", StringComparison.OrdinalIgnoreCase) || key.Equals("COM21", StringComparison.OrdinalIgnoreCase))
                comKey = "COM21";
            else if (key.Equals("Led2P", StringComparison.OrdinalIgnoreCase) || key.Equals("COM23", StringComparison.OrdinalIgnoreCase))
                comKey = "COM23";
            else if (key.StartsWith("COM", StringComparison.OrdinalIgnoreCase))
                comKey = key.ToUpperInvariant();
            else
                return;

            ConfiguredComPorts[comKey] = IsDisabledKeyword(val) ? string.Empty : val;
        }

        private static void ApplyHidConfigEntry(string key, string val)
        {
            int idx;
            if (key.Equals("Hid1P", StringComparison.OrdinalIgnoreCase))
                idx = 0;
            else if (key.Equals("Hid2P", StringComparison.OrdinalIgnoreCase))
                idx = 1;
            else
                return;

            if (IsDisabledKeyword(val))
            {
                HidExplicitlyDisabled[idx] = true;
                ConfiguredHidPaths[idx] = null;
            }
            else if (string.IsNullOrEmpty(val) || val.Equals("auto", StringComparison.OrdinalIgnoreCase))
            {
                HidExplicitlyDisabled[idx] = false;
                ConfiguredHidPaths[idx] = null;
            }
            else
            {
                HidExplicitlyDisabled[idx] = false;
                ConfiguredHidPaths[idx] = val;
            }
        }

        private static bool IsDisabledKeyword(string val)
        {
            return val.Equals("none", StringComparison.OrdinalIgnoreCase) ||
                   val.Equals("off", StringComparison.OrdinalIgnoreCase) ||
                   val.Equals("disabled", StringComparison.OrdinalIgnoreCase);
        }

        private static string BuildDefaultIniText()
        {
            return string.Join("\n", new[]
            {
                "# LinuxQrFix Linux 硬件端口配置（修改后重启游戏生效）",
                "# 串口留空表示不映射该 COM 口；Hid1P/Hid2P 留空或写 auto 表示自动探测，写 none 表示禁用。",
                "",
                "[Serial]",
                "# 1P / 2P 触摸屏串口设备（对应游戏内 COM3 / COM4）",
                "Touch1P=/dev/ttyUSB_at32",
                "Touch2P=",
                "",
                "# 1P / 2P 灯板串口设备（对应游戏内 COM21 / COM23）",
                "Led1P=/dev/ttyUSB_ch340",
                "Led2P=",
                "",
                "[Hid]",
                "# 1P / 2P 手台按键 HID 设备",
                "Hid1P=/dev/hidraw_maimai",
                "Hid2P=",
                ""
            });
        }

        /// <summary>
        /// Maps Windows COM port names (e.g. COM3/COM4/COM21/COM23) to Linux tty devices via
        /// MAIMAI_COMx env vars, LinuxQrFix.ini, or default udev symlinks if present.
        /// </summary>
        private static string ResolveComPort(string portName)
        {
            if (string.IsNullOrEmpty(portName))
                return null;

            string key = portName.Trim().ToUpperInvariant();
            string envPath = Environment.GetEnvironmentVariable("MAIMAI_" + key);
            if (!string.IsNullOrEmpty(envPath) && File.Exists(envPath))
                return envPath;

            string configured;
            if (ConfiguredComPorts.TryGetValue(key, out configured))
                return !string.IsNullOrEmpty(configured) && File.Exists(configured) ? configured : null;

            string mapped;
            if (DefaultComPorts.TryGetValue(key, out mapped) && File.Exists(mapped))
                return mapped;

            return null;
        }

        private static void GetPortNamesPostfix(ref string[] __result)
        {
            var list = __result != null ? new List<string>(__result) : new List<string>();
            var candidates = new HashSet<string>(new[] { "COM3", "COM4", "COM21", "COM23" }, StringComparer.OrdinalIgnoreCase);
            foreach (string com in ConfiguredComPorts.Keys)
                candidates.Add(com);

            foreach (string com in candidates)
            {
                if (ResolveComPort(com) != null && !list.Contains(com))
                    list.Add(com);
            }

            AddDirectAquaMaiDevPaths(list);
            __result = list.ToArray();
        }

        private static void AddDirectAquaMaiDevPaths(List<string> list)
        {
            var typeFields = new[]
            {
                new KeyValuePair<string, string>("AquaMai.Mods.GameSystem.TouchPanelPort", "portName_1P"),
                new KeyValuePair<string, string>("AquaMai.Mods.GameSystem.TouchPanelPort", "portName_2P"),
                new KeyValuePair<string, string>("AquaMai.Mods.GameSystem.LightBoardPort", "portName_1PLED"),
                new KeyValuePair<string, string>("AquaMai.Mods.GameSystem.LightBoardPort", "portName_2PLED"),
            };

            foreach (var tf in typeFields)
            {
                var type = AccessTools.TypeByName(tf.Key);
                var field = type != null ? AccessTools.Field(type, tf.Value) : null;
                var val = field != null ? field.GetValue(null) as string : null;
                if (!string.IsNullOrEmpty(val) && val.StartsWith("/dev/", StringComparison.Ordinal) && File.Exists(val) && !list.Contains(val))
                    list.Add(val);
            }
        }

        private static void SerialPortOpenPrefix(object __instance)
        {
            if (__instance == null || SerialPortNameField == null)
                return;

            var current = SerialPortNameField.GetValue(__instance) as string;
            string mapped = ResolveComPort(current);
            if (mapped != null && mapped != current)
            {
                SerialPortNameField.SetValue(__instance, mapped);
                MelonLogger.Msg($"[LinuxQrFix] SerialPort.Open: {current} -> {mapped}");
            }
        }

        /// <summary>
        /// usbserial_generic returns ENOTTY for TIOCMBIS/TIOCMBIC; ignore IOException from SetSignal.
        /// </summary>
        private static Exception SetSignalFinalizer(Exception __exception)
        {
            if (__exception is IOException)
            {
                if (!_loggedSetSignalIgnored)
                {
                    _loggedSetSignalIgnored = true;
                    MelonLogger.Msg("[LinuxQrFix] Ignored SerialPortStream.SetSignal IOException (usbserial_generic/pty)");
                }
                return null;
            }
            return __exception;
        }

        /// <summary>
        /// Replaces Windows hid.dll/HidLibrary inside AquaMai's AdxHidInput with Linux /dev/hidraw input
        /// while keeping 2e3c:5750 (AT32 composite device) open so it doesn't reset every ~10s.
        /// </summary>
        private static int InitAdxHidBridge()
        {
            try
            {
                var adxType = AccessTools.TypeByName("AquaMai.Mods.GameSystem.AdxHidInput");
                if (adxType == null)
                    return 0;

                var disableButtonsField = AccessTools.Field(adxType, "disableButtons");
                if (disableButtonsField != null && disableButtonsField.GetValue(null) is bool disabled && disabled)
                    return 0;

                var inputBufField = AccessTools.Field(adxType, "inputBuf");
                var inputPendingField = AccessTools.Field(adxType, "inputBufPending");
                var keyEnabledField = AccessTools.Field(adxType, "keyEnabled");
                var hidRunningField = AccessTools.Field(adxType, "hidThreadRunning");
                var onAfterPatch = AccessTools.Method(adxType, "OnAfterPatch");

                if (inputBufField == null || inputPendingField == null || keyEnabledField == null || onAfterPatch == null)
                    return 0;

                if (keyEnabledField.GetValue(null) is bool alreadyEnabled && alreadyEnabled)
                    return 0;

                var hotPlugField = AccessTools.Field(adxType, "hotPlugSupport");
                bool hotPlug = hotPlugField != null && hotPlugField.GetValue(null) is bool hp && hp;

                bool enable1P = !HidExplicitlyDisabled[0] &&
                    (!string.IsNullOrEmpty(ConfiguredHidPaths[0]) || FindHidrawPath(0) != null || hotPlug);
                bool enable2P = !HidExplicitlyDisabled[1] &&
                    (!string.IsNullOrEmpty(ConfiguredHidPaths[1]) || FindHidrawPath(1) != null);

                if (!enable1P && !enable2P)
                    return 0;

                _adxInputBuf = inputBufField.GetValue(null) as byte[,];
                _adxInputBufPending = inputPendingField.GetValue(null) as byte[,];
                if (_adxInputBuf == null || _adxInputBufPending == null)
                    return 0;

                keyEnabledField.SetValue(null, true);
                var running = hidRunningField?.GetValue(null) as bool[];
                int started = 0;

                if (enable1P)
                {
                    if (running != null && running.Length > 0)
                        running[0] = true;
                    StartHidrawThread(0);
                    started++;
                }

                if (enable2P)
                {
                    if (running != null && running.Length > 1)
                        running[1] = true;
                    StartHidrawThread(1);
                    started++;
                }

                onAfterPatch.Invoke(null, null);
                MelonLogger.Msg($"[LinuxQrFix] Enabled AdxHidInput Linux hidraw bridge (1P={enable1P}, 2P={enable2P})");
                return started;
            }
            catch (Exception ex)
            {
                MelonLogger.Warning($"[LinuxQrFix] InitAdxHidBridge failed: {ex}");
                return 0;
            }
        }

        private static string FindHidrawPath(int playerIndex)
        {
            string envKey = playerIndex == 0 ? "MAIMAI_HID1P" : "MAIMAI_HID2P";
            string envPath = Environment.GetEnvironmentVariable(envKey);
            if (!string.IsNullOrEmpty(envPath))
                return File.Exists(envPath) ? envPath : null;

            string configured = ConfiguredHidPaths[playerIndex];
            if (!string.IsNullOrEmpty(configured))
                return File.Exists(configured) ? configured : null;

            if (playerIndex == 0 && File.Exists(HidrawKeepalivePath))
                return HidrawKeepalivePath;

            try
            {
                const string sysHidraw = "/sys/class/hidraw";
                if (!Directory.Exists(sysHidraw))
                    return null;

                string[] targets = playerIndex == 0
                    ? new[] { "2E3C:00005750", "2E3C:00005767" }
                    : new[] { "2E4C:00005750", "2E3C:00005768" };

                foreach (string dir in Directory.GetDirectories(sysHidraw))
                {
                    string ueventPath = Path.Combine(dir, "device/uevent");
                    if (!File.Exists(ueventPath))
                        continue;

                    string uevent = File.ReadAllText(ueventPath);
                    bool matched = false;
                    foreach (string vidPid in targets)
                    {
                        if (uevent.IndexOf(vidPid, StringComparison.OrdinalIgnoreCase) >= 0)
                        {
                            matched = true;
                            break;
                        }
                    }

                    if (!matched)
                        continue;

                    string devPath = "/dev/" + Path.GetFileName(dir);
                    if (File.Exists(devPath))
                        return devPath;
                }
            }
            catch
            {
            }

            return null;
        }

        private static void StartHidrawThread(int playerIndex)
        {
            var thread = new Thread(() => HidrawLoop(playerIndex))
            {
                IsBackground = true,
                Name = $"LinuxQrFix-Hidraw-{playerIndex + 1}P"
            };
            thread.Start();
        }

        private static void HidrawLoop(int playerIndex)
        {
            byte[] buf = new byte[64];
            while (true)
            {
                try
                {
                    string path = FindHidrawPath(playerIndex);
                    if (path == null)
                    {
                        Thread.Sleep(500);
                        continue;
                    }

                    using (var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite, 64, false))
                    {
                        MelonLogger.Msg($"[LinuxQrFix] Opened {playerIndex + 1}P hidraw: {path}");
                        int n;
                        while ((n = fs.Read(buf, 0, buf.Length)) > 0)
                        {
                            FeedAdxInput(playerIndex, buf, n);
                        }
                    }
                }
                catch
                {
                }

                ClearAdxInputBuf(playerIndex);
                Thread.Sleep(500);
            }
        }

        private static void FeedAdxInput(int playerIndex, byte[] buf, int n)
        {
            if (_adxInputBuf == null || _adxInputBufPending == null || n < 13)
                return;

            // Windows HidLibrary ReadFile prepends dummy 0x00 ReportId when descriptor omits ReportId,
            // shifting Linux raw[0..12] -> Windows data[1..13] (buttons at 2..9, aux at 10..13).
            for (int i = 0; i < 13 && i < n; i++)
            {
                int idx = i + 1;
                byte b = buf[i];
                if (b == 1 && _adxInputBuf[playerIndex, idx] == 0)
                    _adxInputBufPending[playerIndex, idx] = 1;
                _adxInputBuf[playerIndex, idx] = b;

                if (!_loggedFirstHidButton[playerIndex] && i >= 1 && b != 0)
                {
                    _loggedFirstHidButton[playerIndex] = true;
                    MelonLogger.Msg($"[LinuxQrFix] First {playerIndex + 1}P HID button byte at raw[{i}]=0x{b:X2} (len={n}): {BitConverter.ToString(buf, 0, Math.Min(n, 16))}");
                }
            }
        }

        private static void ClearAdxInputBuf(int playerIndex)
        {
            if (_adxInputBuf == null || _adxInputBufPending == null)
                return;

            for (int i = 0; i < 32; i++)
            {
                _adxInputBuf[playerIndex, i] = 0;
                _adxInputBufPending[playerIndex, i] = 0;
            }
        }
    }
}
