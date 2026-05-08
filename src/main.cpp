#include <QApplication>
#include <QBuffer>
#include <QByteArray>
#include <QColor>
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
#include <QList>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QProcess>
#include <QScreen>
#include <QThread>
#include <QVariantMap>
#include <QWidget>
#include <QWindow>

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
    QColor borderColor;
};

struct FrozenScreen {
    QRect geometry;
    QImage image;
};

struct Selection {
    QRect globalRect;
    QList<FrozenScreen> frozenScreens;
};

class SelectorWindow;

struct SelectionState {
    QPoint startGlobal;
    QPoint currentGlobal;
    QList<SelectorWindow *> windows;
    bool selecting = false;
    bool accepted = false;
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
    SelectorWindow(QScreen *screen, QImage frozenBackground, QColor borderColor, SelectionState *selectionState)
        : QWidget(nullptr)
        , m_screenGeometry(screen ? screen->geometry() : QRect())
        , m_frozenBackground(std::move(frozenBackground))
        , m_borderColor(std::move(borderColor))
        , m_selectionState(selectionState)
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

    FrozenScreen frozenScreen() const
    {
        return FrozenScreen{m_screenGeometry, m_frozenBackground};
    }

    void showSelector()
    {
        if (QScreen *targetScreen = screen()) {
            setScreen(targetScreen);
            setGeometry(targetScreen->geometry());
            winId();
            if (QWindow *window = windowHandle()) {
                window->setScreen(targetScreen);
            }
        }

        showFullScreen();
        raise();
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

        const QRect selection = globalSelectionRect().intersected(m_screenGeometry);
        if (!selection.isNull()) {
            const QRect localSelection = globalToLocal(selection);
            if (!m_frozenBackground.isNull()) {
                painter.drawImage(localSelection, m_frozenBackground, localSelection);
            } else {
                painter.setCompositionMode(QPainter::CompositionMode_Clear);
                painter.fillRect(localSelection, Qt::transparent);
                painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
            }

            QPen pen(m_borderColor, 2);
            pen.setCosmetic(true);
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(localSelection.adjusted(1, 1, -2, -2));
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) {
            return;
        }

        m_selectionState->selecting = true;
        m_selectionState->accepted = false;
        m_selectionState->startGlobal = localToGlobal(event->position().toPoint());
        m_selectionState->currentGlobal = m_selectionState->startGlobal;
        updateSelectors();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!m_selectionState->selecting) {
            return;
        }

        m_selectionState->currentGlobal = localToGlobal(event->position().toPoint());
        updateSelectors();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (!m_selectionState->selecting || event->button() != Qt::LeftButton) {
            return;
        }

        m_selectionState->currentGlobal = localToGlobal(event->position().toPoint());
        const QRect finalSelection = QRect(m_selectionState->startGlobal, m_selectionState->currentGlobal)
                                         .normalized()
                                         .adjusted(1, 1, -1, -1);
        m_selectionState->selecting = false;
        m_selectionState->accepted = finalSelection.width() > 0 && finalSelection.height() > 0;
        hide();
        qApp->quit();
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Escape) {
            m_selectionState->accepted = false;
            hide();
            qApp->quit();
            return;
        }
    }

private:
    QPoint localToGlobal(const QPoint &point) const
    {
        return m_screenGeometry.topLeft() + point;
    }

    QRect globalToLocal(const QRect &rect) const
    {
        return rect.translated(-m_screenGeometry.topLeft());
    }

    QRect globalSelectionRect() const
    {
        if (!m_selectionState || (!m_selectionState->selecting && !m_selectionState->accepted)) {
            return {};
        }

        QRect selection(m_selectionState->startGlobal, m_selectionState->currentGlobal);
        selection = selection.normalized().adjusted(1, 1, -1, -1);
        if (selection.width() <= 0 || selection.height() <= 0) {
            return {};
        }

        return selection;
    }

    void updateSelectors()
    {
        for (SelectorWindow *window : m_selectionState->windows) {
            window->update();
        }
    }

    QRect m_screenGeometry;
    QImage m_frozenBackground;
    QColor m_borderColor;
    SelectionState *m_selectionState = nullptr;
};

static QImage cropFrozenSelection(const Selection &selection)
{
    if (selection.globalRect.isNull() || selection.frozenScreens.isEmpty()) {
        return {};
    }

    QImage image(selection.globalRect.size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);

    QPainter painter(&image);
    bool painted = false;
    for (const FrozenScreen &screen : selection.frozenScreens) {
        if (screen.geometry.isEmpty()) {
            continue;
        }

        const QRect intersection = selection.globalRect.intersected(screen.geometry);
        if (intersection.isNull()) {
            continue;
        }

        if (screen.image.isNull()) {
            return {};
        }

        const double scaleX = double(screen.image.width()) / double(screen.geometry.width());
        const double scaleY = double(screen.image.height()) / double(screen.geometry.height());

        QRect sourceRect(
            int((intersection.x() - screen.geometry.x()) * scaleX),
            int((intersection.y() - screen.geometry.y()) * scaleY),
            int(intersection.width() * scaleX),
            int(intersection.height() * scaleY));
        sourceRect = sourceRect.intersected(screen.image.rect());
        if (sourceRect.isNull()) {
            continue;
        }

        const QRect targetRect(intersection.topLeft() - selection.globalRect.topLeft(), intersection.size());
        painter.drawImage(targetRect, screen.image, sourceRect);
        painted = true;
    }

    if (!painted) {
        return {};
    }

    return image;
}

static Selection selectRegion(bool freeze, const QColor &borderColor, bool debug)
{
    SelectionState selectionState;
    QList<SelectorWindow *> selectors;
    const QList<QScreen *> screens = QGuiApplication::screens();

    for (QScreen *screen : screens) {
        if (debug && screen) {
            const QRect geometry = screen->geometry();
            std::fprintf(stderr,
                         "kwinshot: selector screen=%s geometry x=%d y=%d width=%d height=%d\n",
                         qPrintable(screen->name()),
                         geometry.x(),
                         geometry.y(),
                         geometry.width(),
                         geometry.height());
        }

        QImage background;
        if (freeze && screen) {
            background = captureScreen(screen->name(), debug);
        }

        auto *selector = new SelectorWindow(screen, background, borderColor, &selectionState);
        selectors.append(selector);
        selectionState.windows.append(selector);
        selector->showSelector();
    }

    if (selectors.isEmpty()) {
        return {};
    }

    QScreen *cursorScreen = QGuiApplication::screenAt(QCursor::pos());
    SelectorWindow *focusSelector = selectors.first();
    for (SelectorWindow *selector : selectors) {
        if (cursorScreen && selector->screen() == cursorScreen) {
            focusSelector = selector;
            break;
        }
    }
    focusSelector->activateWindow();
    focusSelector->setFocus();

    qApp->exec();

    Selection selection;
    if (selectionState.accepted) {
        selection.globalRect = QRect(selectionState.startGlobal, selectionState.currentGlobal).normalized().adjusted(1, 1, -1, -1);
        for (SelectorWindow *selector : selectors) {
            const FrozenScreen frozenScreen = selector->frozenScreen();
            selection.frozenScreens.append(frozenScreen);
        }
    }

    qDeleteAll(selectors);
    return selection;
}

static QColor defaultBorderColor(const QApplication &app)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    const QColor accent = app.palette().color(QPalette::Accent);
    if (accent.isValid()) {
        return accent;
    }
#endif
    return app.palette().color(QPalette::Highlight);
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
    QCommandLineOption borderColorOption(QStringLiteral("border-color"),
                                         QStringLiteral("Selection border color, for example '#3daee9' or 'red'."),
                                         QStringLiteral("color"));
    QCommandLineOption debugOption(QStringLiteral("debug"), QStringLiteral("Print debug information."));

    parser.addOption(fileOption);
    parser.addOption(stdoutOption);
    parser.addOption(clipboardOption);
    parser.addOption(noFreezeOption);
    parser.addOption(delayOption);
    parser.addOption(borderColorOption);
    parser.addOption(debugOption);
    parser.addPositionalArgument(QStringLiteral("target"), QStringLiteral("region, active-window, or fullscreen."));
    parser.process(app);

    Config config;
    config.borderColor = defaultBorderColor(app);
    config.freeze = !parser.isSet(noFreezeOption);
    config.debug = parser.isSet(debugOption) || qEnvironmentVariableIsSet("KWINSHOT_DEBUG");

    if (parser.isSet(borderColorOption)) {
        const QColor color(parser.value(borderColorOption));
        if (!color.isValid()) {
            parser.showMessageAndExit(QCommandLineParser::MessageType::Error,
                                      QStringLiteral("Invalid --border-color value: %1").arg(parser.value(borderColorOption)),
                                      1);
        }
        config.borderColor = color;
    }

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
        const Selection selection = selectRegion(config.freeze, config.borderColor, config.debug);
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
