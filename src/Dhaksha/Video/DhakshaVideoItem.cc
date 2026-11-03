#include "DhakshaVideoItem.h"

#include <QtQuick/QQuickWindow>
#include <QtQuick/QSGRendererInterface>
#include <limits>

#include "DhakshaFlightLog.h"
#include "DhakshaNativePlayer.h"

#if defined(__android__) || defined(Q_OS_ANDROID)
#define DHAKSHA_NATIVE_ITEM 1
#include <QtAndroidExtras/QAndroidJniEnvironment>
#include <QtAndroidExtras/QAndroidJniObject>
#include <QtGui/QMatrix4x4>
#include <QtGui/QOpenGLContext>
#include <QtGui/QOpenGLFunctions>
#include <QtQuick/QSGRenderNode>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#endif

#ifdef DHAKSHA_NATIVE_ITEM

namespace {

/// Qt 5's QAndroidJniEnvironment has no checkAndClearExceptions()
bool clearJniException(QAndroidJniEnvironment& env)
{
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return true;
    }
    return false;
}

constexpr GLenum kTextureExternalOes = 0x8D65;  // GL_TEXTURE_EXTERNAL_OES
// Frames waiting on the surface latched in one render; the last one is drawn
constexpr int kMaxLatchesPerRender = 16;
// Renders that look again for a frame the decoder released but the surface has not received yet
constexpr int kMaxLatchRetries = 8;

const char* const kVertexShader = R"(
attribute vec2 aPosition;
attribute vec2 aTexCoord;
uniform mat4 uMvp;
uniform mat4 uTexMatrix;
varying vec2 vTexCoord;
void main() {
    gl_Position = uMvp * vec4(aPosition, 0.0, 1.0);
    vTexCoord = (uTexMatrix * vec4(aTexCoord, 0.0, 1.0)).xy;
})";

const char* const kFragmentShader = R"(#extension GL_OES_EGL_image_external : require
precision mediump float;
uniform samplerExternalOES uTexture;
varying vec2 vTexCoord;
void main() {
    gl_FragColor = texture2D(uTexture, vTexCoord);
})";

/// Draws the hardware decoder's surface: the decoder writes into an Android SurfaceTexture bound to
/// a GL external texture, and each render latches the newest decoded frame and draws it. Runs on
/// the Qt render thread with Qt Quick's OpenGL ES context current.
///
/// SurfaceTexture.updateTexImage() takes the oldest queued frame, so one call per render let frames pile
/// up behind a slow display, and the full queue then held up the decoder. Each render now latches until
/// the timestamp stops changing, and asks the render thread for another frame itself while the decoder
/// has released a newer one, without waiting for the busy GUI thread.
class DhakshaVideoNode : public QSGRenderNode
{
public:
    explicit DhakshaVideoNode(std::shared_ptr<DhakshaDecoderCore> decoder) : _decoder(std::move(decoder)) {}

    ~DhakshaVideoNode() override
    {
        // GL objects need the context, which only releaseResources() is guaranteed to have
        _releaseSurface();
    }

    void sync(const QRectF& rect, bool cropToFill, QQuickWindow* window)
    {
        _rect = rect;
        _cropToFill = cropToFill;
        _quickWindow = window;
    }

    StateFlags changedStates() const override { return BlendState | DepthState; }

    RenderingFlags flags() const override { return BoundedRectRendering; }

    QRectF rect() const override { return _rect; }

    void releaseResources() override
    {
        _releaseSurface();
        QOpenGLContext* context = QOpenGLContext::currentContext();
        if (context) {
            QOpenGLFunctions* gl = context->functions();
            if (_program) {
                gl->glDeleteProgram(_program);
            }
            if (_texture) {
                gl->glDeleteTextures(1, &_texture);
            }
        }
        _program = 0;
        _texture = 0;
    }

    void render(const RenderState* state) override
    {
        QOpenGLContext* context = QOpenGLContext::currentContext();
        if (!context || _rect.isEmpty()) {
            return;
        }
        QOpenGLFunctions* gl = context->functions();
        if (!_program && !_initGl(gl)) {
            return;
        }
        if (!_window && !_createSurface()) {
            return;
        }

        const qint64 released = _decoder->lastRenderedTimestampNs();
        if (released != _releasedTimestampNs) {
            _releasedTimestampNs = released;
            _latchRetries = 0;
        }
        if (_latchedTimestampNs < released) {
            (void) _latchNewest();
            // Released by the decoder but not on the surface yet: look again on the next frame
            if ((_latchedTimestampNs < released) && (++_latchRetries <= kMaxLatchRetries) && _quickWindow) {
                _quickWindow->update();
            }
        }
        if (!_hasImage) {
            return;
        }

        // Fit the picture inside the item, or fill the item by showing only the middle of the picture
        QRectF target = _rect;
        float s0 = 0.f, s1 = 1.f, t0 = 0.f, t1 = 1.f;
        const QSize video = _decoder->videoSize();
        if (video.isValid() && (video.height() > 0)) {
            const qreal videoAspect = static_cast<qreal>(video.width()) / video.height();
            const qreal itemAspect = _rect.width() / _rect.height();
            if (_cropToFill) {
                if (videoAspect > itemAspect) {
                    const float visible = static_cast<float>(itemAspect / videoAspect);
                    s0 = (1.f - visible) / 2.f;
                    s1 = (1.f + visible) / 2.f;
                } else {
                    const float visible = static_cast<float>(videoAspect / itemAspect);
                    t0 = (1.f - visible) / 2.f;
                    t1 = (1.f + visible) / 2.f;
                }
            } else if (videoAspect > itemAspect) {
                const qreal height = _rect.width() / videoAspect;
                target = QRectF(_rect.left(), _rect.center().y() - height / 2, _rect.width(), height);
            } else {
                const qreal width = _rect.height() * videoAspect;
                target = QRectF(_rect.center().x() - width / 2, _rect.top(), width, _rect.height());
            }
        }

        // Triangle strip: top-left, bottom-left, top-right, bottom-right. Texture t runs bottom to top.
        const GLfloat positions[] = {
            float(target.left()),  float(target.top()), float(target.left()),  float(target.bottom()),
            float(target.right()), float(target.top()), float(target.right()), float(target.bottom()),
        };
        const GLfloat texCoords[] = {
            s0, t1, s0, t0, s1, t1, s1, t0,
        };

        const QMatrix4x4 mvp = *state->projectionMatrix() * *matrix();
        gl->glUseProgram(_program);
        gl->glUniformMatrix4fv(_mvpLocation, 1, GL_FALSE, mvp.constData());
        gl->glUniformMatrix4fv(_texMatrixLocation, 1, GL_FALSE, _texMatrix);
        gl->glActiveTexture(GL_TEXTURE0);
        gl->glBindTexture(kTextureExternalOes, _texture);
        gl->glUniform1i(_samplerLocation, 0);
        gl->glBindBuffer(GL_ARRAY_BUFFER, 0);
        gl->glVertexAttribPointer(GLuint(_positionLocation), 2, GL_FLOAT, GL_FALSE, 0, positions);
        gl->glVertexAttribPointer(GLuint(_texCoordLocation), 2, GL_FLOAT, GL_FALSE, 0, texCoords);
        gl->glEnableVertexAttribArray(GLuint(_positionLocation));
        gl->glEnableVertexAttribArray(GLuint(_texCoordLocation));
        gl->glDisable(GL_BLEND);
        gl->glDisable(GL_DEPTH_TEST);
        gl->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        gl->glDisableVertexAttribArray(GLuint(_positionLocation));
        gl->glDisableVertexAttribArray(GLuint(_texCoordLocation));
        gl->glBindTexture(kTextureExternalOes, 0);
        gl->glUseProgram(0);
    }

private:
    static GLuint _compile(QOpenGLFunctions* gl, GLenum type, const char* source)
    {
        const GLuint shader = gl->glCreateShader(type);
        gl->glShaderSource(shader, 1, &source, nullptr);
        gl->glCompileShader(shader);
        GLint ok = GL_FALSE;
        gl->glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[512] = {};
            gl->glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
            qCWarning(DhakshaNativeVideoLog) << "Shader compile failed:" << log;
            gl->glDeleteShader(shader);
            return 0;
        }
        return shader;
    }

    bool _initGl(QOpenGLFunctions* gl)
    {
        const GLuint vertex = _compile(gl, GL_VERTEX_SHADER, kVertexShader);
        const GLuint fragment = _compile(gl, GL_FRAGMENT_SHADER, kFragmentShader);
        if (!vertex || !fragment) {
            return false;
        }
        const GLuint program = gl->glCreateProgram();
        gl->glAttachShader(program, vertex);
        gl->glAttachShader(program, fragment);
        gl->glLinkProgram(program);
        gl->glDeleteShader(vertex);
        gl->glDeleteShader(fragment);
        GLint ok = GL_FALSE;
        gl->glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (!ok) {
            qCWarning(DhakshaNativeVideoLog) << "Shader link failed";
            gl->glDeleteProgram(program);
            return false;
        }
        _program = program;
        _positionLocation = gl->glGetAttribLocation(program, "aPosition");
        _texCoordLocation = gl->glGetAttribLocation(program, "aTexCoord");
        _mvpLocation = gl->glGetUniformLocation(program, "uMvp");
        _texMatrixLocation = gl->glGetUniformLocation(program, "uTexMatrix");
        _samplerLocation = gl->glGetUniformLocation(program, "uTexture");

        gl->glGenTextures(1, &_texture);
        gl->glBindTexture(kTextureExternalOes, _texture);
        gl->glTexParameteri(kTextureExternalOes, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl->glTexParameteri(kTextureExternalOes, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        gl->glTexParameteri(kTextureExternalOes, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl->glTexParameteri(kTextureExternalOes, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl->glBindTexture(kTextureExternalOes, 0);
        return true;
    }

    bool _createSurface()
    {
        QAndroidJniEnvironment env;
        _surfaceTexture = QAndroidJniObject("android/graphics/SurfaceTexture", "(I)V", jint(_texture));
        if (clearJniException(env) || !_surfaceTexture.isValid()) {
            qCWarning(DhakshaNativeVideoLog) << "Could not create the video SurfaceTexture";
            _surfaceTexture = QAndroidJniObject();
            return false;
        }
        _surface = QAndroidJniObject("android/view/Surface", "(Landroid/graphics/SurfaceTexture;)V",
                              _surfaceTexture.object<jobject>());
        if (clearJniException(env) || !_surface.isValid()) {
            qCWarning(DhakshaNativeVideoLog) << "Could not create the video Surface";
            _releaseSurface();
            return false;
        }
        _window = ANativeWindow_fromSurface(static_cast<JNIEnv*>(env), _surface.object<jobject>());
        if (!_window) {
            _releaseSurface();
            return false;
        }
        _decoder->setOutputWindow(_window);
        return true;
    }

    bool _latchNewest()
    {
        QAndroidJniEnvironment env;
        bool latched = false;
        for (int i = 0; i < kMaxLatchesPerRender; ++i) {
            _surfaceTexture.callMethod<void>("updateTexImage");
            if (clearJniException(env)) {
                break;
            }
            const jlong timestamp = _surfaceTexture.callMethod<jlong>("getTimestamp");
            if (clearJniException(env) || (timestamp == _latchedTimestampNs)) {
                break;
            }
            _latchedTimestampNs = timestamp;
            latched = true;
        }
        if (!latched) {
            return false;
        }
        jfloatArray matrix = env->NewFloatArray(16);
        _surfaceTexture.callMethod<void>("getTransformMatrix", "([F)V", matrix);
        if (!clearJniException(env)) {
            env->GetFloatArrayRegion(matrix, 0, 16, _texMatrix);
            _hasImage = true;
        }
        env->DeleteLocalRef(matrix);
        return _hasImage;
    }

    void _releaseSurface()
    {
        if (_window) {
            _decoder->setOutputWindow(nullptr);
            ANativeWindow_release(_window);
            _window = nullptr;
        }
        if (_surface.isValid()) {
            _surface.callMethod<void>("release");
            _surface = QAndroidJniObject();
        }
        if (_surfaceTexture.isValid()) {
            _surfaceTexture.callMethod<void>("release");
            _surfaceTexture = QAndroidJniObject();
        }
        QAndroidJniEnvironment env;
        (void) clearJniException(env);
        _hasImage = false;
        _latchedTimestampNs = 0;
    }

    std::shared_ptr<DhakshaDecoderCore> _decoder;
    QRectF _rect;
    bool _cropToFill = false;
    GLuint _program = 0;
    GLuint _texture = 0;
    GLint _positionLocation = -1;
    GLint _texCoordLocation = -1;
    GLint _mvpLocation = -1;
    GLint _texMatrixLocation = -1;
    GLint _samplerLocation = -1;
    QAndroidJniObject _surfaceTexture;
    QAndroidJniObject _surface;
    ANativeWindow* _window = nullptr;
    QQuickWindow* _quickWindow = nullptr;
    qint64 _latchedTimestampNs = 0;
    qint64 _releasedTimestampNs = 0;
    int _latchRetries = 0;
    bool _hasImage = false;
    GLfloat _texMatrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
};

}  // namespace

#endif  // DHAKSHA_NATIVE_ITEM

DhakshaVideoItem::DhakshaVideoItem(QQuickItem* parent) : QQuickItem(parent), _player(new DhakshaNativePlayer(this))
{
    setFlag(ItemHasContents, true);

    _restartTimer.setSingleShot(true);
    _restartTimer.setInterval(0);
    (void) connect(&_restartTimer, &QTimer::timeout, this, &DhakshaVideoItem::restart);

    _statusTimer.setInterval(1000);
    (void) connect(&_statusTimer, &QTimer::timeout, this, &DhakshaVideoItem::_updateStatus);

    // Redraw as soon as the decoder has drawn a frame; at most one redraw request waits at a time. While
    // frames keep coming the render thread also asks for the next redraw itself (DhakshaVideoNode::render)
    _player->decoder()->setFrameCallback([this]() {
        if (!_updateQueued.exchange(true)) {
            QMetaObject::invokeMethod(
                this,
                [this]() {
                    _updateQueued.store(false);
                    update();
                },
                Qt::QueuedConnection);
        }
    });
}

DhakshaVideoItem::~DhakshaVideoItem()
{
    _player->decoder()->setFrameCallback(nullptr);
    _player->stop();
}

bool DhakshaVideoItem::supported() const
{
    return DhakshaNativePlayer::supported();
}

void DhakshaVideoItem::setSource(const QString& source)
{
    if (source != _source) {
        _source = source;
        emit sourceChanged();
        _scheduleRestart();
    }
}

void DhakshaVideoItem::setTcp(bool tcp)
{
    if (tcp != _tcp) {
        _tcp = tcp;
        emit tcpChanged();
        _scheduleRestart();
    }
}

void DhakshaVideoItem::setCropToFill(bool cropToFill)
{
    if (cropToFill != _cropToFill) {
        _cropToFill = cropToFill;
        emit cropToFillChanged();
        update();
    }
}

void DhakshaVideoItem::restart()
{
    _restartTimer.stop();
    if (isVisible() && window() && !_source.isEmpty() && supported()) {
        _player->start(_source, _tcp);
        DhakshaFlightLog::write(QStringLiteral("Video: fast player started for %1 over %2").arg(_source, _tcp ? QStringLiteral("TCP") : QStringLiteral("UDP")));
        _statusTicks = 0;
        _statusTimer.start();
        _sinceStatus.start();
    } else {
        if (_player->running()) {
            DhakshaFlightLog::write(QStringLiteral("Video: fast player stopped"));
        }
        _player->stop();
        _statusTimer.stop();
    }
    _lastFrames = _player->decoder()->framesRendered();
    if (_playing) {
        _playing = false;
        emit playingChanged();
    }
}

void DhakshaVideoItem::_scheduleRestart()
{
    _restartTimer.start();
}

void DhakshaVideoItem::_updateStatus()
{
    const std::shared_ptr<DhakshaDecoderCore> decoder = _player->decoder();
    const quint64 frames = decoder->framesRendered();
    const bool playing = frames != _lastFrames;
    const qint64 elapsedMs = _sinceStatus.restart();
    const int fps = (elapsedMs > 0)
                        ? static_cast<int>(qRound64(static_cast<double>(frames - _lastFrames) * 1000.0 / elapsedMs))
                        : 0;
    _lastFrames = frames;
    const bool connected = _player->connected();
    const int dropped = static_cast<int>(qMin<quint64>(decoder->framesDropped(), std::numeric_limits<int>::max()));
    const QString codec = _player->codecName();
    const QString transport = _player->transportName();
    if ((connected != _connected) || (fps != _fps) || (dropped != _droppedFrames) || (codec != _codecName) ||
        (transport != _transportName)) {
        _connected = connected;
        _fps = fps;
        _droppedFrames = dropped;
        _codecName = codec;
        _transportName = transport;
        emit statsChanged();
    }
    if (playing != _playing) {
        _playing = playing;
        emit playingChanged();
        DhakshaFlightLog::write(playing ? QStringLiteral("Video: picture running") : QStringLiteral("Video: picture STOPPED"));
    }
    // Every 5 s while playing: the numbers needed to line a video problem up with the flight
    if ((++_statusTicks % 5) == 0) {
        const QSize videoSize = decoder->videoSize();
        DhakshaFlightLog::write(QStringLiteral("Video stats: %1 %2 %3 %4x%5, %6 fps, rendered %7, dropped %8, lost packets %9, decoder %10")
                                    .arg(connected ? QStringLiteral("connected") : QStringLiteral("NOT connected"), transport, codec)
                                    .arg(videoSize.width())
                                    .arg(videoSize.height())
                                    .arg(fps)
                                    .arg(frames)
                                    .arg(decoder->framesDropped())
                                    .arg(_player->packetsLost())
                                    .arg(decoder->decoderName().isEmpty() ? QStringLiteral("none") : decoder->decoderName()));
    }
    const QSize size = decoder->videoSize();
    const QString name = decoder->decoderName();
    if ((size != _videoSize) || (name != _decoderName)) {
        _videoSize = size;
        _decoderName = name;
        emit videoSizeChanged();
    }
}

void DhakshaVideoItem::itemChange(ItemChange change, const ItemChangeData& value)
{
    QQuickItem::itemChange(change, value);
    if ((change == ItemVisibleHasChanged) || (change == ItemSceneChange)) {
        _scheduleRestart();
    }
}

QSGNode* DhakshaVideoItem::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData* data)
{
    Q_UNUSED(data);
#ifdef DHAKSHA_NATIVE_ITEM
    auto* node = static_cast<DhakshaVideoNode*>(oldNode);
    if (!node) {
        if (!window() || (window()->rendererInterface()->graphicsApi() != QSGRendererInterface::OpenGL)) {
            return nullptr;
        }
        node = new DhakshaVideoNode(_player->decoder());
    }
    node->sync(QRectF(0, 0, width(), height()), _cropToFill, window());
    node->markDirty(QSGNode::DirtyMaterial);
    return node;
#else
    delete oldNode;
    return nullptr;
#endif
}
