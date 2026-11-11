/****************************************************************************
 *
 * DhakshaGroundControl: owns the single active gimbal camera.
 *
 ****************************************************************************/

#include "DhakshaCameraManager.h"
#include "DhakshaNativePlayer.h"
#include "SiyiGimbalCamera.h"

#ifndef DHAKSHA_CAMERA_NO_QGC
#include "DhakshaFlightLog.h"
#include "QGCApplication.h"
#include "QGCToolbox.h"
#include "SettingsManager.h"
#include "VideoManager.h"
#include "VideoSettings.h"
#endif

#include <QQmlEngine>
#include <QSettings>

namespace {
    const char* kSettingsGroup  = "DhakshaCamera";
    const char* kIpKey          = "ip";
    const char* kRtspKey        = "rtspUrl";
    const char* kVideoModeKey   = "videoMode";
    // Read by GstVideoReceiver when it opens the RTSP stream
    const char* kRtspLatencyKey = "DhakshaCamera/rtspLatencyMs";
    const char* kRtspTcpKey     = "DhakshaCamera/rtspTcp";
    const char* kGimbalSpeedKey = "DhakshaCamera/gimbalMaxSpeed";
    const char* kNativeVideoKey = "nativeVideo";
    const char* kNativeTransportKey = "nativeTransport";

    // The A8 mini's own settings group, so settings another Dhaksha build saved for its cameras are not read
    const char* kCameraGroup    = "a8mini";
}

DhakshaCameraManager* DhakshaCameraManager::instance()
{
    static DhakshaCameraManager* manager = new DhakshaCameraManager();
    return manager;
}

QObject* DhakshaCameraManager::qmlSingletonFactory(QQmlEngine*, QJSEngine*)
{
    DhakshaCameraManager* manager = instance();
    QQmlEngine::setObjectOwnership(manager, QQmlEngine::CppOwnership);
    return manager;
}

DhakshaCameraManager::DhakshaCameraManager(QObject* parent)
    : QObject(parent)
{
    QSettings settings;
    settings.beginGroup(kSettingsGroup);
    // The A8 mini is the only camera, whatever another build saved
    _vendor = VendorA8Mini;
    _nativeVideo = settings.value(kNativeVideoKey, true).toBool();
    _nativeTransport = qBound(static_cast<int>(NativeTransportTcp), settings.value(kNativeTransportKey, NativeTransportTcp).toInt(),
                              static_cast<int>(NativeTransportUdp));
    settings.endGroup();
    _loadVendorSettings();
}

QStringList DhakshaCameraManager::vendorNames() const
{
    return { tr("None"), QStringLiteral("A8 mini") };
}

QString DhakshaCameraManager::defaultCameraIp(int vendor)
{
    switch (vendor) {
    case VendorA8Mini:      return QStringLiteral("192.168.144.25");
    default:                return QString();
    }
}

QString DhakshaCameraManager::defaultRtspUrl(int vendor)
{
    switch (vendor) {
    case VendorA8Mini:      return QStringLiteral("rtsp://192.168.144.25:8554/main.264");
    default:                return QString();
    }
}

void DhakshaCameraManager::_loadVendorSettings()
{
    QSettings settings;
    settings.beginGroup(kSettingsGroup);
    settings.beginGroup(kCameraGroup);
    setCameraIp(settings.value(kIpKey, defaultCameraIp(_vendor)).toString());
    setRtspUrl(settings.value(kRtspKey, defaultRtspUrl(_vendor)).toString());
    setVideoMode(settings.value(kVideoModeKey, defaultVideoMode(_vendor)).toInt());
}

void DhakshaCameraManager::_saveSettings()
{
    QSettings settings;
    settings.beginGroup(kSettingsGroup);
    settings.beginGroup(kCameraGroup);
    settings.setValue(kIpKey, _cameraIp);
    settings.setValue(kRtspKey, _rtspUrl);
    settings.setValue(kVideoModeKey, _videoMode);
}

void DhakshaCameraManager::setVendor(int vendor)
{
    vendor = qBound(0, vendor, VendorCount - 1);
    if (vendor != _vendor) {
        _vendor = vendor;
        emit vendorChanged();
        _loadVendorSettings();
    }
}

void DhakshaCameraManager::setCameraIp(const QString& cameraIp)
{
    const QString trimmed = cameraIp.trimmed();
    if (trimmed != _cameraIp) {
        _cameraIp = trimmed;
        emit cameraIpChanged();
    }
}

void DhakshaCameraManager::setRtspUrl(const QString& rtspUrl)
{
    const QString trimmed = rtspUrl.trimmed();
    if (trimmed != _rtspUrl) {
        _rtspUrl = trimmed;
        emit rtspUrlChanged();
    }
}

int DhakshaCameraManager::defaultVideoMode(int vendor)
{
    // SIYI cameras over the SIYI link froze with QGC's 17 ms buffer
    Q_UNUSED(vendor)
    return VideoSmooth;
}

void DhakshaCameraManager::setVideoMode(int mode)
{
    mode = qBound(static_cast<int>(VideoLowDelay), mode, static_cast<int>(VideoReliable));
    if (mode != _videoMode) {
        _videoMode = mode;
        emit videoModeChanged();
    }
}

int DhakshaCameraManager::gimbalMaxSpeed() const
{
    return qBound(kMinGimbalSpeed, QSettings().value(kGimbalSpeedKey, kMaxGimbalSpeed).toInt(), kMaxGimbalSpeed);
}

void DhakshaCameraManager::setGimbalMaxSpeed(int degreesPerSecond)
{
    degreesPerSecond = qBound(kMinGimbalSpeed, degreesPerSecond, kMaxGimbalSpeed);
    if (degreesPerSecond != gimbalMaxSpeed()) {
        QSettings().setValue(kGimbalSpeedKey, degreesPerSecond);
        emit gimbalMaxSpeedChanged();
    }
}

void DhakshaCameraManager::restoreVendorDefaults()
{
    setCameraIp(defaultCameraIp(_vendor));
    setRtspUrl(defaultRtspUrl(_vendor));
}

void DhakshaCameraManager::apply()
{
    _saveSettings();
    start();
}

void DhakshaCameraManager::start()
{
    _createCamera();
    _applyVideoMode();
    _applyNativeVideo();
    _startVideoStream();
    emit videoRestartRequested();
}

QString DhakshaCameraManager::flightLogPath()
{
#ifndef DHAKSHA_CAMERA_NO_QGC
    return DhakshaFlightLog::filePath();
#else
    return QString();
#endif
}

bool DhakshaCameraManager::nativeVideoSupported()
{
    return DhakshaNativePlayer::supported();
}

void DhakshaCameraManager::setNativeVideo(bool native)
{
    if (native != _nativeVideo) {
        _nativeVideo = native;
        QSettings settings;
        settings.beginGroup(kSettingsGroup);
        settings.setValue(kNativeVideoKey, native);
        _applyNativeVideo();
    }
}

void DhakshaCameraManager::setNativeTransport(int transport)
{
    transport = qBound(static_cast<int>(NativeTransportTcp), transport, static_cast<int>(NativeTransportUdp));
    if (transport != _nativeTransport) {
        _nativeTransport = transport;
        QSettings settings;
        settings.beginGroup(kSettingsGroup);
        settings.setValue(kNativeTransportKey, transport);
        emit nativeTransportChanged();
    }
}

void DhakshaCameraManager::setNativeVideoStatus(const QString& status)
{
    if (status != _nativeVideoStatus) {
        _nativeVideoStatus = status;
        emit nativeVideoStatusChanged();
    }
}

void DhakshaCameraManager::_applyNativeVideo()
{
    _nativeVideoActive = nativeVideoSupported() && _nativeVideo && (_vendor != VendorNone);
#ifndef DHAKSHA_CAMERA_NO_QGC
    qgcApp()->toolbox()->videoManager()->setMainVideoExternal(_nativeVideoActive);
#endif
    emit nativeVideoChanged();
}

void DhakshaCameraManager::_createCamera()
{
    if (_camera) {
        _camera->deleteLater();
        _camera = nullptr;
    }

    switch (_vendor) {
    case VendorA8Mini:
        _camera = new SiyiGimbalCamera(_cameraIp, _rtspUrl, this);
        break;
    default:
        break;
    }
    emit cameraChanged();
}

void DhakshaCameraManager::_startVideoStream()
{
    if (_vendor != VendorNone) {
        _showVideo(_rtspUrl);
    }
}

void DhakshaCameraManager::_showVideo(const QString& rtspUrl)
{
    if (!rtspUrl.isEmpty() && (rtspUrl != _mainVideoUrl)) {
        _mainVideoUrl = rtspUrl;
        emit mainVideoUrlChanged();
    }
#ifndef DHAKSHA_CAMERA_NO_QGC
    if (rtspUrl.isEmpty()) {
        return;
    }
    VideoSettings* videoSettings = qgcApp()->toolbox()->settingsManager()->videoSettings();
    videoSettings->rtspUrl()->setRawValue(rtspUrl);
    videoSettings->videoSource()->setRawValue(VideoSettings::videoSourceRTSP);
#else
    Q_UNUSED(rtspUrl)
#endif
}

void DhakshaCameraManager::_applyVideoMode()
{
    const int  latencyMs = _videoMode == VideoSmooth ? 150 : _videoMode == VideoReliable ? 100 : 17;
    const bool tcp       = _videoMode == VideoReliable;

    QSettings settings;
    const bool changed = settings.value(kRtspLatencyKey, 17).toInt() != latencyMs || settings.value(kRtspTcpKey, false).toBool() != tcp;
    settings.setValue(kRtspLatencyKey, latencyMs);
    settings.setValue(kRtspTcpKey, tcp);
#ifndef DHAKSHA_CAMERA_NO_QGC
    // The receiver reads these when it opens the stream, so a running stream is reopened
    VideoManager* videoManager = qgcApp()->toolbox()->videoManager();
    if (changed && videoManager) {
        videoManager->stopVideo();
        videoManager->startVideo();
    }
#else
    Q_UNUSED(changed)
#endif
}
