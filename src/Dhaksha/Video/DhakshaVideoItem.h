#pragma once

#include <QtCore/QElapsedTimer>
#include <QtCore/QPointer>
#include <QtCore/QSize>
#include <QtCore/QTimer>
#include <QtQuick/QQuickItem>
#include <atomic>

class DhakshaNativePlayer;

/// DhakshaGroundControl's fast camera player for the Fly View. Plays an RTSP camera with
/// DhakshaNativePlayer and draws the newest decoded frame straight from the hardware decoder's GPU
/// surface, inside the Qt Quick scene so the map and controls stay on top of it.
/// Android only; elsewhere `supported` is false and the item draws nothing.
class DhakshaVideoItem : public QQuickItem
{
    Q_OBJECT
    Q_PROPERTY(QString source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(bool tcp READ tcp WRITE setTcp NOTIFY tcpChanged)  ///< RTP over the RTSP connection, else over UDP
    Q_PROPERTY(bool cropToFill READ cropToFill WRITE setCropToFill NOTIFY
                   cropToFillChanged)  ///< Fill the item, cropping the video, instead of fitting it
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)  ///< Frames are arriving
    Q_PROPERTY(QSize videoSize READ videoSize NOTIFY videoSizeChanged)
    Q_PROPERTY(QString decoderName READ decoderName NOTIFY videoSizeChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY statsChanged)  ///< The camera answered and is sending
    Q_PROPERTY(int fps READ fps NOTIFY statsChanged)               ///< Frames drawn per second
    Q_PROPERTY(int droppedFrames READ droppedFrames NOTIFY statsChanged)
    Q_PROPERTY(QString codecName READ codecName NOTIFY statsChanged)
    Q_PROPERTY(QString transportName READ transportName NOTIFY statsChanged)
    Q_PROPERTY(bool supported READ supported CONSTANT)

public:
    explicit DhakshaVideoItem(QQuickItem* parent = nullptr);
    ~DhakshaVideoItem() override;

    QString source() const { return _source; }

    bool tcp() const { return _tcp; }

    bool cropToFill() const { return _cropToFill; }

    bool playing() const { return _playing; }

    QSize videoSize() const { return _videoSize; }

    QString decoderName() const { return _decoderName; }

    bool connected() const { return _connected; }

    int fps() const { return _fps; }

    int droppedFrames() const { return _droppedFrames; }

    QString codecName() const { return _codecName; }

    QString transportName() const { return _transportName; }

    bool supported() const;

    void setSource(const QString& source);
    void setTcp(bool tcp);
    void setCropToFill(bool cropToFill);

    /// Reconnects to the camera
    Q_INVOKABLE void restart();

signals:
    void sourceChanged();
    void tcpChanged();
    void cropToFillChanged();
    void playingChanged();
    void videoSizeChanged();
    void statsChanged();

protected:
    QSGNode* updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data) override;
    void itemChange(ItemChange change, const ItemChangeData& value) override;

private:
    void _scheduleRestart();
    void _updateStatus();

    DhakshaNativePlayer* _player = nullptr;
    QString _source;
    bool _tcp = true;
    bool _cropToFill = false;
    bool _playing = false;
    QSize _videoSize;
    QString _decoderName;
    bool _connected = false;
    int _fps = 0;
    int _droppedFrames = 0;
    QString _codecName;
    QString _transportName;
    quint64 _lastFrames = 0;
    int _statusTicks = 0;
    QElapsedTimer _sinceStatus;
    QTimer _restartTimer;
    QTimer _statusTimer;
    std::atomic<bool> _updateQueued{false};
};
