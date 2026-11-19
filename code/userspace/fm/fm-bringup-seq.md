# FM bring-up sequence: app -> framework -> Broadcom service -> JNI

Sources (all from stock `system.raw`, world-readable image):
- `app/Bluetooth/Bluetooth.apk` -> `classes.dex` (contains
  `com.broadcom.fm.fmreceiver.*`: `FmService`, `FmNativehandler`,
  `FmReceiverServiceState`)
- `priv-app/HybridRadio2016_M/HybridRadio2016_M.apk` (resources/manifest only,
  NO dex inside) + `priv-app/HybridRadio2016_M/oat/arm/HybridRadio2016_M.odex`
  (ELF combines native code + dex at file offset `0x272c`, size `1754828` bytes
  per dex header; adler32+sha1 in the embedded header are zeroed/stale and must
  be recomputed before androguard will parse it)
- `framework/boot.oat` (compiled framework: `com.samsung.android.media.fmradio.*`,
  `com.android.server.FMPlayerNative*`, `com.broadcom.fm.fmreceiver.FmProxy`)
- `lib/libbluetooth_jni.so` (JNI registration), `lib/hw/bluetooth.default.so`
  (BTA_FM_* stack)
- `etc/floating_feature.xml`, `etc/audio_policy.conf`, `etc/mixer_paths.xml`

## 1. Process / service topology (manifest evidence)

Bluetooth.apk (`package="com.android.bluetooth"`,
`sharedUserId="android.uid.bluetooth"`):
- service `com.broadcom.fm.fmreceiver.FmService` (from androguard service list)
- `FmService.onCreate` log string: `"FM Service  onCreate"`;
  stub creation log: `"FmReceiverServiceStub created"`

FM app (`package="com.sec.android.app.fm"`, main activity
`com.sec.android.app.fm.MainActivity`):
- services: `FMListPlayerService`, `NotificationService`, `DNSService`
  (`com.sec.android.app.dns.DNSService`)
- receivers: `MediaButtonReceiver`, `SecretCodeReceiver`, `AlarmReceiver`,
  `FMResetSettingReceiver`, `FMPhoneStateReceiver`, `FMRadioProvider`,
  `BackupAndRestoreService`, `AutoResumeReceiver`
- permission `android.permission.ACCESS_FM_RECEIVER` (declared; framework
  `boot.oat` also contains `android.permission.ACCESS_FM_RECEIVER` and
  `FmReceiverServiceState.FM_RECEIVER_PERM`)
- action strings in app dex: `com.sec.android.fm.player.tune[.next/.prev]`,
  `com.sec.android.fm.volume_unlock`, `com.samsung.app.fmradio.PLAYBACK_VIEWER`

## 2. App layer: `com.sec.android.app.fm.RadioPlayer`

Singleton facade over framework `FmPlayer` (`mPlayer` field type
`Lcom/samsung/android/media/fmradio/FmPlayer;`,
listener `mPlayerListener: FmEventListener`).

### 2.1 `initialize(Context)` — service lookup
1. `context.getApplicationContext()`
2. `getSystemService("FMPlayer")` -> cast to `FmPlayer` -> `mPlayer`
3. `mPlayer.addListener(mPlayerListener)`
4. `getSystemService("audio")` -> `AudioManager mAudioManager`
5. `ChannelStore.getInstance()` -> `mChannelStore`
6. Speaker/volume restore path via `AudioManager.isRadioSpeakerOn()`,
   `RadioApplication.isSpeakerEnabled()`, `FMNotificationManager.initialize/registerNotification`

### 2.2 `turnOn()` — ordered bring-up (disassembled, `RadioPlayer`)
```
new WorkerThread() -> start()            // async op queue; tuneAsync posts doOperation(1, freq)
mPlayer.enableRadio() : boolean          // throws FmPlayerException (headset/airplane/TvOut variants)
  on success:
    applyStereo()                        // FmPlayer.setAudioMode(9) stereo | setAudioMode(8) mono
                                         // choice: mono if Settings.System "mono_audio_db" != 0 AND speaker on
    applyRds()                           // FmPlayer.setRadioDataSystemEnabled(pref "SettingsPreference"."stationid")
    applyAf()                            // FmPlayer.setAlternateFrequencyEnabled(pref "SettingsPreference"."af")
    [DNS hook if !FEATURE_DISABLEDNS]    // DNSService.runDNSSystem(new ModeData(3))
    restore stream volume:               // SamsungAudioManager.stream(1); getStreamVolume -> setStreamVolume(s, v, 0)
```
`turnOn(boolean)` variant skips the stereo/RDS/AF/volume steps (bare `enableRadio()`).
Evidence strings: `"turnOn()"`, `"setStereo() is called"`, `"setMono() is called"`,
`"applyAf() - "`, `"applyRds() - "`, `"The volume from audiomanager:"`.

### 2.3 Tune / seek / scan (disassembled)
- `tune(int freq)`: `mPlayer.tune((long)(freq * 10)) : boolean`.
  Units: app `freq` is 10 kHz steps; framework takes kHz
  (e.g. 8750 -> 87500 kHz). DNS pre-hook `ModeData(3)` same as turnOn.
- `tuneAsync(int)` / `tuneAsyncEx(int)`: `WorkerThread.doOperation(1, freq)`
  (op 1 = tune; `"tuneAsync() - freq : "` log).
- `seekUp()/seekDown() : long`, `seekUpAsync/seekDownAsync`,
  `scan()` -> `FmPlayer.startScan()`, `cancelScan()`, `cancelSeek()`,
  `getCurrentRssi() : long`.
- `getValidFrequency(f)`: clamp to `[8750, 10800]`, else
  `RadioApplication.getInitialFrequency()`.
- `getDefaultFrequency()`: `8830` if `FMRadioFeature.FEATURE_DEFAULTCHANNEL.equals("88.3")`
  else `8750`.
- Framework guard string: `"Invalid frequency value. Must be int between the values 7600 and 10800."`
- `mute(boolean)` -> `FmPlayer.setMuteEnabled(boolean) : boolean`;
  `setVolume(long)`; `enableSpeaker()/disableSpeaker()` ->
  `FmPlayer.setSpeakerEnabled(boolean)` (+ `RadioApplication.setSpeakerEnabled`).
- `isOn()` -> `FmPlayer.isRadioEnabled()`; `isScanning/isSeeking` passthrough.
- `turnOff() : boolean`: `mFrequency = -1`; `initAutoOff()`; DNS stop hook;
  `mPlayer.disableRadio() : boolean`; `WorkerThread.terminate()`.

### 2.4 Feature flags (`FMRadioFeature`, `etc/floating_feature.xml`)
Observed floating features (exact file contents):
```xml
<SEC_FLOATING_FEATURE_FMRADIO_CONFIG_CHIP_VENDOR>2</SEC_FLOATING_FEATURE_FMRADIO_CONFIG_CHIP_VENDOR>
<SEC_FLOATING_FEATURE_FMRADIO_REMOVE_AF_MENU>TRUE</SEC_FLOATING_FEATURE_FMRADIO_REMOVE_AF_MENU>
<SEC_FLOATING_FEATURE_FMRADIO_SUPPORT_HYBRID_RADIO>FALSE</SEC_FLOATING_FEATURE_FMRADIO_SUPPORT_HYBRID_RADIO>
<SEC_FLOATING_FEATURE_FMRADIO_SUPPORT_RDS>TRUE</SEC_FLOATING_FEATURE_FMRADIO_SUPPORT_RDS>
```
CSC keys referenced by app dex (values live in CSC partition, NOT in
system.raw — values below are key names only):
`CscFeature_FMRadio_BandWidthAs`, `CscFeature_FMRadio_DeconstantAs`,
`CscFeature_FMRadio_DefaultChannelAs`, `CscFeature_FMRadio_FrequencySpaceAs`,
`CscFeature_FMRadio_RecordingVolumeAs`, `CscFeature_FMRadio_DisableMenuAf`,
`CscFeature_FMRadio_DisableMenuRds`, `CscFeature_FMRadio_DisableMenuInternetRadio`,
`CscFeature_FMRadio_DisableRTPlusInfo`,
`SEC_FLOATING_FEATURE_FMRADIO_REMOVE_AF_MENU`,
`SEC_FLOATING_FEATURE_FMRADIO_SUPPORT_HYBRID_RADIO`.
`FMRadioFeature` fields: `CHIP_VENDOR, DISABLE_AF_MENU, ENABLE_HYBRIDRADIO,
FEATURE_BANDWIDTH, FEATURE_DECONSTANT, FEATURE_DEFAULTCHANNEL, FEATURE_DISABLEDNS,
FEATURE_DISABLEMENUAF/RDS, FEATURE_DISABLERTPLUSINFO, FEATURE_ENABLE_SURVEY_MODE,
FEATURE_FREQUENCYSPACE, FEATURE_RECORDINGVOLUME, SUPPORT_RDS`.

## 3. Framework layer (names from `boot.oat` strings; numeric values NOT recovered)

- `com.samsung.android.media.fmradio.FmPlayer` — public API used by the app
  (37 `method_id`s observed in app dex; selection with descriptors):
  `enableRadio()Z`, `disableRadio()Z`, `isRadioEnabled()Z`,
  `tune(J)Z`, `seekUp()J`, `seekDown()J`, `startScan()V`, `cancelScan()Z`,
  `cancelSeek()V`, `getCurrentChannel()J`,
  `setMuteEnabled(Z)Z`, `setVolume(J)V`, `setSpeakerEnabled(Z)Z`,
  `setAudioMode(I)V`, `setChannelSpacing(I)V`,
  `setAlternateFrequencyEnabled(Z)V`, `setRadioDataSystemEnabled(Z)V`,
  `setRadioDomainNameSystemEnabled(Z)V`, `setInternetStreamingEnabled(Z)V`,
  `setRecordMode(Z)V`, `configureSoftmute(III)V`, `setSoftmuteEnabled(Z)V`,
  `getTunningParameter(...)` / `setTunningParameter(...)` x3 overloads,
  `addListener/removeListener(FmEventListener)`,
  `isRadioDataSystemEnabled/isRadioDomainNameSystemEnabled/isScanning/isSeeking/isSoftmuteEnabled`.
  Exceptions: `FmPlayerException`, `FmPlayerNotEnabledException`,
  `FmPlayerScanningException`, `AirPlaneModeEnabledException`,
  `HeadsetNotConnectedException`, `LowBatteryException`, `TvOutConnectedException`.
- `com.samsung.android.media.fmradio.internal.IFMPlayer` (+ `$Stub`, `$Stub$Proxy`)
  and `IFMEventListener` — Binder interface to system_server (name observed:
  `com.samsung.android.media.fmradio.internal.IFMPlayer`).
- `com.android.server.FMPlayerNative` (`FMPlayerNative_brcm` string) +
  inner `FmReceiverCallback`, `RDSData`, `RTPlusData`, `PIECCData`,
  `FMPlayerVolumeTable` — native-backed bridge. Every bridge method guards on
  `mBcomService != null`, e.g. `"on() mBcomService is null."`,
  `"tune() mBcomService is null."`, `"setBand() mBcomService is null."`,
  `"setChannelSpacing() mBcomService is null."`, `"setAudioPath() mBcomService is null."`,
  `"setDEConstant() / setMono() / setStereo() / setVolume() / muteOn() / muteOff() /
  seekUp() / seekDown() / searchAll() / cancelSeek() / setRdsMode() / setCnt_th() /
  setSNR_th() / setSoftmute() / setNoisePowerThreshold() / setPilotPowerThreshold() /
  setFrequencyOffsetThreshold() / setFMIntenna() / enableDNS() / disableDNS() /
  getCurrentRSSI() / getCurrentSNR() / offFMService() mBcomService is null."`
- `com.broadcom.fm.fmreceiver.FmProxy` (`FmProxy.java`), `FmReceiverServiceConfig`
  — client-side proxy/config; bind failure string:
  `"Could not bind to IFmReceiverService with "`.
- Constant vocabularies (names only; ordinals live in compiled framework):
  bands `BAND_76000_108000_kHz / BAND_76000_90000_kHz / BAND_87500_108000_kHz`
  (also as `BANDWIDTHAS_...`), `BAND_FM / BAND_FM_HD / BAND_AM / BAND_AM_HD`;
  regions `FUNC_REGION_DEFAULT / FUNC_REGION_EUR / FUNC_REGION_NA / FUNC_REGION_JP /
  FUNC_REGION_JP_II` (+ `REGION_JPN`); steps `FREQ_STEP_50KHZ / FREQ_STEP_100KHZ /
  FREQ_STEP_DEFAULT`; de-emphasis `DEEMPHASIS_50U / DEEMPHASIS_75U / DEEMPHASIS_TIME_DEFAULT`;
  audio paths `AUDIO_PATH_NONE / DIGITAL / SPEAKER / WIRE_HEADSET`
  (+ `ACTION_ON_AUDIO_PATH / EXTRA_AUDIO_PATH`);
  intents `com.broadcom.bt.app.fm.action.ON_STATUS / ON_SEEK_CMPL / ON_RDS_DATA /
  ON_RDS_MODE / ON_AUDIO_PATH / ON_AUDIO_MODE / ON_AUDIO_QUAL / ON_EST_NFL / ON_WRLD_RGN`.

## 4. Broadcom service layer (`Bluetooth.apk` dex, disassembled)

AIDL-ish surface: `IFmReceiverService$Stub` + `IFmReceiverCallback`
(`onStatusEvent`, `onRdsDataEvent`, `onRdsPIECCEvent`, `onRdsRTPlusEvent`, `onRdsAFEvent`).
`FmService$FmReceiverServiceStub` exposes 1:1 Java methods (all observed):
`init/cleanUp/cleanupFmService/getStatus/getTunedFrequency/getRadioIsOn/getIsMute/
getMonoStereoMode/getCurrentRssi/getCurrentSNR/turnOnRadio/turnOffRadio/tuneRadio/
seekStation/seekRdsStation/seekStationCombo/seekStationAbort/setWorldRegion/
setStepSize/setAudioPath/setAudioMode/setDeemPhasis/setCOS/setSnrThreshold/
setFrequencyOffsetThreshold/setNoisePowerThreshold/setPilotPowerThreshold/
setRdsMode/setLiveAudioPolling/setPiEccMode/setRfMute/setFMVolume/setFMIntenna/
setSnrThreshold/estimateNoiseFloorLevel/muteAudio/registerCallback/unregisterCallback/
offFMService`.

### 4.1 `turnOnRadio(int functionalityMask, char[] clientName) : int`
(disassembled; 0 = initiated OK, nonzero = failure stage)
1. `mClientName = String.copyValueOf(clientName)`; store `mFunctionalityMask`
2. `registerIntent()` + `registerBluetoothStateReceiver()`
3. if `getRadioIsOn()` -> `process_enableChip()` (fast path, radio already up)
   else `BluetoothAdapter.enableRadio()` (powers the shared BT/FM chip) ->
   `initializeNative()` -> `process_turnOnRadio(mask)`:
   `mFuncMask = mask`; `radio_state = STARTING(1)`; arm 10 s watchdog
   (`operationHandler.sendMessageDelayed(..., 10000)`);
   **`enableFmNative(mask) : boolean`** -> on false log `"turnOnRadioNative is failed"`.
4. On success the service posts `mHandler` message 3/arg 0 after 50 ms
   (`MESSAGE_SEND_RADIO_ON_EVENT_DELAY` path) and later fires `onRadioOnEvent`.

### 4.2 Config / tune / seek (each returns 0 OK / 2 fail; exceptions -> 2)
All synchronized wrappers masking args then calling `*Native`:
- `setWorldRegion(int)` -> `setRegionNative(int)` (mask: none)
- `setStepSize(int)` -> `setScanStepNative(int)`
- `setAudioPath(int)` -> `setAudioPathNative(int)` (`& 3`)
- `setAudioMode(int)` -> `setAudioModeNative(int)` (`& 3`)
- `setDeemPhasis(int)` -> `configureDeemphasisNative(int)`
- `muteAudio(boolean)` -> `muteNative(boolean)`
- `setRfMute`/`setFMIntenna`/`setFMVolume`/`setCOS`/`setSnrThreshold`/
  `setFrequencyOffsetThreshold`/`setNoisePowerThreshold`/`setPilotPowerThreshold`/
  `setPiEccMode`/`setRdsMode(int x4)` (`setRdsModeNative(z,z,i)` + `setRtPsModeNative(z)`)/
  `setLiveAudioPolling` — analogous
- `tuneRadio(int freqKHz)` -> `tuneNative(int)`
- `seekStation(int mode, int dir)` -> `searchNative(mode, dir&0x83, 0, 0) : long`
- `seekStationCombo(s,e,minSig,dir,method,multiCh,rdsType,rdsVal)` ->
  `comboSearchNative(...) : long`
- `estimateNoiseFloorLevel(int)`; `getStatus()` packs
  `(mFreq, mRssi, mSnr, mRadioIsOn, mRdsProgramType, PS, RT, PTYN, mIsMute)`
  into `FM_Status_Params` -> `sendStatusEventCallback(...)`.

### 4.3 `turnOffRadio(boolean keepChipOn)` / teardown
`process_turnOffRadio`: `IsKeepChipOn = keep`; `radio_state = STOPPING(3?)`;
arm 10 s watchdog; **`disableFmNative(false)`**; on false log
`"disableFmNative is failed."`; then `onRadioOffEvent(0)` -> `clearRDS()` ->
if `!keepChipOn`: `process_disableChip()` =
`BluetoothAdapter.disableRadio()` unless BT is ON (state 10/13 checks),
plus `"SKW Send disable service"` handler ping.
`stop()` -> `cleanupNative()` + `clearRDS()`; `start()` only flips `mIsStarted`
(`"Service already started...Skipping"`).

### 4.4 State defaults (`FmReceiverServiceState.<clinit>`, disassembled)
`mFuncMask = 80 (0x50)`, `mFreq = 0`, `mRssi = mSnr = 127`,
`mRadioIsOn = mIsMute = mSeekSuccess = mRdsOn = mAfOn = mRtPsOn = mPiEccOn = false`,
`mRdsType = mAlternateFreqHopThreshold = mAudioMode = 0`, **`mAudioPath = 1`**,
`mWorldRegion = 0`, `mStepSize = 0`, `mLiveAudioQuality = false`,
`mEstimatedNoiseFloorLevel = 1`, `mSignalPollInterval = 100`,
**`mDeemphasisTime = 64`**, `radio_state = 0 (OFF)`, `mRdsPi = mRdsEcc = -1`,
`radio_op_state = 1`.
Op/timeout vocab: `OPERATION_ENABLE/DISABLE/GENERIC/QUEUE_FM_CMD/STATUS_EVENT_CALLBACK/
RDS_DATA_EVENT_CALLBACK/RDS_RTPLUS_EVENT_CALLBACK/RDS_AF_EVENT_CALLBACK`,
`OPERATION_TIMEOUT_STARTUP/SHUTDOWN/SEARCH/NFL/GENERIC`,
`OPERATION_TIME_OUT_MILLIS`, `"BTAPP TIMEOUT on OPERATION_ENABLE/DISABLE"`.

## 5. JNI boundary (native evidence)

- `libbluetooth_jni.so` exports
  `_ZN7android32register_com_broadcom_fm_serviceEP7_JNIEnv`
  (`register_com_broadcom_fm_service(JNIEnv*)`) and the method names
  `com/broadcom/fm/fmreceiver/FmNativehandler`, `enableFmNative`,
  `disableFmNative`, `setFMVolumeNative`, `setRFMuteNative`, `setFMIntennaNative`;
  failure log: `"jni FMRadio service registration failure: %d"` (tag `FmServiceJni`).
- `libbluetooth_jni.so` FM JNI logs pin down semantics:
  `"[JNI] - setRFMuteNative : rf_mute = %i"`,
  `"[JNI] - setFMVolumeNative : volume = %i"`,
  `"[JNI] - BTA_FM_SEARCH_CMPL_EVT: status = %i rssi = %i freq = %i snr = %i"`,
  `"[JNI] - TRANSMITTING EVENT BTA_FM_AF_JMP_EVT : [%i,%i,%i]"`,
  `"[JNI] - BTA_FM_RDS_UPD_EVT, PS=[%s] / RT=[%s]"`,
  `"[JNI] ***** waitFmReceiver is failed in %d sec. condition=0x%x"`,
  `"Fm module is not loaded"`, `"Failed to get Fm Interface"`,
  `"Failed to initialize Fm, status: %d"`, `"Fm cleanupNative"`.
- `bluetooth.default.so` implements the stack side: `bta_fm_enable/disable`,
  `BTA_FmEnable/BTA_FmDisable/BTA_FmConfigAudioPath/BTA_FmConfigDeemphasis/
  BTA_FmConfigBlendSoftMuteParams/BTA_FmComboSearch/BTA_FmEstNoiseFloor`,
  `bta_fm_api_enable`, `bta_fm_af_switch`, `bta_fm_check_reg_param`, etc.
  HCI opcode mapping (VS `0xFC15` and per-cmd bytes) is recovered in the
  companion HCI slice, not here.

## 6. Audio path (no code changes needed; config evidence)

- `etc/audio_policy.conf`: `primary` input lists
  `AUDIO_DEVICE_IN_FM` among capture devices.
- `etc/mixer_paths.xml` (S2801 codec): `<pcmdai fmradio_link="4" />`;
  paths `route-ap-fm` (SRC3 En), `route-ap-fm-tx` (CH1/CH3 mixers), `route-ap-fm-rx`
  (RMIX2, CH3 Rec), `dev-fm-speaker` (SPK ADCL/R Switch = 1),
  `dev-fm-headset` (HPL/HPR ADCL/R Switch = 1), `dev-fm-input`
  (LINELL/LINERL/LINELR/LINERR routing).
- App-side volume path is `AudioManager.setStreamVolume(SamsungAudioManager.stream(1), v, 0)`.

## 7. Ordered bring-up / tune / teardown (putting it together)

Bring-up:
1. App `RadioPlayer.initialize()` binds framework `FmPlayer` (`getSystemService("FMPlayer")`)
   + registers `FmEventListener`.
2. `RadioPlayer.turnOn()` starts `WorkerThread`, calls `FmPlayer.enableRadio()`
   (requires headset; throws `HeadsetNotConnectedException` etc.).
3. Framework `FMPlayerNative` binds `IFmReceiverService`
   (`"Could not bind to IFmReceiverService with "` on failure).
4. `FmService.turnOnRadio(functionalityMask, clientName)` ->
   `BluetoothAdapter.enableRadio()` (shared chip power) ->
   `initializeNative()` -> `enableFmNative(mask)` -> BTA_FM_ENABLE -> HCI VS.
5. App post-enable config: `setAudioMode(8|9)` (mono/stereo),
   `setRadioDataSystemEnabled(stationid?)`, `setAlternateFrequencyEnabled(af?)`,
   volume restore; service-level equivalents `setAudioPath/setWorldRegion/
   setStepSize/setDeemPhasis/setRdsMode` are available but the stock app drives
   band/region/step via framework `setBand/setChannelSpacing/setDEConstant`
   (`"setBand() / setChannelSpacing() / setDEConstant() mBcomService is null."`
   strings prove these framework methods exist; numeric codes in compiled
   framework only).
6. `tune(freq10kHz)` -> `tuneNative(freq10kHz*10 kHz)`; valid app range
   8750..10800 (default 8750, or 8830 when `FEATURE_DEFAULTCHANNEL == "88.3"`),
   framework accepts 7600..10800.

Teardown:
`RadioPlayer.turnOff()` -> `FmPlayer.disableRadio()` ->
`offFMService/turnOffRadio(keepChipOn)` -> `disableFmNative(false)` ->
`BluetoothAdapter.disableRadio()` (only if BT itself is off) ->
`cleanupNative()`.

## 8. Explicit inferences (not directly observed)

- [INFERENCE] Numeric values of `FUNC_REGION_*`, `FREQ_STEP_*`, `DEEMPHASIS_*`,
  `AUDIO_PATH_*`, `BAND_*` enums: names are confirmed in `boot.oat`, but the
  framework is compiled (oat) so ordinals were not recovered. Service defaults
  (`mWorldRegion=0`, `mStepSize=0`, `mAudioPath=1`, `mDeemphasisTime=64`) are
  observed from `FmReceiverServiceState.<clinit>` bytecode.
- [INFERENCE] `functionalityMask` bit layout (app passes an int through
  `FmPlayer.enableRadio()` -> `turnOnRadio(mask, name)`; default service mask
  `0x50` = `FUNC_*` bitmask combination). Exact bit assignment needs the
  framework `FmPlayer.enableRadio()` disassembly (oat).
- [INFERENCE] HCI VS `0xFC15` payload mapping for each `*Native` call is NOT in
  this slice — see the companion HCI command-table slice
  (`bta_fm_snd_vendor_cmd`, cmd bytes `0x00/02/05/08/09/0a/14/16/4d/de/fc/fd/fe`).
- [INFERENCE] `WorkerThread.doOperation` opcodes beyond op 1 (tune) were not
  decoded; teardown order at the app layer (`disableRadio` before thread
  terminate) IS observed.
- [INFERENCE] `FEATURE_*` CSC values (bandwidth/deconstant/frequencyspace/
  recordingvolume) live on the CSC partition, absent from `system.raw`; only key
  names + `floating_feature.xml` values above are grounded.
