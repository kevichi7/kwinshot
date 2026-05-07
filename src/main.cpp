#include <QApplication>
#include <QBuffer>
#include <QByteArray>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCursor>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QProcess>
#include <QScreen>
#include <QThread>
#include <QVariantMap>
#include <QWidget>

#include <unistd.h>

#include <cstdio>

enum class Target {
    Region,
    ActiveWindow,
    Fullscreen,
};

enum class Output {
    Clipboard,
    File,
    Stdout,
};

struct Config {
    Target target = Target::Region;
    Output output = Output::Clipboard;
    QString filePath;
    bool freeze = true;
    bool debug = false;
    int delayMs = 40;
};

struct Selection {
    QRect globalRect;
    QRect localRect;
    QSize screenSize;
    QImage frozenBackground;
};

static bool readExact(int fd, QByteArray &data, qsizetype size)
{
    data.resize(size);
    qsizetype offset = 0;

    while (offset < size) {
        const ssize_t n = read(fd, data.data() + offset, size - offset);
        if (n <= 0) {
            return false;
        }
        offset += n;
    }

    return true;
}

static QImage imageFromKWinResult(const QVariantMap &results, int readFd, bool debug, const char *context)
{
    const int width = results.value(QStringLiteral("width")).toUInt();
    const int height = results.value(QStringLiteral("height")).toUInt();
    const int stride = results.value(QStringLiteral("stride")).toUInt();
    const auto format = static_cast<QImage::Format>(results.value(QStringLiteral("format")).toUInt());

    if (debug) {
        std::fprintf(stderr, "kwinshot: %s result type=%s width=%d height=%d stride=%d format=%d\n",
                     context,
                     qPrintable(results.value(QStringLiteral("type")).toString()),
                     width,
                     height,
                     stride,
                     int(format));
    }

    if (width <= 0 || height <= 0 || stride <= 0) {
        std::fprintf(stderr, "kwinshot: invalid image metadata\n");
        return {};
    }

    QByteArray raw;
    if (!readExact(readFd, raw, qsizetype(stride) * height)) {
        std::fprintf(stderr, "kwinshot: failed to read screenshot bytes\n");
        return {};
    }

    QImage image(reinterpret_cast<const uchar *>(raw.constData()), width, height, stride, format);
    return image.copy();
}

static QDBusInterface screenshotInterface()
{
    return QDBusInterface(
        QStringLiteral("org.kde.KWin"),
        QStringLiteral("/org/kde/KWin/ScreenShot2"),
        QStringLiteral("org.kde.KWin.ScreenShot2"),
        QDBusConnection::sessionBus());
}

template <typename Call>
static QImage captureWithPipe(Call call, bool debug, const char *context)
{
    int pipeFds[2];
    if (pipe(pipeFds) != 0) {
        std::perror("kwinshot: pipe");
        return {};
    }

    QDBusReply<QVariantMap> reply = call(pipeFds[1]);
    close(pipeFds[1]);

    if (!reply.isValid()) {
        close(pipeFds[0]);
        std::fprintf(stderr, "kwinshot: %s: %s\n",
                     qPrintable(reply.error().name()),
                     qPrintable(reply.error().message()));
        return {};
    }

    QImage image = imageFromKWinResult(reply.value(), pipeFds[0], debug, context);
    close(pipeFds[0]);
    return image;
}

static QVariantMap captureOptions()
{
    QVariantMap options;
    options.insert(QStringLiteral("include-cursor"), false);
    options.insert(QStringLiteral("native-resolution"), false);
    return options;
}

static QImage captureArea(const QRect &region, bool debug)
{
    QDBusInterface iface = screenshotInterface();
    const QVariantMap options = captureOptions();

    return captureWithPipe([&](int writeFd) {
        return QDBusReply<QVariantMap>(iface.call(
            QStringLiteral("CaptureArea"),
            region.x(),
            region.y(),
            uint(region.width()),
            uint(region.height()),
            options,
            QVariant::fromValue(QDBusUnixFileDescriptor(writeFd))));
    }, debug, "area");
}

static QImage captureActiveWindow(bool debug)
{
    QDBusInterface iface = screenshotInterface();
    const QVariantMap options = captureOptions();

    return captureWithPipe([&](int writeFd) {
        return QDBusReply<QVariantMap>(iface.call(
            QStringLiteral("CaptureActiveWindow"),
            options,
            QVariant::fromValue(QDBusUnixFileDescriptor(writeFd))));
    }, debug, "active-window");
}

static QImage captureScreen(const QString &name, bool debug)
{
    QDBusInterface iface = screenshotInterface();
    const QVariantMap options = captureOptions();

    return captureWithPipe([&](int writeFd) {
        return QDBusReply<QVariantMap>(iface.call(
            QStringLiteral("CaptureScreen"),
            name,
            options,
            QVariant::fromValue(QDBusUnixFileDescriptor(writeFd))));
    }, debug, "screen");
}

static QByteArray imageToPng(const QImage &image)
{
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);

    if (!image.save(&buffer, "PNG")) {
        std::fprintf(stderr, "kwinshot: failed to encode PNG\n");
        return {};
    }

    return png;
}

static bool writeClipboard(const QByteArray &png)
{
    QProcess wlCopy;
    wlCopy.start(QStringLiteral("wl-copy"), {QStringLiteral("--type"), QStringLiteral("image/png")});

    if (!wlCopy.waitForStarted()) {
        std::fprintf(stderr, "kwinshot: failed to start wl-copy\n");
        return false;
    }

    wlCopy.write(png);
    wlCopy.closeWriteChannel();

    if (!wlCopy.waitForFinished() || wlCopy.exitStatus() != QProcess::NormalExit || wlCopy.exitCode() != 0) {
        std::fprintf(stderr, "kwinshot: wl-copy failed\n");
        return false;
    }

    return true;
}

static bool writeOutput(const QImage &image, const Config &config)
{
    const QByteArray png = imageToPng(image);
    if (png.isEmpty()) {
        return false;
    }

    if (config.output == Output::Clipboard) {
        return writeClipboard(png);
    }

    if (config.output == Output::Stdout) {
        QFile out;
        if (!out.open(stdout, QIODevice::WriteOnly)) {
            std::fprintf(stderr, "kwinshot: failed to open stdout\n");
            return false;
        }
        return out.write(png) == png.size();
    }

    QFile file(config.filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        std::fprintf(stderr, "kwinshot: failed to open output file: %s\n", qPrintable(config.filePath));
        return false;
    }

    return file.write(png) == png.size();
}

class SelectorWindow : public QWidget
{
public:
    SelectorWindow(QScreen *screen, QImage frozenBackground)
        : QWidget(nullptr)
        , m_screenGeometry(screen ? screen->geometry() : QRect())
        , m_frozenBackground(std::move(frozenBackground))
    {
        setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool);
        setAttribute(Qt::WA_TranslucentBackground);
        setCursor(Qt::CrossCursor);
        setFocusPolicy(Qt::StrongFocus);
        setMouseTracking(true);

        if (screen) {
            setScreen(screen);
            setGeometry(screen->geometry());
        }
    }

    QRect selectedGlobalRect() const
    {
        const QRect local = selectedLocalRect();
        if (local.isNull()) {
            return {};
        }

        return QRect(m_screenGeometry.topLeft() + local.topLeft(), local.size());
    }

    QRect selectedLocalRect() const
    {
        if (!m_accepted) {
            return {};
        }

        QRect local = m_selection.normalized();
        if (local.width() <= 0 || local.height() <= 0) {
            return {};
        }

        local = local.adjusted(1, 1, -1, -1);
        if (local.width() <= 0 || local.height() <= 0) {
            return {};
        }

        return local;
    }

    QSize screenSize() const
    {
        return m_screenGeometry.size();
    }

    QImage frozenBackground() const
    {
        return m_frozenBackground;
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, false);

        if (!m_frozenBackground.isNull()) {
            painter.drawImage(rect(), m_frozenBackground);
        } else {
            painter.setCompositionMode(QPainter::CompositionMode_Source);
            painter.fillRect(rect(), QColor(0, 0, 0, 80));
        }

        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        painter.fillRect(rect(), QColor(0, 0, 0, 100));

        const QRect selection = m_selection.normalized();
        if (!selection.isNull()) {
            if (!m_frozenBackground.isNull()) {
                painter.drawImage(selection, m_frozenBackground, selection);
            } else {
                painter.setCompositionMode(QPainter::CompositionMode_Clear);
                painter.fillRect(selection, Qt::transparent);
                painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
            }

            QPen pen(QColor(125, 211, 252, 255), 2);
            pen.setCosmetic(true);
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(selection.adjusted(1, 1, -2, -2));
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) {
            return;
        }

        m_selecting = true;
        m_start = event->position().toPoint();
        m_selection = QRect(m_start, m_start);
        update();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!m_selecting) {
            return;
        }

        m_selection = QRect(m_start, event->position().toPoint()).normalized();
        update();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (!m_selecting || event->button() != Qt::LeftButton) {
            return;
        }

        m_selection = QRect(m_start, event->position().toPoint()).normalized();
        m_selecting = false;
        m_accepted = true;
        hide();
        qApp->quit();
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Escape) {
            m_accepted = false;
            hide();
            qApp->quit();
            return;
        }
    }

private:
    QRect m_screenGeometry;
    QImage m_frozenBackground;
    QPoint m_start;
    QRect m_selection;
    bool m_selecting = false;
    bool m_accepted = false;
};

static QImage cropFrozenSelection(const Selection &selection)
{
    if (selection.frozenBackground.isNull() || selection.localRect.isNull() || selection.screenSize.isEmpty()) {
        return {};
    }

    const QImage &background = selection.frozenBackground;
    const double scaleX = double(background.width()) / double(selection.screenSize.width());
    const double scaleY = double(background.height()) / double(selection.screenSize.height());

    QRect sourceRect(
        int(selection.localRect.x() * scaleX),
        int(selection.localRect.y() * scaleY),
        int(selection.localRect.width() * scaleX),
        int(selection.localRect.height() * scaleY));
    sourceRect = sourceRect.intersected(background.rect());

    if (sourceRect.isNull()) {
        return {};
    }

    return background.copy(sourceRect);
}

static Selection selectRegion(QScreen *screen, bool freeze, bool debug)
{
    QImage background;
    if (freeze && screen) {
        background = captureScreen(screen->name(), debug);
    }

    SelectorWindow selector(screen, background);
    selector.showFullScreen();
    selector.raise();
    selector.activateWindow();
    selector.setFocus();

    qApp->exec();
    Selection selection;
    selection.globalRect = selector.selectedGlobalRect();
    selection.localRect = selector.selectedLocalRect();
    selection.screenSize = selector.screenSize();
    selection.frozenBackground = selector.frozenBackground();
    return selection;
}

static Config parseConfig(QApplication &app)
{
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Fast KWin-native screenshots."));
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption fileOption(QStringList{QStringLiteral("f"), QStringLiteral("file")},
                                  QStringLiteral("Write PNG to file."),
                                  QStringLiteral("path"));
    QCommandLineOption stdoutOption(QStringLiteral("stdout"), QStringLiteral("Write PNG to stdout."));
    QCommandLineOption clipboardOption(QStringLiteral("clipboard"), QStringLiteral("Copy PNG to clipboard."));
    QCommandLineOption noFreezeOption(QStringLiteral("no-freeze"),
                                       QStringLiteral("Select and capture the live desktop instead of the frozen frame."));
    QCommandLineOption delayOption(QStringLiteral("delay-ms"),
                                   QStringLiteral("Delay after selector closes before capture."),
                                   QStringLiteral("ms"));
    QCommandLineOption debugOption(QStringLiteral("debug"), QStringLiteral("Print debug information."));

    parser.addOption(fileOption);
    parser.addOption(stdoutOption);
    parser.addOption(clipboardOption);
    parser.addOption(noFreezeOption);
    parser.addOption(delayOption);
    parser.addOption(debugOption);
    parser.addPositionalArgument(QStringLiteral("target"), QStringLiteral("region, active-window, or fullscreen."));
    parser.process(app);

    Config config;
    config.freeze = !parser.isSet(noFreezeOption);
    config.debug = parser.isSet(debugOption) || qEnvironmentVariableIsSet("KWINSHOT_DEBUG");

    if (parser.isSet(delayOption)) {
        bool ok = false;
        const int value = parser.value(delayOption).toInt(&ok);
        if (ok && value >= 0) {
            config.delayMs = value;
        }
    }

    const QStringList positional = parser.positionalArguments();
    const QString target = positional.isEmpty() ? QStringLiteral("region") : positional.first();
    if (target == QStringLiteral("active-window")) {
        config.target = Target::ActiveWindow;
    } else if (target == QStringLiteral("fullscreen") || target == QStringLiteral("full-screen")) {
        config.target = Target::Fullscreen;
    } else {
        config.target = Target::Region;
    }

    if (parser.isSet(fileOption)) {
        config.output = Output::File;
        config.filePath = parser.value(fileOption);
    } else if (parser.isSet(stdoutOption)) {
        config.output = Output::Stdout;
    } else {
        config.output = Output::Clipboard;
    }

    return config;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("kwinshot"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    const Config config = parseConfig(app);

    QScreen *screen = QGuiApplication::screenAt(QCursor::pos());
    if (!screen) {
        screen = QGuiApplication::primaryScreen();
    }

    QImage image;
    if (config.target == Target::ActiveWindow) {
        image = captureActiveWindow(config.debug);
    } else if (config.target == Target::Fullscreen) {
        image = captureScreen(screen ? screen->name() : QString(), config.debug);
    } else {
        const Selection selection = selectRegion(screen, config.freeze, config.debug);
        if (selection.globalRect.isNull()) {
            return 0;
        }

        if (config.debug) {
            std::fprintf(stderr, "kwinshot: region x=%d y=%d width=%d height=%d\n",
                         selection.globalRect.x(),
                         selection.globalRect.y(),
                         selection.globalRect.width(),
                         selection.globalRect.height());
        }

        if (config.freeze) {
            image = cropFrozenSelection(selection);
        }

        if (image.isNull()) {
            QCoreApplication::processEvents();
            QThread::msleep(uint(config.delayMs));
            QCoreApplication::processEvents();
            image = captureArea(selection.globalRect, config.debug);
        }
    }

    if (image.isNull()) {
        return 1;
    }

    return writeOutput(image, config) ? 0 : 1;
}
