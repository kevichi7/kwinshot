#include <QApplication>
#include <QBuffer>
#include <QByteArray>
#include <QColor>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCursor>
#include <QDateTime>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusUnixFileDescriptor>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QGuiApplication>
#include <QIcon>
#include <QImage>
#include <QKeyEvent>
#include <QList>
#include <QLockFile>
#include <QMouseEvent>
#include <QPainter>
#include <QPalette>
#include <QProcess>
#include <QSaveFile>
#include <QScreen>
#include <QStandardPaths>
#include <QThread>
#include <QVariantMap>
#include <QWidget>
#include <QWindow>

#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>

enum class Target {
    Region,
    ActiveWindow,
    Fullscreen,
};

enum class Output {
    Clipboard,
    File,
    Stdout,
    Autosave,
    SaveDialog,
};

enum class SelectionAction {
    None,
    Clipboard,
    Save,
};

struct Config {
    Target target = Target::Region;
    Output output = Output::Clipboard;
    QString filePath;
    bool copyToClipboard = false;
    bool freeze = true;
    bool debug = false;
    bool chooseOutput = true;
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
    SelectionAction action = SelectionAction::None;
};

class SelectorWindow;

struct ActionButton {
    QRect rect;
    QString label;
    QString iconName;
    SelectionAction action = SelectionAction::None;
};

struct SelectionState {
    QPoint startGlobal;
    QPoint currentGlobal;
    QList<SelectorWindow *> windows;
    bool chooseOutput = true;
    bool selecting = false;
    bool awaitingAction = false;
    bool accepted = false;
    SelectionAction action = SelectionAction::None;
};

static bool readExact(int fd, uchar *data, qsizetype size)
{
    qsizetype offset = 0;

    while (offset < size) {
        const ssize_t n = read(fd, data + offset, size - offset);
        if (n < 0 && errno == EINTR) {
            continue;
        }
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

    QImage image(width, height, format);
    if (image.isNull()) {
        std::fprintf(stderr, "kwinshot: failed to allocate screenshot image\n");
        return {};
    }

    const qsizetype sourceStride = stride;
    const qsizetype targetStride = image.bytesPerLine();
    if (sourceStride == targetStride) {
        if (!readExact(readFd, image.bits(), targetStride * height)) {
            std::fprintf(stderr, "kwinshot: failed to read screenshot bytes\n");
            return {};
        }
        return image;
    }

    if (debug) {
        std::fprintf(stderr,
                     "kwinshot: stride mismatch source=%lld target=%lld, reading row by row\n",
                     static_cast<long long>(sourceStride),
                     static_cast<long long>(targetStride));
    }

    QByteArray row;
    row.resize(sourceStride);
    const qsizetype copyBytes = qMin(sourceStride, targetStride);
    for (int y = 0; y < height; ++y) {
        if (!readExact(readFd, reinterpret_cast<uchar *>(row.data()), sourceStride)) {
            std::fprintf(stderr, "kwinshot: failed to read screenshot bytes\n");
            return {};
        }

        uchar *targetRow = image.scanLine(y);
        std::memcpy(targetRow, row.constData(), size_t(copyBytes));
        if (copyBytes < targetStride) {
            std::memset(targetRow + copyBytes, 0, size_t(targetStride - copyBytes));
        }
    }

    return image;
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

static QString autosavePath();

static bool writeClipboard(const QByteArray &png)
{
    QProcess wlCopy;
    wlCopy.start(QStringLiteral("wl-copy"), {QStringLiteral("--type"), QStringLiteral("image/png")});

    if (!wlCopy.waitForStarted()) {
        std::fprintf(stderr, "kwinshot: failed to start wl-copy\n");
        return false;
    }

    qsizetype offset = 0;
    while (offset < png.size()) {
        const qint64 written = wlCopy.write(png.constData() + offset, png.size() - offset);
        if (written < 0) {
            std::fprintf(stderr, "kwinshot: failed to write to wl-copy: %s\n", qPrintable(wlCopy.errorString()));
            return false;
        }
        if (written == 0) {
            if (!wlCopy.waitForBytesWritten()) {
                std::fprintf(stderr, "kwinshot: timed out writing to wl-copy: %s\n", qPrintable(wlCopy.errorString()));
                return false;
            }
            continue;
        }
        offset += written;
    }

    wlCopy.closeWriteChannel();

    if (!wlCopy.waitForFinished() || wlCopy.exitStatus() != QProcess::NormalExit || wlCopy.exitCode() != 0) {
        const QString error = QString::fromLocal8Bit(wlCopy.readAllStandardError()).trimmed();
        if (error.isEmpty()) {
            std::fprintf(stderr, "kwinshot: wl-copy failed\n");
        } else {
            std::fprintf(stderr, "kwinshot: wl-copy failed: %s\n", qPrintable(error));
        }
        return false;
    }

    return true;
}

static bool writeOutput(const QImage &image, const Config &config)
{
    if (config.output == Output::Clipboard) {
        const QByteArray png = imageToPng(image);
        if (png.isEmpty()) {
            return false;
        }
        return writeClipboard(png);
    }

    if (config.output == Output::Stdout) {
        QFile out;
        if (!out.open(stdout, QIODevice::WriteOnly)) {
            std::fprintf(stderr, "kwinshot: failed to open stdout\n");
            return false;
        }
        if (!image.save(&out, "PNG")) {
            std::fprintf(stderr, "kwinshot: failed to write PNG to stdout: %s\n", qPrintable(out.errorString()));
            return false;
        }
        return true;
    }

    if (config.output == Output::Autosave) {
        const QString filePath = autosavePath();
        if (filePath.isEmpty()) {
            return false;
        }
        Config fileConfig = config;
        fileConfig.output = Output::File;
        fileConfig.filePath = filePath;
        return writeOutput(image, fileConfig);
    }

    QSaveFile file(config.filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        std::fprintf(stderr, "kwinshot: failed to open output file: %s\n", qPrintable(config.filePath));
        return false;
    }

    if (!image.save(&file, "PNG")) {
        std::fprintf(stderr, "kwinshot: failed to write screenshot: %s\n", qPrintable(file.errorString()));
        return false;
    }

    if (!file.commit()) {
        std::fprintf(stderr, "kwinshot: failed to finalize output file: %s\n", qPrintable(file.errorString()));
        return false;
    }

    if (config.copyToClipboard) {
        const QByteArray png = imageToPng(image);
        if (png.isEmpty() || !writeClipboard(png)) {
            std::fprintf(stderr, "kwinshot: saved screenshot, but failed to copy it to clipboard\n");
            return false;
        }
    }

    return true;
}

static QString autosavePath()
{
    QString picturesPath = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    if (picturesPath.isEmpty()) {
        picturesPath = QDir::homePath() + QStringLiteral("/Pictures");
    }

    QDir screenshotsDir(picturesPath + QStringLiteral("/Screenshots"));
    if (!screenshotsDir.exists() && !screenshotsDir.mkpath(QStringLiteral("."))) {
        std::fprintf(stderr, "kwinshot: failed to create screenshots directory: %s\n", qPrintable(screenshotsDir.path()));
        return {};
    }

    const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));
    const QString baseName = QStringLiteral("kwinshot_%1").arg(stamp);
    QString filePath = screenshotsDir.filePath(baseName + QStringLiteral(".png"));
    for (int suffix = 1; QFile::exists(filePath); ++suffix) {
        filePath = screenshotsDir.filePath(QStringLiteral("%1_%2.png").arg(baseName).arg(suffix));
    }

    return filePath;
}

static bool saveImageWithDialog(const QImage &image)
{
    const QString filePath = QFileDialog::getSaveFileName(
        nullptr,
        QStringLiteral("Save Screenshot"),
        QDir::homePath() + QStringLiteral("/Pictures/kwinshot.png"),
        QStringLiteral("PNG Images (*.png)"));
    if (filePath.isEmpty()) {
        return true;
    }

    const QByteArray png = imageToPng(image);
    if (png.isEmpty()) {
        return false;
    }

    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        std::fprintf(stderr, "kwinshot: failed to open output file: %s\n", qPrintable(filePath));
        return false;
    }

    if (file.write(png) != png.size()) {
        std::fprintf(stderr, "kwinshot: failed to write screenshot: %s\n", qPrintable(file.errorString()));
        return false;
    }

    if (!file.commit()) {
        std::fprintf(stderr, "kwinshot: failed to finalize output file: %s\n", qPrintable(file.errorString()));
        return false;
    }

    if (!writeClipboard(png)) {
        std::fprintf(stderr, "kwinshot: saved screenshot, but failed to copy it to clipboard\n");
    }

    return true;
}

static QString instanceLockPath()
{
    QString runtimePath = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (runtimePath.isEmpty()) {
        runtimePath = QDir::tempPath();
    }
    return QDir(runtimePath).filePath(QStringLiteral("kwinshot.lock"));
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
            QRectF borderRect(localSelection);
            borderRect = borderRect.intersected(QRectF(rect()));
            borderRect.adjust(1.0, 1.0, -1.0, -1.0);
            if (borderRect.width() > 0.0 && borderRect.height() > 0.0) {
                painter.drawRect(borderRect);
            }
        }

        drawActionButtons(painter);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) {
            hideSelectors();
            qApp->quit();
            return;
        }

        if (m_selectionState->awaitingAction) {
            const SelectionAction action = actionAt(event->position().toPoint());
            if (action != SelectionAction::None) {
                finishWithAction(action);
                return;
            }
        }

        m_selectionState->selecting = true;
        m_selectionState->accepted = false;
        m_selectionState->awaitingAction = false;
        m_selectionState->action = SelectionAction::None;
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
        if (finalSelection.width() <= 0 || finalSelection.height() <= 0) {
            m_selectionState->accepted = false;
            hideSelectors();
            qApp->quit();
            return;
        }

        if (!m_selectionState->chooseOutput) {
            m_selectionState->action = SelectionAction::Clipboard;
            m_selectionState->accepted = true;
            hideSelectors();
            qApp->quit();
            return;
        }

        m_selectionState->awaitingAction = true;
        updateSelectors();
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Escape) {
            m_selectionState->accepted = false;
            hideSelectors();
            qApp->quit();
            return;
        }

        if (!m_selectionState->awaitingAction) {
            return;
        }

        if (event->matches(QKeySequence::Copy)
            || event->key() == Qt::Key_Return
            || event->key() == Qt::Key_Enter) {
            finishWithAction(SelectionAction::Clipboard);
            return;
        }

        if (event->matches(QKeySequence::Save)) {
            finishWithAction(SelectionAction::Save);
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
        if (!m_selectionState || (!m_selectionState->selecting && !m_selectionState->awaitingAction && !m_selectionState->accepted)) {
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

    void hideSelectors()
    {
        for (SelectorWindow *window : m_selectionState->windows) {
            window->hide();
        }
    }

    void finishWithAction(SelectionAction action)
    {
        m_selectionState->action = action;
        m_selectionState->accepted = true;
        m_selectionState->awaitingAction = false;
        hideSelectors();
        qApp->quit();
    }

    QList<ActionButton> actionButtons() const
    {
        if (!m_selectionState->awaitingAction) {
            return {};
        }

        const QRect selection = globalSelectionRect().intersected(m_screenGeometry);
        if (selection.isNull()) {
            return {};
        }

        const QRect localSelection = globalToLocal(selection);
        const QFontMetrics metrics(qApp->font());
        const int height = qMax(34, metrics.height() + 16);
        const int buttonWidth = height;
        const int gap = 8;
        const int totalWidth = buttonWidth * 2 + gap;
        const int x = qBound(12, localSelection.center().x() - totalWidth / 2, qMax(12, width() - totalWidth - 12));
        int y = localSelection.bottom() + 12;
        if (y + height > this->height() - 12) {
            y = localSelection.top() - height - 12;
        }
        y = qBound(12, y, qMax(12, this->height() - height - 12));

        return {
            ActionButton{QRect(x, y, buttonWidth, height), QStringLiteral("Copy"), QStringLiteral("edit-copy"), SelectionAction::Clipboard},
            ActionButton{QRect(x + buttonWidth + gap, y, buttonWidth, height), QStringLiteral("Save"), QStringLiteral("document-save"), SelectionAction::Save},
        };
    }

    SelectionAction actionAt(const QPoint &point) const
    {
        for (const ActionButton &button : actionButtons()) {
            if (button.rect.contains(point)) {
                return button.action;
            }
        }

        return SelectionAction::None;
    }

    void drawActionButtons(QPainter &painter)
    {
        const QList<ActionButton> buttons = actionButtons();
        if (buttons.isEmpty()) {
            return;
        }

        QFont font = qApp->font();
        font.setBold(true);
        painter.setFont(font);
        painter.setRenderHint(QPainter::Antialiasing, true);

        for (const ActionButton &button : buttons) {
            QColor background = palette().color(QPalette::Window);
            background.setAlpha(230);
            QColor border = m_borderColor;
            border.setAlpha(240);

            painter.setPen(QPen(border, 1));
            painter.setBrush(background);
            painter.drawRoundedRect(button.rect, 6, 6);

            const QIcon icon = QIcon::fromTheme(button.iconName);
            if (!icon.isNull()) {
                icon.paint(&painter, button.rect.adjusted(8, 8, -8, -8), Qt::AlignCenter);
            } else {
                painter.setPen(palette().color(QPalette::WindowText));
                painter.drawText(button.rect, Qt::AlignCenter, button.label.left(1));
            }
        }

        painter.setRenderHint(QPainter::Antialiasing, false);
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

        const double sourceLeft = double(intersection.x() - screen.geometry.x()) * scaleX;
        const double sourceTop = double(intersection.y() - screen.geometry.y()) * scaleY;
        const double sourceRight = double(intersection.x() + intersection.width() - screen.geometry.x()) * scaleX;
        const double sourceBottom = double(intersection.y() + intersection.height() - screen.geometry.y()) * scaleY;
        QRect sourceRect(
            int(std::floor(sourceLeft)),
            int(std::floor(sourceTop)),
            int(std::ceil(sourceRight) - std::floor(sourceLeft)),
            int(std::ceil(sourceBottom) - std::floor(sourceTop)));
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

static Selection selectRegion(bool freeze, const QColor &borderColor, bool chooseOutput, bool debug)
{
    SelectionState selectionState;
    selectionState.chooseOutput = chooseOutput;
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
        selection.action = selectionState.action;
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
    QCommandLineOption autosaveOption(QStringLiteral("autosave"),
                                      QStringLiteral("Write PNG to ~/Pictures/Screenshots with a timestamped name."));
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
    parser.addOption(autosaveOption);
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
        if (!ok || value < 0) {
            parser.showMessageAndExit(QCommandLineParser::MessageType::Error,
                                      QStringLiteral("Invalid --delay-ms value: %1").arg(parser.value(delayOption)),
                                      1);
        }
        config.delayMs = value;
    }

    const QStringList positional = parser.positionalArguments();
    const QString target = positional.isEmpty() ? QStringLiteral("region") : positional.first();
    if (positional.size() > 1) {
        parser.showMessageAndExit(QCommandLineParser::MessageType::Error,
                                  QStringLiteral("Too many positional arguments."),
                                  1);
    }

    if (target == QStringLiteral("region")) {
        config.target = Target::Region;
    } else if (target == QStringLiteral("active-window")) {
        config.target = Target::ActiveWindow;
    } else if (target == QStringLiteral("fullscreen") || target == QStringLiteral("full-screen")) {
        config.target = Target::Fullscreen;
    } else {
        parser.showMessageAndExit(QCommandLineParser::MessageType::Error,
                                  QStringLiteral("Unknown target: %1").arg(target),
                                  1);
    }

    const int fileOutputOptions = int(parser.isSet(fileOption)) + int(parser.isSet(autosaveOption));
    if (fileOutputOptions > 1) {
        parser.showMessageAndExit(QCommandLineParser::MessageType::Error,
                                  QStringLiteral("Choose only one file output option."),
                                  1);
    }

    const int outputOptions = int(parser.isSet(fileOption))
        + int(parser.isSet(stdoutOption))
        + int(parser.isSet(autosaveOption));
    if (outputOptions > 1) {
        parser.showMessageAndExit(QCommandLineParser::MessageType::Error,
                                  QStringLiteral("Choose only one output option."),
                                  1);
    }

    if (parser.isSet(stdoutOption) && parser.isSet(clipboardOption)) {
        parser.showMessageAndExit(QCommandLineParser::MessageType::Error,
                                  QStringLiteral("--stdout cannot be combined with --clipboard."),
                                  1);
    }

    config.copyToClipboard = parser.isSet(clipboardOption);

    if (parser.isSet(fileOption)) {
        if (parser.value(fileOption).isEmpty()) {
            parser.showMessageAndExit(QCommandLineParser::MessageType::Error,
                                      QStringLiteral("--file requires a path."),
                                      1);
        }
        config.output = Output::File;
        config.chooseOutput = false;
        config.filePath = parser.value(fileOption);
    } else if (parser.isSet(stdoutOption)) {
        config.output = Output::Stdout;
        config.chooseOutput = false;
    } else if (parser.isSet(autosaveOption)) {
        config.output = Output::Autosave;
        config.chooseOutput = false;
    } else if (parser.isSet(clipboardOption)) {
        config.output = Output::Clipboard;
        config.chooseOutput = false;
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
    QGuiApplication::setDesktopFileName(QStringLiteral("net.local.kwinshot"));

    Config config = parseConfig(app);

    QLockFile instanceLock(instanceLockPath());
    if (!instanceLock.tryLock(0)) {
        return 0;
    }

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
        const Selection selection = selectRegion(config.freeze, config.borderColor, config.chooseOutput, config.debug);
        if (selection.globalRect.isNull()) {
            return 0;
        }

        if (config.chooseOutput) {
            if (selection.action == SelectionAction::Save) {
                config.output = Output::SaveDialog;
            } else {
                config.output = Output::Clipboard;
            }
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

        if (config.output == Output::SaveDialog) {
            if (image.isNull()) {
                return 1;
            }
            return saveImageWithDialog(image) ? 0 : 1;
        }
    }

    if (image.isNull()) {
        return 1;
    }

    return writeOutput(image, config) ? 0 : 1;
}
