/*
 * custom-clipboard — a clipboard history popup with click-to-paste for KDE Plasma.
 *
 * Layout and interaction follow the Windows 11 clipboard panel (search on top,
 * roomy rounded rows, type thumbnails, timestamps, hover actions) rendered with
 * Plasma/Breeze colours.
 *
 * History comes from Klipper's SQLite database, so it is not limited by
 * Klipper's own in-memory cap. Selecting an entry pastes it straight into the
 * previously focused input via ydotool (uinput) — the only reliable way to send
 * a keystroke to another app on Wayland.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QCursor>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QFile>
#include <QGuiApplication>
#include <QHash>
#include <QIcon>
#include <QImage>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QSettings>
#include <QShortcut>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUrl>
#include <QStyledItemDelegate>
#include <QFrame>
#include <QStackedWidget>
#include <QTabBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <QDBusConnection>
#include <QDBusInterface>

#include <kglobalaccel.h>

#include <algorithm>

namespace
{
const int ROW_HEIGHT = 44;
const int THUMB = 34;
const int BTN = 28;
const int POPUP_WIDTH = 400;

// Right-hand action buttons. Painting and hit-testing both call these, so they
// can never disagree, and no width is hardcoded. Remove is outermost (harder to
// hit by accident); star sits inboard of it.
QRect removeButtonRect(const QRect &row)
{
    return QRect(row.right() - 10 - BTN, row.center().y() - BTN / 2, BTN, BTN);
}

QRect starButtonRect(const QRect &row)
{
    return QRect(removeButtonRect(row).left() - 4 - BTN, row.center().y() - BTN / 2, BTN, BTN);
}

// Opt-in click tracing, so a mis-click can be diagnosed from evidence rather
// than guesswork. Enable with CUSTOM_CLIPBOARD_DEBUG=1
void traceClick(const QString &line)
{
    if (qEnvironmentVariableIsEmpty("CUSTOM_CLIPBOARD_DEBUG")) {
        return;
    }
    QFile f(QDir::tempPath() + QStringLiteral("/custom-clipboard-clicks.log"));
    if (f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        f.write(line.toUtf8() + '\n');
    }
}

enum SortMode { Newest, Oldest, AlphaAsc, AlphaDesc };

// The real Klipper database, ignoring CUSTOM_CLIPBOARD_DATA. The suite must be
// able to name the user's actual file even while sandboxed, otherwise its
// "real database untouched" assertion would compare the sandbox against itself.
QString realKlipperDbPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
        + QStringLiteral("/klipper/history3.sqlite");
}

QString klipperDir()
{
    // Overridable so --test can exercise the write paths against a throwaway
    // copy instead of the user's real clipboard history.
    const QByteArray env = qgetenv("CUSTOM_CLIPBOARD_DATA");
    if (!env.isEmpty()) {
        return QString::fromLocal8Bit(env);
    }
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + QStringLiteral("/klipper");
}

void setClipboardText(const QString &text)
{
    // Route through Klipper so its history/starred ordering stays authoritative.
    QDBusInterface klipper(QStringLiteral("org.kde.klipper"),
                           QStringLiteral("/klipper"),
                           QStringLiteral("org.kde.klipper.klipper"));
    klipper.asyncCall(QStringLiteral("setClipboardContents"), text);
}

struct Entry {
    QString uuid;
    QString text;
    QString mimetypes;
    qint64 when = 0;
    bool starred = false;
    bool isImage = false;
};

// Counts rows in a database at an explicit path, ignoring CUSTOM_CLIPBOARD_DATA.
// Used by the suite to assert the user's real history is never modified, which
// cannot be checked through the sandboxed readHistory().
int readRealHistorySize(const QString &dbPath)
{
    if (!QFile::exists(dbPath)) {
        return -1;
    }
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("ccreal"));
    db.setDatabaseName(dbPath);
    db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
    if (!db.open()) {
        QSqlDatabase::removeDatabase(QStringLiteral("ccreal"));
        return -1;
    }
    QSqlQuery q(db);
    q.exec(QStringLiteral("PRAGMA busy_timeout=2000"));
    const int n = q.exec(QStringLiteral("SELECT COUNT(*) FROM main")) && q.next() ? q.value(0).toInt() : -1;
    db.close();
    QSqlDatabase::removeDatabase(QStringLiteral("ccreal"));
    return n;
}

QVector<Entry> readHistory()
{
    QVector<Entry> out;
    const QString path = klipperDir() + QStringLiteral("/history3.sqlite");
    if (!QFile::exists(path)) {
        return out;
    }
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("cchist"));
    db.setDatabaseName(path);
    db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
    if (!db.open()) {
        QSqlDatabase::removeDatabase(QStringLiteral("cchist"));
        return out;
    }
    QSqlQuery q(db);
    q.exec(QStringLiteral("PRAGMA busy_timeout=2000"));
    q.exec(QStringLiteral("SELECT uuid, text, mimetypes, starred, "
                           "MAX(added_time, IFNULL(last_used_time,0)) FROM main"));
    while (q.next()) {
        Entry e;
        e.uuid = q.value(0).toString();
        e.text = q.value(1).toString();
        e.mimetypes = q.value(2).toString();
        e.starred = q.value(3).toInt() != 0;
        e.when = qint64(q.value(4).toDouble());
        e.isImage = e.mimetypes.contains(QLatin1String("image/"));
        out.append(e);
    }
    db.close();
    QSqlDatabase::removeDatabase(QStringLiteral("cchist"));
    return out;
}

QImage loadImage(const Entry &e)
{
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("ccimg"));
    db.setDatabaseName(klipperDir() + QStringLiteral("/history3.sqlite"));
    db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
    QString dataId;
    if (db.open()) {
        QSqlQuery q(db);
        q.exec(QStringLiteral("PRAGMA busy_timeout=2000"));
        q.exec(QStringLiteral("SELECT data_uuid FROM aux WHERE uuid='%1' AND mimetype LIKE 'image/%' LIMIT 1")
                      .arg(e.uuid));
        if (q.next()) {
            dataId = q.value(0).toString();
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("ccimg"));
    if (dataId.isEmpty()) {
        return QImage();
    }
    // Klipper lays payloads out as data/<item-uuid>/<data-uuid>. Older/other
    // layouts put the blob straight in data/, so try both.
    const QString base = klipperDir() + QStringLiteral("/data/");
    for (const QString &path : {base + e.uuid + QLatin1Char('/') + dataId, base + dataId}) {
        QImage img;
        if (img.load(path)) {
            return img;
        }
    }
    return QImage();
}

// Is the ydotool client binary installed at all?
bool haveYdotool()
{
    return !QStandardPaths::findExecutable(QStringLiteral("ydotool")).isEmpty();
}

// The ydotool binary existing is NOT enough. Injection goes through the
// ydotoold daemon, and when that daemon cannot open /dev/uinput it dies and
// systemd gives up ("Start request repeated too quickly"). Every ydotool call
// then fails silently, so a click would close the panel and paste nothing —
// which looks exactly like "clicking an item is broken".
//
// Ask the daemon instead of guessing: a successful no-op command proves both
// the socket and the daemon are alive. Guessing socket paths is not reliable —
// ydotoold listens on $XDG_RUNTIME_DIR/.ydotool_socket, and QStandardPaths
// resolves that differently under the offscreen platform the tests run on.
bool ydotoolReady()
{
    // Cache the verdict: this runs in the click path, and spawning a process and
    // waiting on it there would stall the UI for the length of the probe. A dead
    // daemon is also the common case worth avoiding, since every click would
    // otherwise pay for a failing probe.
    static bool cached = false;
    static qint64 probedAt = 0;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (probedAt != 0 && now - probedAt < 2000) {
        return cached;
    }

    QProcess probe;
    probe.start(QStringLiteral("ydotool"), {QStringLiteral("key"), QStringLiteral("0:0")});
    if (!probe.waitForStarted(2000)) {
        cached = false;
    } else {
        probe.waitForFinished(2000);
        cached = probe.exitStatus() == QProcess::NormalExit && probe.exitCode() == 0;
    }
    probedAt = now;
    return cached;
}

QString flat(const QString &text, int maxChars)
{
    QString s = text;
    s.replace(QRegularExpression(QStringLiteral("[\\r\\n\\t]+")), QStringLiteral("  "));
    s.replace(QRegularExpression(QStringLiteral(" {2,}")), QStringLiteral(" "));
    s = s.trimmed();
    if (s.size() > maxChars) {
        s = s.left(maxChars - 1) + QChar(0x2026);
    }
    return s;
}

QString relativeTime(qint64 secs)
{
    if (secs <= 0) {
        return QString();
    }
    if (secs < 60) {
        return QStringLiteral("just now");
    }
    if (secs < 3600) {
        return QStringLiteral("%1 min ago").arg(secs / 60);
    }
    if (secs < 86400) {
        return QStringLiteral("%1 h ago").arg(secs / 3600);
    }
    if (secs < 604800) {
        return QStringLiteral("%1 d ago").arg(secs / 86400);
    }
    return QDateTime::fromSecsSinceEpoch(secs).toString(QStringLiteral("d MMM"));
}

// Klipper also records copied *files* as plain text/uri-list entries. When such
// a URI points at a real image on disk, show it as an image with a thumbnail
// instead of dumping a file:// path at the user.
bool isImagePath(const QString &path)
{
    static const QStringList exts = {QStringLiteral("png"),  QStringLiteral("jpg"), QStringLiteral("jpeg"),
                                     QStringLiteral("gif"),  QStringLiteral("bmp"), QStringLiteral("webp"),
                                     QStringLiteral("svg"),  QStringLiteral("tif"), QStringLiteral("tiff")};
    return !path.isEmpty() && exts.contains(QFileInfo(path).suffix().toLower());
}

QString localImageUriPath(const Entry &e)
{
    if (e.isImage || e.uuid.isEmpty()) {
        return QString();
    }
    const bool uriish = e.mimetypes.contains(QLatin1String("text/uri-list"))
                        || e.text.startsWith(QLatin1String("file://"));
    if (!uriish) {
        return QString();
    }
    const QString first = e.text.split(QRegularExpression(QStringLiteral("[\r\n]")), Qt::SkipEmptyParts).value(0).trimmed();
    if (!first.startsWith(QLatin1String("file://"))) {
        return QString();
    }
    const QString path = QUrl(first).toLocalFile();
    if (path.isEmpty() || !QFileInfo::exists(path)) {
        return QString();
    }
    return isImagePath(path) ? path : QString();
}

// Klipper stores the original filename for images separately from the blob.
QString imageFileName(const QString &uuid)
{
    QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("ccname"));
    db.setDatabaseName(klipperDir() + QStringLiteral("/history3.sqlite"));
    db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
    QString dataId;
    if (db.open()) {
        QSqlQuery q(db);
        q.exec(QStringLiteral("PRAGMA busy_timeout=2000"));
        q.prepare(QStringLiteral("SELECT data_uuid FROM aux WHERE uuid=? AND mimetype=?"));
        q.addBindValue(uuid);
        q.addBindValue(QStringLiteral("application/x-kde-suggestedfilename"));
        if (q.exec() && q.next()) {
            dataId = q.value(0).toString();
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("ccname"));
    if (dataId.isEmpty()) {
        return QString();
    }
    QFile f(klipperDir() + QStringLiteral("/data/") + uuid + QLatin1Char('/') + dataId);
    if (!f.open(QIODevice::ReadOnly)) {
        return QString();
    }
    return QString::fromUtf8(f.readAll()).trimmed();
}

// Sorting is deliberately independent of `starred`: starred entries live in
// their own tab, so within "All" they must sort exactly like everything else.
void sortEntries(QVector<Entry> &v, SortMode mode)
{
    switch (mode) {
    case Newest:
        std::stable_sort(v.begin(), v.end(), [](const Entry &a, const Entry &b) {
            return a.when > b.when;
        });
        break;
    case Oldest:
        std::stable_sort(v.begin(), v.end(), [](const Entry &a, const Entry &b) {
            return a.when < b.when;
        });
        break;
    case AlphaAsc:
        std::stable_sort(v.begin(), v.end(), [](const Entry &a, const Entry &b) {
            return a.text.compare(b.text, Qt::CaseInsensitive) < 0;
        });
        break;
    case AlphaDesc:
        std::stable_sort(v.begin(), v.end(), [](const Entry &a, const Entry &b) {
            return a.text.compare(b.text, Qt::CaseInsensitive) > 0;
        });
        break;
    }
}

// Returns the indexes of entries matching the search text / starred filter.
QVector<int> filterRows(const QVector<Entry> &v, const QString &needle, bool starredOnly)
{
    QVector<int> rows;
    for (int i = 0; i < v.size(); ++i) {
        const Entry &e = v.at(i);
        if (starredOnly && !e.starred) {
            continue;
        }
        if (!needle.isEmpty() && !e.text.contains(needle, Qt::CaseInsensitive)) {
            continue;
        }
        rows.append(i);
    }
    return rows;
}

// Shared between the widget and its delegate, so the delegate never needs to
// know about Deck (which would be a circular dependency).
class Store
{
public:
    QVector<Entry> entries;
    mutable QHash<QString, QImage> thumbs;
    mutable QHash<QString, QString> captions;
    int hoverRow = -1;

    const Entry *entryFor(int entryIndex) const
    {
        return (entryIndex >= 0 && entryIndex < entries.size()) ? &entries.at(entryIndex) : nullptr;
    }

    // Caption for a row: images show their filename, text shows its content.
    QString captionFor(const Entry &e) const
    {
        if (!isVisualImage(e)) {
            return e.text;
        }
        auto it = captions.constFind(e.uuid);
        if (it != captions.constEnd()) {
            return it.value();
        }
        QString name;
        if (e.isImage) {
            name = imageFileName(e.uuid);
        }
        if (name.isEmpty()) {
            const QString path = localImageUriPath(e);
            name = path.isEmpty() ? QString() : QFileInfo(path).fileName();
        }
        if (name.isEmpty()) {
            const QSize px = imageFor(e.uuid).size();
            name = px.isValid() && !px.isEmpty() ? QStringLiteral("Image — %1×%2").arg(px.width()).arg(px.height())
                                                 : QStringLiteral("Image");
        }
        captions.insert(e.uuid, name);
        return name;
    }

    bool isVisualImage(const Entry &e) const
    {
        return e.isImage || !localImageUriPath(e).isEmpty();
    }

    QImage imageFor(const QString &uuid) const
    {
        auto it = thumbs.constFind(uuid);
        if (it != thumbs.constEnd()) {
            return it.value();
        }
        QImage img;
        for (const Entry &e : entries) {
            if (e.uuid != uuid) {
                continue;
            }
            if (e.isImage) {
                img = loadImage(e);
            } else {
                const QString path = localImageUriPath(e);
                if (!path.isEmpty()) {
                    img.load(path);
                }
            }
            break;
        }
        thumbs.insert(uuid, img);
        return img;
    }

    void clearThumbs()
    {
        thumbs.clear();
        captions.clear();
    }
};
}

class RowDelegate : public QStyledItemDelegate
{
public:
    explicit RowDelegate(Store *store)
        : m_store(store)
    {
    }

    void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &idx) const override
    {
        const bool selected = opt.state & QStyle::State_Selected;
        const bool hovered = m_store && m_store->hoverRow == idx.row();
        const QRect row = opt.rect;

        p->save();
        p->setRenderHint(QPainter::Antialiasing, true);

        if (selected || hovered) {
            QColor bg = opt.palette.color(selected ? QPalette::Highlight : QPalette::AlternateBase);
            if (!selected) {
                bg.setAlpha(90);
            }
            p->setPen(Qt::NoPen);
            p->setBrush(bg);
            p->drawRoundedRect(row.adjusted(8, 1, -8, -1), 7, 7);
        }

        const Entry *e = m_store ? m_store->entryFor(idx.data(Qt::UserRole).toInt()) : nullptr;
        const QColor fg = selected ? opt.palette.color(QPalette::HighlightedText) : opt.palette.color(QPalette::Text);
        const QColor dim = selected ? opt.palette.color(QPalette::HighlightedText)
                                    : opt.palette.color(QPalette::PlaceholderText);

        // Thumbnail / type icon
        const QRect thumb(row.left() + 14, row.center().y() - THUMB / 2, THUMB, THUMB);
        if (e && m_store->isVisualImage(*e)) {
            const QImage img = m_store->imageFor(e->uuid);
            if (img.isNull()) {
                // Blob unreadable — show a picture glyph rather than a blank.
                QFont g = opt.font;
                g.setPointSizeF(qMax(7.0, opt.font.pointSizeF() - 1));
                p->setFont(g);
                p->setPen(dim);
                p->drawText(thumb, Qt::AlignCenter, QStringLiteral("▣"));
            } else {
                const QImage scaled = img.scaled(THUMB, THUMB, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                const QRect fitted(thumb.x() + (THUMB - scaled.width()) / 2,
                                   thumb.y() + (THUMB - scaled.height()) / 2,
                                   scaled.width(),
                                   scaled.height());
                p->setRenderHint(QPainter::SmoothPixmapTransform, true);
                p->drawImage(fitted, scaled);
                p->setPen(QPen(dim, 1));
                p->setBrush(Qt::NoBrush);
                p->drawRoundedRect(fitted.adjusted(0, 0, -1, -1), 4, 4);
            }
        } else {
            QIcon icon = QIcon::fromTheme(QStringLiteral("edit-paste"), QIcon::fromTheme(QStringLiteral("text-plain")));
            if (!icon.isNull()) {
                icon.paint(p, thumb, Qt::AlignCenter);
            } else {
                // Theme icon missing (common on minimal systems): draw our own.
                p->setPen(QPen(dim, 1.2));
                p->setBrush(Qt::NoBrush);
                const QRect page(thumb.x() + 7, thumb.y() + 3, thumb.width() - 15, thumb.height() - 6);
                p->drawRoundedRect(page, 3, 3);
                p->drawLine(page.topRight(), page.topRight() + QPoint(-5, -5));
                p->drawLine(page.topRight() + QPoint(-5, -5), page.topRight() + QPoint(-11, -5));
                QFont g = opt.font;
                g.setPointSizeF(qMax(7.0, opt.font.pointSizeF() - 2));
                p->setFont(g);
                p->drawText(page.adjusted(0, 4, 0, 0), Qt::AlignCenter, QStringLiteral("T"));
            }
        }

        // Text + timestamp, in two non-overlapping bands
        const int textLeft = row.left() + 14 + THUMB + 10;
        // Text must clear the leftmost button, whichever that currently is.
        const int textRight = qMin(starButtonRect(row).left(), removeButtonRect(row).left()) - 8;
        const int textWidth = qMax(40, textRight - textLeft);
        const QRect titleRect(textLeft, row.top() + 7, textWidth, 18);
        const QRect subRect(textLeft, row.top() + 25, textWidth, 14);

        QFont f = opt.font;
        p->setFont(f);
        p->setPen(fg);
        const QString title = e ? (m_store->isVisualImage(*e) ? m_store->captionFor(*e)
                                          : (e->text.isEmpty() ? QStringLiteral("(empty entry)")
                                                               : flat(e->text, 200)))
                                : QString();
        p->drawText(titleRect,
                    Qt::AlignLeft | Qt::AlignVCenter,
                    opt.fontMetrics.elidedText(title, Qt::ElideRight, titleRect.width()));

        if (e) {
            QFont small = f;
            small.setPointSizeF(qMax(7.0, f.pointSizeF() - 1.5));
            p->setFont(small);
            p->setPen(dim);
            const QString stamp = m_store->isVisualImage(*e) ? QStringLiteral("Image") : QStringLiteral("Text");
            const QString when = relativeTime(QDateTime::currentSecsSinceEpoch() - e->when);
            const QString sub = when.isEmpty() ? stamp : QStringLiteral("%1  ·  %2").arg(stamp, when);
            p->drawText(subRect,
                        Qt::AlignLeft | Qt::AlignVCenter,
                        opt.fontMetrics.elidedText(sub, Qt::ElideRight, subRect.width()));
        }

        // Hover actions, or the persistent star marker
        if (hovered) {
            // Filled chips make the click targets unmistakable.
            QColor chip = opt.palette.color(QPalette::Window);
            chip.setAlpha(selected ? 60 : 90);
            p->setPen(Qt::NoPen);
            p->setBrush(chip);
            for (const QRect &r : {starButtonRect(row), removeButtonRect(row)}) {
                p->drawRoundedRect(r.adjusted(0, 0, -1, -1), 6, 6);
            }
            QFont bf = f;
            bf.setPointSizeF(f.pointSizeF() + 1);
            p->setFont(bf);
            p->setPen(fg);
            p->drawText(starButtonRect(row), Qt::AlignCenter, e && e->starred ? QStringLiteral("★") : QStringLiteral("☆"));
            p->drawText(removeButtonRect(row), Qt::AlignCenter, QStringLiteral("✕"));
        } else if (e && e->starred) {
            QFont sf = f;
            sf.setPointSizeF(f.pointSizeF() + 1);
            p->setFont(sf);
            p->setPen(selected ? fg : QColor(0xE0, 0xA8, 0x2F));
            p->drawText(starButtonRect(row), Qt::AlignCenter, QStringLiteral("★"));
        }
        p->restore();
    }

    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override
    {
        // Width must come from the view. Forcing it here (e.g. to POPUP_WIDTH)
        // makes item rects wider than the viewport, which decouples where the
        // delegate paints from where hit-testing measures.
        return QSize(0, ROW_HEIGHT);
    }

private:
    Store *m_store = nullptr;
};

class Deck : public QWidget
{
public:
    Deck()
    {
        setWindowTitle(QStringLiteral("Clipboard"));
        // NOTE: deliberately a Tool window, not Qt::Popup. A popup needs a live
        // transient parent, and anchoring one to the focused window made the
        // panel fail to appear at all. Dismissal on an outside click is handled
        // by the activation-change handler in event() instead.
        setWindowFlags(Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        setAttribute(Qt::WA_TranslucentBackground);
        setMouseTracking(true);

        root = new QWidget(this); // member, not a local: other methods use it
        root->setObjectName(QStringLiteral("root"));
        root->setStyleSheet(QStringLiteral(R"(
            #root { background: palette(window); border: 1px solid palette(mid); border-radius: 12px; }
            QLabel#title { font-weight: 600; font-size: 13pt; }
            QLabel#count { color: palette(placeholder-text); }
            QLineEdit { border: 1px solid palette(mid); border-radius: 8px; padding: 6px 10px;
                        background: palette(base); selection-background-color: palette(highlight); }
            QLineEdit:focus { border-color: palette(highlight); }
            QTabBar::tab { padding: 5px 14px; border-radius: 7px; margin-right: 6px; color: palette(text); }
            QTabBar::tab:selected { background: palette(highlight); color: palette(highlighted-text); }
            QTabWidget::pane { border: none; }
            QListWidget { border: none; background: transparent; }
            QListWidget::item { border: none; }
            QComboBox { border: 1px solid palette(mid); border-radius: 7px; padding: 7px 12px;
                        background: palette(base); min-height: 18px; }
            QComboBox:hover { border-color: palette(highlight); }
            QComboBox::drop-down { border: none; width: 22px; }
            QComboBox QAbstractItemView { padding: 6px; border: 1px solid palette(mid);
                                         border-radius: 8px; background: palette(window);
                                         selection-background-color: palette(highlight);
                                         selection-color: palette(highlighted-text); outline: none; }
            QComboBox QAbstractItemView::item { padding: 8px 12px; border-radius: 6px;
                                                min-height: 22px; }
            QComboBox QAbstractItemView::item:hover { background: palette(alternate-base); }
            QPushButton { border: 1px solid palette(mid); border-radius: 7px; padding: 4px 12px;
                          background: palette(base); }
            QPushButton:hover { background: palette(alternate-base); }
            QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
            QScrollBar::handle:vertical { background: palette(mid); border-radius: 4px; min-height: 30px; }
            QScrollBar::add-line, QScrollBar::sub-line { height: 0; }
        )"));

        auto *outer = new QVBoxLayout(this);
        outer->setContentsMargins(0, 0, 0, 0);
        auto *lay = new QVBoxLayout(root);
        lay->setContentsMargins(14, 12, 14, 14);
        lay->setSpacing(8);
        outer->addWidget(root);

        // Header: title + count
        auto *head = new QHBoxLayout;
        head->setSpacing(8);
        auto *title = new QLabel(QStringLiteral("Clipboard"), root);
        title->setObjectName(QStringLiteral("title"));
        head->addWidget(title);
        m_count = new QLabel(root);
        m_count->setObjectName(QStringLiteral("count"));
        head->addWidget(m_count);
        head->addStretch(1);
        m_clearBtn = new QPushButton(QStringLiteral("Clear"), root);
        m_clearBtn->setToolTip(QStringLiteral("Remove every entry except starred ones"));
        head->addWidget(m_clearBtn);
        m_newBtn = new QPushButton(QStringLiteral("＋  New"), root);
        m_newBtn->setToolTip(QStringLiteral("Add a new entry"));
        m_newBtn->setFlat(true);
        head->addWidget(m_newBtn);
        lay->addLayout(head);

        // Search on top, like Windows 11
        m_search = new QLineEdit(root);
        m_search->setPlaceholderText(QStringLiteral("Search clipboard…"));
        m_search->setClearButtonEnabled(true);
        lay->addWidget(m_search);

        // Tabs + sort. A bare QTabBar (not QTabWidget) is used deliberately:
        // QTabWidget's pane would contain the list, which makes the sort box
        // centre itself against the full-height list instead of the tab row.
        auto *bar = new QHBoxLayout;
        bar->setSpacing(8);
        m_tabBar = new QTabBar(root);
        m_tabBar->setDrawBase(false);
        m_tabBar->addTab(QStringLiteral("All"));
        m_tabBar->addTab(QStringLiteral("Starred"));
        m_tabBar->setTabVisible(1, false); // only shown when something is starred
        bar->addWidget(m_tabBar);
        bar->addStretch(1);
        m_sort = new QComboBox(root);
        m_sort->addItem(QStringLiteral("Newest"), int(Newest));
        m_sort->addItem(QStringLiteral("Oldest"), int(Oldest));
        m_sort->addItem(QStringLiteral("A–Z"), int(AlphaAsc));
        m_sort->addItem(QStringLiteral("Z–A"), int(AlphaDesc));
        m_sort->setToolTip(QStringLiteral("Sort order"));
        bar->addWidget(m_sort);
        lay->addLayout(bar);

        m_list = makeList();
        m_starList = makeList();
        m_stack = new QStackedWidget(root);
        m_stack->setFrameShape(QFrame::NoFrame);
        m_stack->addWidget(m_list);
        m_stack->addWidget(m_starList);
        lay->addWidget(m_stack, 1);

        // Inline "new entry" field — a modal dialog would open behind this
        // always-on-top frameless window, so it lives in the popup itself.
        m_newEntry = new QLineEdit(root);
        m_newEntry->setPlaceholderText(QStringLiteral("Type a new entry and press Enter…"));
        m_newEntry->setVisible(false);
        lay->addWidget(m_newEntry);

        // The status line floats OVER the list rather than sitting in the layout.
        // As a layout row it resized the list every time it appeared, shifting
        // every row down — so a fast second click after an action landed on a
        // different row's body and pasted it instead of hitting the button
        // again. Absolute positioning keeps the rows still, so the button under
        // the pointer is still the button under the pointer.
        m_status = new QLabel(root);
        m_status->setObjectName(QStringLiteral("statusmsg"));
        m_status->setVisible(false);
        m_status->setWordWrap(true);
        m_status->setAlignment(Qt::AlignCenter);
        m_status->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_status->setStyleSheet(QStringLiteral(R"(
            #statusmsg { color: palette(placeholder-text);
                     background: palette(window);
                     border-top: 1px solid palette(mid);
                     padding: 7px 10px; }
        )"));

        connect(m_search, &QLineEdit::textChanged, this, [this] { rebuild(); });
        connect(m_newEntry, &QLineEdit::returnPressed, this, &Deck::commitNewEntry);
        connect(m_newBtn, &QPushButton::clicked, this, &Deck::toggleNewEntry);
        connect(m_clearBtn, &QPushButton::clicked, this, &Deck::toggleClear);
        buildOverlay();
        connect(m_sort, &QComboBox::activated, this, [this] {
            m_sortMode = SortMode(m_sort->currentData().toInt());
            applySort();
            rebuild();
        });
        connect(m_tabBar, &QTabBar::currentChanged, this, [this](int i) {
            m_stack->setCurrentIndex(i);
            setHoverRow(-1);
        });

        for (QListWidget *lw : {m_list, m_starList}) {
            connect(lw, &QListWidget::itemActivated, this, [this, lw](QListWidgetItem *it) { choose(lw, it); });
            connect(lw, &QListWidget::itemClicked, this, [this, lw](QListWidgetItem *it) { choose(lw, it); });
            connect(lw, &QListWidget::customContextMenuRequested, this, &Deck::contextMenu);
            lw->setContextMenuPolicy(Qt::CustomContextMenu);
        }

        m_delegate = new RowDelegate(&m_store);
        m_delegate->setParent(this);
        m_list->setItemDelegate(m_delegate);
        m_starList->setItemDelegate(m_delegate);

        auto *esc = new QShortcut(QKeySequence(Qt::Key_Escape), this);
        esc->setContext(Qt::WindowShortcut);
        connect(esc, &QShortcut::activated, this, [this] {
            if (m_overlay && m_overlay->isVisible()) {
                m_overlay->hide(); // cancel the confirm first
                return;
            }
            hide();
        });
    }

    // --- headless preview / test support ------------------------------------
    void renderState(const QString &path, const QString &state)
    {
        reload();
        m_search->clear();
        m_newEntry->setVisible(false);
        m_tabBar->setCurrentIndex(0);
        m_stack->setCurrentIndex(0);
        if (state == QLatin1String("search")) {
            m_search->setText(QStringLiteral("the"));
        } else if (state == QLatin1String("starred")) {
            m_tabBar->setCurrentIndex(1);
            m_stack->setCurrentIndex(1);
        } else if (state == QLatin1String("new")) {
            m_newEntry->setText(QStringLiteral("a freshly added entry"));
            m_newEntry->setVisible(true);
        } else if (state == QLatin1String("alpha")) {
            m_sort->setCurrentIndex(2); // A-Z
            m_sortMode = AlphaAsc;
            applySort();
            rebuild();
        }
        resize(POPUP_WIDTH, 480);
        show();
        QApplication::processEvents();
        // State is applied only once the widget is laid out and visible;
        // touching child geometry before this used to crash.
        if (state == QLatin1String("hover")) {
            hoverAt(m_list->viewport(), rowRectFor(2).center());
        } else if (state == QLatin1String("confirm")) {
            showConfirm(QStringLiteral("Clear clipboard history?"),
                        QStringLiteral("This permanently removes 27 unstarred entries.\n\n14 starred entries are kept."));
        }
        QApplication::processEvents();
        if (state == QLatin1String("hover") && !qEnvironmentVariableIsEmpty("CUSTOM_CLIPBOARD_DEBUG")) {
            const QRect r = rowRectFor(2);
            traceClick(QStringLiteral("GEOMETRY viewportW=%1 row=(%2,%3 %4x%5) star=(%6,%7 %8x%9) remove=(%10,%11 %12x%13)")
                           .arg(m_list->viewport()->width())
                           .arg(r.x())
                           .arg(r.y())
                           .arg(r.width())
                           .arg(r.height())
                           .arg(starButtonRect(r).x())
                           .arg(starButtonRect(r).y())
                           .arg(starButtonRect(r).width())
                           .arg(starButtonRect(r).height())
                           .arg(removeButtonRect(r).x())
                           .arg(removeButtonRect(r).y())
                           .arg(removeButtonRect(r).width())
                           .arg(removeButtonRect(r).height()));
        }
        QApplication::processEvents();
        grab().save(path);
        hide();
    }

    // --- test hooks -----------------------------------------------------------
    // Real mouse events delivered straight to the widget. This exercises the
    // exact hit-testing the user clicks on, with no OS-level input injection
    // (which would hijack the real desktop).
    void showForTest()
    {
        reload();
        m_tabBar->setCurrentIndex(0);
        m_stack->setCurrentIndex(0);
        resize(POPUP_WIDTH, 480);
        show();
        QApplication::processEvents();
    }

    QListWidget *testList()
    {
        return m_list;
    }

    QRect rowRectFor(int row) const
    {
        return m_list->visualRect(m_list->model()->index(row, 0));
    }

    // Must come from the deck's own (sorted) store: Qt::UserRole indexes that
    // array, not a freshly-read unsorted one.
    QString uuidAtRow(int row) const
    {
        auto *it = m_list->item(row);
        if (!it) {
            return QString();
        }
        const Entry *e = m_store.entryFor(it->data(Qt::UserRole).toInt());
        return e ? e->uuid : QString();
    }

    bool starredAtRow(int row) const
    {
        auto *it = m_list->item(row);
        if (!it) {
            return false;
        }
        const Entry *e = m_store.entryFor(it->data(Qt::UserRole).toInt());
        return e && e->starred;
    }

    int firstNonStarredRow() const
    {
        for (int i = 0; i < m_list->count(); ++i) {
            if (!starredAtRow(i)) {
                return i;
            }
        }
        return -1;
    }

    // A real double-click: Qt delivers the second press as MouseButtonDblClick,
// NOT MouseButtonPress. Synthesising two plain presses would hide that.
static int doubleClickAt(QWidget *target, const QPoint &pos)
    {
        const QPointF p(pos);
        const QPointF g = target->mapToGlobal(pos);
        int dblClicks = 0;
        QMouseEvent press(QEvent::MouseButtonPress, p, g, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(target, &press);
        QMouseEvent rel1(QEvent::MouseButtonRelease, p, g, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(target, &rel1);
        QMouseEvent dbl(QEvent::MouseButtonDblClick, p, g, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(target, &dbl);
        if (dbl.isAccepted()) {
            ++dblClicks;
        }
        QMouseEvent rel2(QEvent::MouseButtonRelease, p, g, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(target, &rel2);
        QApplication::processEvents();
        return dblClicks;
    }

    static bool clickAt(QWidget *target, const QPoint &pos)
    {
        const QPointF p(pos);
        QMouseEvent press(QEvent::MouseButtonPress, p, target->mapToGlobal(pos), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(target, &press);
        QMouseEvent release(QEvent::MouseButtonRelease, p, target->mapToGlobal(pos), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(target, &release);
        QApplication::processEvents();
        return true;
    }

    static void hoverAt(QWidget *target, const QPoint &pos)
    {
        const QPointF p(pos);
        QMouseEvent move(QEvent::MouseMove, p, target->mapToGlobal(pos), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(target, &move);
        QApplication::processEvents();
    }

    QWidget *rootWidget() const
    {
        return root;
    }

    // Reproduce the reported double-click on the remove button: press, release,
// then press again at the same spot with no hover in between (exactly what a
// fast double-click delivers).
struct DoubleClickResult {
        int removed = 0;
        bool pasted = false;
    };

    // Replays the reported failure: click the X twice quickly and report what the
    // panel actually did. A real double-click's second press arrives as
    // MouseButtonDblClick, which synthesising two plain presses would not exercise.
    DoubleClickResult doubleClickRemoveForTest(int row)
    {
        DoubleClickResult result;
        const QPoint p = removeButtonRect(rowRectFor(row)).center();
        QWidget *vp = testList()->viewport();
        const int countBefore = m_list->count();
        m_chooseCount = 0;

        Deck::clickAt(vp, p); // first click
        QApplication::processEvents();

        // No hover or motion in between: straight to a second press/release.
        Deck::doubleClickAt(vp, p);
        QApplication::processEvents();

        result.removed = countBefore - m_list->count();
        result.pasted = m_chooseCount > 0;
        return result;
    }

    int chooseCountForTest() const
    {
        return m_chooseCount;
    }

    void resetChooseCountForTest()
    {
        m_chooseCount = 0;
    }

    void flashForTest(const QString &msg)
    {
        flash(msg);
    }

    bool statusVisibleForTest() const
    {
        return m_status && m_status->isVisible();
    }

    bool statusIsOverlayForTest() const
    {
        return m_status && m_status->testAttribute(Qt::WA_TransparentForMouseEvents);
    }

    // --- dismissal rule, under test ------------------------------------------
    // Lets the headless suite act out "the compositor deactivated us" with the
    // pointer at a chosen spot, without a real desktop.
    void armDismissalForTest()
    {
        m_deactivateArmed = true;
    }

    void setCursorPosForTest(const QPoint &globalPos)
    {
        m_cursorOverride = globalPos;
    }

    void deactivateForTest()
    {
        handleDeactivate();
    }

    void showConfirmForTest(const QString &t, const QString &b)
    {
        showConfirm(t, b);
    }

    bool confirmVisibleForTest() const
    {
        return m_overlay && m_overlay->isVisible();
    }

    void hideConfirmForTest()
    {
        if (m_overlay) {
            m_overlay->hide();
        }
    }

    int starredCount() const
    {
        int n = 0;
        for (const Entry &e : m_store.entries) {
            n += e.starred ? 1 : 0;
        }
        return n;
    }

    void selectFirstOf(int listIndex)
    {
        QListWidget *lw = listIndex == 1 ? m_starList : m_list;
        if (lw->count()) {
            lw->setCurrentRow(0);
        }
    }

    void setStarByIndex(int entryIndex, bool on)
    {
        if (entryIndex >= 0 && entryIndex < m_store.entries.size()) {
            setStar(m_store.entries.at(entryIndex).uuid, on);
        }
    }

    void deleteByIndex(int entryIndex)
    {
        if (entryIndex >= 0 && entryIndex < m_store.entries.size()) {
            deleteEntry(m_store.entries.at(entryIndex).uuid);
        }
    }

    // Deletes every unstarred entry, keeping starred ones. Returns how many
    // were removed. Starred rows are explicitly preserved.
    int clearUnstarred()
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("ccclear"));
        db.setDatabaseName(klipperDir() + QStringLiteral("/history3.sqlite"));
        int removed = 0;
        if (db.open()) {
            QSqlQuery q(db);
            q.exec(QStringLiteral("PRAGMA busy_timeout=2000"));
            const QString where = QStringLiteral(" WHERE starred=0 OR starred IS NULL");

            if (q.exec(QStringLiteral("SELECT COUNT(*) FROM main") + where) && q.next()) {
                removed = q.value(0).toInt();
            }
            // aux rows first, otherwise they become orphans
            q.prepare(QStringLiteral("DELETE FROM aux WHERE uuid IN (SELECT uuid FROM main") + where + QStringLiteral(")"));
            q.exec();
            q.prepare(QStringLiteral("DELETE FROM main") + where);
            q.exec();
            db.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("ccclear"));
        reload();
        return removed;
    }

    int unstarredCount() const
    {
        int n = 0;
        for (const Entry &e : m_store.entries) {
            n += e.starred ? 0 : 1;
        }
        return n;
    }

    // Exposed for the headless test suite.
    void reload()
    {
        m_store.clearThumbs();
        m_store.entries = readHistory();
        applySort();
        rebuild();
    }

    void setStar(const QString &uuid, bool on)
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("ccstar"));
        db.setDatabaseName(klipperDir() + QStringLiteral("/history3.sqlite"));
        if (db.open()) {
            QSqlQuery q(db);
            // Must bind properly: QString::arg() only substitutes %1-style
            // placeholders, so "? " would reach SQLite verbatim and fail.
            q.prepare(QStringLiteral("UPDATE main SET starred=? WHERE uuid=?"));
            q.addBindValue(on ? 1 : 0);
            q.addBindValue(uuid);
            if (!q.exec()) {
                qWarning("custom-clipboard: star update failed: %s", qPrintable(q.lastError().text()));
            }
            db.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("ccstar"));
        reload();
    }

    int hoverRow() const
    {
        return m_store.hoverRow;
    }

    const Entry *entryAt(int listRow) const
    {
        return m_store.entryFor(listRow);
    }

    void toggle()
    {
        if (isVisible()) {
            hide();
            return;
        }
        reload();
        m_search->clear();
        m_newEntry->setVisible(false);
        // Always reopen on "All" so the popup is predictable.
        m_tabBar->setCurrentIndex(0);
        m_stack->setCurrentIndex(0);
        if (m_store.entries.isEmpty()) {
            flash(QStringLiteral("Clipboard history is empty"));
            return;
        }
        layoutAtCursor();
        show();
        raise();
        activateWindow();
        m_search->setFocus();
        // Ignore deactivations for a moment: between show() and the compositor
        // granting us focus there is a window where it can deactivate us again.
        m_deactivateArmed = false;
        QTimer::singleShot(250, this, [this] { m_deactivateArmed = true; });
    }

protected:
    // Is the pointer over our own panel? Used to tell "the user clicked on one
    // of our rows" apart from "the user clicked somewhere else".
    bool cursorInsidePanel() const
    {
        return frameGeometry().contains(effectiveCursorPos());
    }

    QPoint effectiveCursorPos() const
    {
        return m_cursorOverride.isNull() ? QCursor::pos() : m_cursorOverride;
    }

    bool event(QEvent *ev) override
    {
        // On Wayland a Tool window gets no pointer grab, so "clicked outside" is
        // reported as the panel losing activation. But our own widgets can also
        // deactivate us — a row click, the confirm card, a context menu opening.
        // Those must NOT dismiss the panel: hiding on the press meant the row
        // never saw its release, so itemClicked never fired and nothing pasted.
        //
        // So only treat it as an outside click when the pointer is genuinely
        // off the panel and no popup of ours owns the interaction.
        if (ev->type() == QEvent::WindowDeactivate && m_deactivateArmed) {
            handleDeactivate();
        }
        return QWidget::event(ev);
    }

    void handleDeactivate()
    {
        if (cursorInsidePanel() || QApplication::activePopupWidget()
            || (m_overlay && m_overlay->isVisible())) {
            traceClick(QStringLiteral("deactivated inside panel -> ignore"));
            return;
        }
        traceClick(QStringLiteral("deactivated outside -> hide"));
        hide();
    }

    void resizeEvent(QResizeEvent *ev) override
    {
        QWidget::resizeEvent(ev);
        if (m_overlay && m_overlay->isVisible()) {
            m_overlay->setGeometry(root->rect());
        }
        positionStatus();
    }

    // Park the status line across the foot of the list. Sits on top of the rows
    // rather than below them, so showing it never resizes the list.
    void positionStatus()
    {
        if (!m_status || !m_stack) {
            return;
        }
        const QRect listArea = m_stack->geometry();
        const int h = m_status->sizeHint().height();
        m_status->setGeometry(listArea.left(), listArea.bottom() - h + 1,
                              listArea.width(), h);
    }

    void paintEvent(QPaintEvent *) override
    {
        // Rounded panel with a soft border, drawn behind the transparent children.
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QRectF r = rect().adjusted(0.5, 0.5, -0.5, -0.5);
        p.setPen(QPen(palette().color(QPalette::Mid), 1));
        p.setBrush(palette().color(QPalette::Window));
        p.drawRoundedRect(r, 12, 12);
    }

private:
    QListWidget *makeList()
    {
        auto *lw = new QListWidget(root);
        lw->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        lw->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); // no ugly h-scrollbar
        lw->setUniformItemSizes(true);
        lw->setMouseTracking(true);
        lw->viewport()->setAutoFillBackground(false);
        lw->viewport()->setAttribute(Qt::WA_TranslucentBackground);
        lw->viewport()->installEventFilter(this);
        return lw;
    }

    void applySort()
    {
        sortEntries(m_store.entries, m_sortMode);
    }

    void rebuild()
    {
        const QString needle = m_search->text().trimmed();
        int stars = 0;
        for (QListWidget *lw : {m_list, m_starList}) {
            const bool starredOnly = (lw == m_starList);
            const int keep = lw->currentRow();
            lw->clear();
            const QVector<int> rows = filterRows(m_store.entries, needle, starredOnly);
            for (int i : rows) {
                auto *it = new QListWidgetItem(lw);
                it->setData(Qt::UserRole, i);
                it->setToolTip(m_store.entries.at(i).text);
            }
            if (lw->count()) {
                lw->setCurrentRow(qBound(0, keep, lw->count() - 1));
            }
        }
        for (const Entry &e : m_store.entries) {
            stars += e.starred ? 1 : 0;
        }
        m_tabBar->setTabVisible(1, stars > 0);
        if (stars == 0 && m_stack->currentIndex() == 1) {
            m_tabBar->setCurrentIndex(0);
            m_stack->setCurrentIndex(0);
        }
        m_count->setText(QStringLiteral("%1 item%2")
                             .arg(m_store.entries.size())
                             .arg(m_store.entries.size() == 1 ? QString() : QStringLiteral("s")));
        m_status->setVisible(false);
    }

    void layoutAtCursor()
    {
        const int rows = qBound(3, int(m_store.entries.size()), 8);
        const int h = rows * ROW_HEIGHT + 232;
        resize(POPUP_WIDTH, h);

        QScreen *screen = QGuiApplication::screenAt(QCursor::pos());
        if (!screen) {
            screen = QGuiApplication::primaryScreen();
        }
        if (!screen) {
            move(QCursor::pos());
            return;
        }
        const QRect avail = screen->availableGeometry();
        int x = QCursor::pos().x() + 12;
        int y = QCursor::pos().y() + 12;
        if (x + width() > avail.right()) {
            x = avail.right() - width();
        }
        if (y + h > avail.bottom()) {
            y = avail.bottom() - h;
        }
        move(qMax(avail.left(), x), qMax(avail.top(), y));
    }

    void flash(const QString &msg)
    {
        m_status->setText(msg);
        positionStatus();
        m_status->setVisible(true);
        QTimer::singleShot(2600, this, [this] { m_status->setVisible(false); });
    }

    // In-window confirmation popup. A QMessageBox would render *behind* this
// always-on-top frameless window — the same bug that made "New" look broken —
// so the dialog is drawn inside our own window instead.
    void buildOverlay()
    {
        m_overlay = new QWidget(root);
        m_overlay->setObjectName(QStringLiteral("overlay"));
        m_overlay->setStyleSheet(QStringLiteral(R"(
            #overlay { background: rgba(0,0,0,110); border-radius: 12px; }
            #card { background: palette(window); border: 1px solid palette(mid); border-radius: 10px; }
            #confirmTitle { font-weight: 600; font-size: 11pt; }
            #confirmBody { color: palette(text); }
            QPushButton { padding: 6px 14px; }
            #danger { background: #c0392b; color: white; border: none; font-weight: 600; }
            #danger:hover { background: #e74c3c; }
        )"));
        auto *ov = new QVBoxLayout(m_overlay);
        ov->setContentsMargins(18, 18, 18, 18);
        ov->addStretch(1);

        auto *card = new QWidget(m_overlay);
        card->setObjectName(QStringLiteral("card"));
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(16, 14, 16, 14);
        cl->setSpacing(10);

        m_confirmTitle = new QLabel(card);
        m_confirmTitle->setObjectName(QStringLiteral("confirmTitle"));
        cl->addWidget(m_confirmTitle);

        m_confirmBody = new QLabel(card);
        m_confirmBody->setObjectName(QStringLiteral("confirmBody"));
        m_confirmBody->setWordWrap(true);
        cl->addWidget(m_confirmBody);

        auto *btns = new QHBoxLayout;
        btns->addStretch(1);
        auto *cancel = new QPushButton(QStringLiteral("Cancel"), card);
        auto *ok = new QPushButton(QStringLiteral("Clear"), card);
        ok->setObjectName(QStringLiteral("danger"));
        btns->addWidget(cancel);
        btns->addWidget(ok);
        cl->addLayout(btns);

        ov->addWidget(card);
        ov->addStretch(1);

        connect(cancel, &QPushButton::clicked, this, [this] { m_overlay->hide(); });
        connect(ok, &QPushButton::clicked, this, &Deck::confirmClear);
        m_overlay->hide();
    }

    void showConfirm(const QString &title, const QString &body)
    {
        m_confirmTitle->setText(title);
        m_confirmBody->setText(body);
        m_overlay->setGeometry(root->rect());
        m_overlay->raise();
        m_overlay->show();
    }

    void confirmClear()
    {
        m_overlay->hide();
        const int removed = clearUnstarred();
        flash(removed ? QStringLiteral("Removed %1 unstarred entr%2.")
                           .arg(removed)
                           .arg(removed == 1 ? QStringLiteral("y") : QStringLiteral("ies"))
                     : QStringLiteral("Nothing was removed."));
    }

    void toggleClear()
    {
        const int n = unstarredCount();
        const int kept = m_store.entries.size() - n;
        if (n == 0) {
            flash(QStringLiteral("Nothing to clear — every entry is starred."));
            return;
        }
        showConfirm(QStringLiteral("Clear clipboard history?"),
                    QStringLiteral("This permanently removes %1 unstarred %2.\n\n%3 starred %4 kept.")
                        .arg(n)
                        .arg(n == 1 ? QStringLiteral("entry") : QStringLiteral("entries"))
                        .arg(kept)
                        .arg(kept == 1 ? QStringLiteral("entry is") : QStringLiteral("entries are")));
    }

    void toggleNewEntry()
    {
        m_newEntry->setVisible(!m_newEntry->isVisible());
        if (m_newEntry->isVisible()) {
            m_newEntry->setFocus();
        } else {
            m_search->setFocus();
        }
    }

    void commitNewEntry()
    {
        const QString text = m_newEntry->text().trimmed();
        if (text.isEmpty()) {
            return;
        }
        setClipboardText(text);
        m_newEntry->clear();
        m_newEntry->setVisible(false);
        m_search->setFocus();
        flash(QStringLiteral("Added to clipboard."));
        QTimer::singleShot(700, this, [this] { reload(); });
    }

    void setHoverRow(int r)
    {
        if (m_store.hoverRow == r) {
            return;
        }
        const int old = m_store.hoverRow;
        m_store.hoverRow = r;
        if (old >= 0) {
            QListWidget *lw = m_stack->currentIndex() == 1 ? m_starList : m_list;
            if (old < lw->count()) {
                lw->viewport()->update(lw->visualItemRect(lw->item(old)));
            }
        }
        if (r >= 0) {
            QListWidget *lw = m_stack->currentIndex() == 1 ? m_starList : m_list;
            if (r < lw->count()) {
                lw->viewport()->update(lw->visualItemRect(lw->item(r)));
            }
        }
    }

    bool eventFilter(QObject *watched, QEvent *ev) override
    {
        if (ev->type() == QEvent::MouseMove
            && (watched == m_list->viewport() || watched == m_starList->viewport())) {
            auto *me = static_cast<QMouseEvent *>(ev);
            auto *view = qobject_cast<QAbstractItemView *>(qobject_cast<QWidget *>(watched)->parentWidget());
            setHoverRow(view->indexAt(me->pos()).row());
            return false;
        }
        // The hover action buttons are painted by the delegate, so the click has
        // to be hit-tested here — they are not real widgets.
        //
        // MouseButtonDblClick must be handled too. Qt delivers the second press
        // of a double-click as a *DblClick* event, not a Press, so a filter that
        // only watches Press lets the second click fall through to the list,
        // which then emits itemClicked and PASTES an entry. That is why clicking
        // the X twice quickly deleted one row and pasted another.
        if ((ev->type() == QEvent::MouseButtonPress || ev->type() == QEvent::MouseButtonDblClick)
            && (watched == m_list->viewport() || watched == m_starList->viewport())) {
            auto *me = static_cast<QMouseEvent *>(ev);
            auto *view = qobject_cast<QAbstractItemView *>(qobject_cast<QWidget *>(watched)->parentWidget());
            const QModelIndex idx = view->indexAt(me->pos());
            if (idx.isValid()) {
                const QRect row = view->visualRect(idx);
                const Entry *e = m_store.entryFor(idx.data(Qt::UserRole).toInt());
                if (e) {
                    const QPoint p = me->pos();
                    if (starButtonRect(row).contains(p)) {
                        traceClick(QStringLiteral("STAR row=%1 pos=%2,%3")
                                       .arg(idx.row())
                                       .arg(p.x())
                                       .arg(p.y()));
                        setStar(e->uuid, !e->starred);
                        return true; // consumed: must not also select/paste
                    }
                    if (removeButtonRect(row).contains(p)) {
                        traceClick(QStringLiteral("REMOVE row=%1 pos=%2,%3")
                                       .arg(idx.row())
                                       .arg(p.x())
                                       .arg(p.y()));
                        deleteEntry(e->uuid);
                        return true;
                    }
                }
            }
            return false;
        }
        if (ev->type() == QEvent::Leave && (watched == m_list->viewport() || watched == m_starList->viewport())) {
            setHoverRow(-1);
            return false;
        }
        return QWidget::eventFilter(watched, ev);
    }

    void contextMenu(const QPoint &pos)
    {
        auto *lw = qobject_cast<QListWidget *>(sender());
        auto *it = lw->itemAt(pos);
        if (!it) {
            return;
        }
        const int idx = it->data(Qt::UserRole).toInt();
        if (idx < 0 || idx >= m_store.entries.size()) {
            return;
        }
        const Entry e = m_store.entries.at(idx);

        QMenu menu;
        menu.setAttribute(Qt::WA_DeleteOnClose);
        QAction *star = menu.addAction(e.starred ? QStringLiteral("Unstar") : QStringLiteral("Star"));
        QAction *copy = menu.addAction(QStringLiteral("Copy only"));
        menu.addSeparator();
        QAction *del = menu.addAction(QStringLiteral("Delete entry"));
        QAction *chosen = menu.exec(lw->viewport()->mapToGlobal(pos));
        if (chosen == star) {
            setStar(e.uuid, !e.starred);
        } else if (chosen == copy) {
            applyEntry(e);
            flash(QStringLiteral("Copied."));
        } else if (chosen == del) {
            deleteEntry(e.uuid);
        }
    }

    void deleteEntry(const QString &uuid)
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("ccdel"));
        db.setDatabaseName(klipperDir() + QStringLiteral("/history3.sqlite"));
        if (db.open()) {
            QSqlQuery q(db);
            for (const QString &sql : {QStringLiteral("DELETE FROM main WHERE uuid=?"),
                                       QStringLiteral("DELETE FROM aux WHERE uuid=?"),
                                       QStringLiteral("DELETE FROM aux WHERE data_uuid=?")}) {
                q.prepare(sql);
                q.addBindValue(uuid);
                q.exec();
            }
            db.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("ccdel"));
        reload();
        flash(QStringLiteral("Entry deleted."));
    }

    void applyEntry(const Entry &e)
    {
        if (e.isImage) {
            const QImage img = loadImage(e);
            if (img.isNull()) {
                flash(QStringLiteral("Could not read that image"));
                return;
            }
            QGuiApplication::clipboard()->setImage(img);
        } else {
            setClipboardText(e.text);
        }
    }

    void choose(QListWidget *lw, QListWidgetItem *it)
    {
        if (!it) {
            return;
        }
        const int idx = it->data(Qt::UserRole).toInt();
        if (idx < 0 || idx >= m_store.entries.size()) {
            return;
        }
        const Entry e = m_store.entries.at(idx);
        ++m_chooseCount;
        applyEntry(e);

        const QSettings cfg;
        const int delay = cfg.value(QStringLiteral("pasteDelayMs"), 200).toInt();
        const bool autoPaste = cfg.value(QStringLiteral("autoPaste"), true).toBool();

        if (!autoPaste) {
            flash(QStringLiteral("Copied (auto-paste off)."));
            QTimer::singleShot(1600, this, [this] { hide(); });
            return;
        }

        // Distinguish "no ydotool at all" from "ydotoold is not running".
        // The second case looks identical to a broken paste unless we say so.
        if (!haveYdotool()) {
            flash(QStringLiteral("Copied. Install ydotool to enable click-to-paste."));
            QTimer::singleShot(1600, this, [this] { hide(); });
            return;
        }
        if (!ydotoolReady()) {
            traceClick(QStringLiteral("PASTE-BLOCKED ydotoold unavailable"));
            flash(QStringLiteral("Copied, but ydotoold is not running - cannot paste.\n"
                                 "Start it with: systemctl --user start ydotool"));
            QTimer::singleShot(4000, this, [this] { hide(); });
            return;
        }
        hide();
        QTimer::singleShot(delay, this, &Deck::injectPaste);
    }

    void injectPaste()
    {
        if (isVisible()) {
            return;
        }
        QProcess::startDetached(QStringLiteral("ydotool"),
                                {QStringLiteral("key"), QStringLiteral("29:1"), QStringLiteral("47:1"),
                                 QStringLiteral("29:0"), QStringLiteral("47:0")});
    }

    QWidget *root = nullptr;
    bool m_deactivateArmed = false;
    int m_chooseCount = 0; // pastes attempted; lets tests assert on the paste
    QPoint m_cursorOverride;
    QTabBar *m_tabBar = nullptr;
    QStackedWidget *m_stack = nullptr;
    QListWidget *m_list = nullptr;
    QListWidget *m_starList = nullptr;
    QLineEdit *m_search = nullptr;
    QLineEdit *m_newEntry = nullptr;
    QLabel *m_count = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_newBtn = nullptr;
    QPushButton *m_clearBtn = nullptr;
    QWidget *m_overlay = nullptr;
    QLabel *m_confirmTitle = nullptr;
    QLabel *m_confirmBody = nullptr;
    QComboBox *m_sort = nullptr;
    RowDelegate *m_delegate = nullptr;
    Store m_store;
    SortMode m_sortMode = Newest;
};

// ---------------------------------------------------------------------------
// Headless test suite. Write paths run against a throwaway copy of the history
// database so the user's real clipboard is never modified.
// ---------------------------------------------------------------------------
int runTests(Deck *deck)
{
    int failures = 0;
    int total = 0;
    auto check = [&](bool ok, const QString &what) {
        printf("  %-52s %s\n", qPrintable(what), ok ? "PASS" : "FAIL");
        ++total;
        if (!ok) {
            ++failures;
        }
    };

    // Redirect the data directory for the WHOLE suite before anything reads it.
//
// This used to be scoped to the write-path block alone, which was a trap: every
// check after that block ran against the user's real Klipper database. The
// double-click regression test clicks the real ✕ button, so it deleted genuine
// clipboard entries on each run. Anything that can delete must be sandboxed for
// the entire suite, not just the part that was known to be destructive.
const QString tmpDir = QDir(QDir::tempPath()).absoluteFilePath(QStringLiteral("custom-clipboard-test"));
    // Resolve the REAL database path BEFORE redirecting, or the "real" path
    // would silently become the sandbox and every seeding check would compare
    // the sandbox against itself.
    const QString realDbPath = realKlipperDbPath();
    QDir(tmpDir).removeRecursively();
    QDir().mkpath(tmpDir);
    qputenv("CUSTOM_CLIPBOARD_DATA", tmpDir.toLocal8Bit());

    // Verify the redirect took effect before running anything destructive. A
    // suite that can silently fall back to the real database is worse than no
    // suite at all, so refuse to continue rather than run unguarded.
    if (klipperDir() == realDbPath || !klipperDir().contains(tmpDir)) {
        fprintf(stderr, "FATAL: sandbox not active (klipperDir=%s, real=%s).\n"
                        "       Refusing to run: an unsandboxed suite would DELETE the\n"
                        "       real clipboard history.\n",
                qPrintable(klipperDir()), qPrintable(realDbPath));
        return 2;
    }

    printf("\n== read path ==\n");
    const int realCountAtStart = readRealHistorySize(realDbPath);
    check(realCountAtStart >= 0, "real history database is readable");
    check(realCountAtStart > 0,
          "real history is not empty (an empty clipboard makes every read/write check vacuous)");
    // Re-seedable, because the write-path checks deliberately empty the sandbox.
    auto seedSandbox = [&] {
        for (const QString &f : QDir(tmpDir).entryList(QDir::Files)) {
            QFile::remove(tmpDir + QLatin1Char('/') + f);
        }
        // Deliberately NOT copying -shm: a stale shared-memory index alongside a
        // copied WAL makes readers see the wrong snapshot.
        for (const QString &suffix : {QStringLiteral(""), QStringLiteral("-wal")}) {
            const QString s = realDbPath + suffix;
            if (QFile::exists(s)) {
                QFile::copy(s, tmpDir + QStringLiteral("/history3.sqlite") + suffix);
            }
        }
    };
    seedSandbox();
    check(readHistory().size() == realCountAtStart, "sandbox seeded from the real database");
    QVector<Entry> base = readHistory();
    check(!base.isEmpty(), "history reads non-empty from SQLite");
    int starred = 0, images = 0;
    for (const Entry &e : base) {
        starred += e.starred ? 1 : 0;
        images += e.isImage ? 1 : 0;
    }
    printf("     (%d entries, %d starred, %d images)\n", int(base.size()), starred, images);
    bool thumbsOk = true;
    for (const Entry &e : base) {
        if (e.isImage && loadImage(e).isNull()) {
            thumbsOk = false;
        }
    }
    check(images == 0 || thumbsOk, "image payloads resolve to thumbnails");

    printf("\n== sorting (starred must NOT be pinned) ==\n");
    auto sorted = [&](SortMode m) {
        QVector<Entry> v = base;
        sortEntries(v, m);
        return v;
    };
    auto monotonic = [](const QVector<Entry> &v, bool desc, bool alpha) {
        for (int i = 1; i < v.size(); ++i) {
            const Entry &p = v.at(i - 1);
            const Entry &c = v.at(i);
            if (alpha) {
                const int cmp = p.text.compare(c.text, Qt::CaseInsensitive);
                if (desc ? (cmp < 0) : (cmp > 0)) {
                    return false;
                }
            } else {
                if (desc ? (p.when < c.when) : (p.when > c.when)) {
                    return false;
                }
            }
        }
        return true;
    };
    check(monotonic(sorted(Newest), true, false), "Newest sorts by recency, descending");
    check(monotonic(sorted(Oldest), false, false), "Oldest sorts by recency, ascending");
    check(monotonic(sorted(AlphaAsc), false, true), "A-Z sorts case-insensitively");
    check(monotonic(sorted(AlphaDesc), true, true), "Z-A sorts case-insensitively");

    // Deterministic fixture: never assert on whatever the user's live history
    // happens to contain (it may be entirely starred, or entirely plain).
    {
        QVector<Entry> fx;
        auto mk = [](const char *id, qint64 when, bool starred, const char *text) {
            Entry e;
            e.uuid = QString::fromLatin1(id);
            e.when = when;
            e.starred = starred;
            e.text = QString::fromLatin1(text);
            return e;
        };
        fx << mk("a", 300, false, "alpha") << mk("b", 200, true, "beta") << mk("c", 100, false, "gamma");
        sortEntries(fx, Newest);
        check(fx.size() == 3 && fx.at(0).uuid == QLatin1String("a") && fx.at(1).uuid == QLatin1String("b")
                  && fx.at(2).uuid == QLatin1String("c"),
              "starred entries are not pinned above newer plain ones");

        QVector<Entry> fxa = fx;
        sortEntries(fxa, AlphaAsc);
        check(fxa.at(0).text == QLatin1String("alpha") && fxa.at(2).text == QLatin1String("gamma"),
              "A-Z ordering works on the fixture");
    }

    printf("\n== search & tab filtering ==\n");
    const QVector<int> all = filterRows(base, QString(), false);
    check(all.size() == base.size(), "empty search returns every entry");
    const QVector<int> stars = filterRows(base, QString(), true);
    bool onlyStarred = true;
    for (int i : stars) {
        onlyStarred = onlyStarred && base.at(i).starred;
    }
    check(stars.size() == starred && onlyStarred, "starred filter returns only starred");
    if (!base.isEmpty()) {
        const QString probe = base.first().text.left(6);
        if (!probe.isEmpty()) {
            const QVector<int> hit = filterRows(base, probe, false);
            bool allHit = !hit.isEmpty();
            for (int i : hit) {
                allHit = allHit && base.at(i).text.contains(probe, Qt::CaseInsensitive);
            }
            check(allHit, "search narrows to matches only");
            check(filterRows(base, QStringLiteral("zzz-no-such-thing"), false).isEmpty(), "no-match search returns empty");
        }
    }

    printf("\n== write path (sandboxed) ==\n");
    // Already sandboxed for the whole suite; nothing below may touch the real DB.
    {
        const QVector<Entry> sandboxed = readHistory();
        check(sandboxed.size() == base.size(), "sandbox reads the same entry count");

            // Star / unstar through the same code path the UI uses.
            int target = -1;
            for (int i = 0; i < sandboxed.size(); ++i) {
                if (!sandboxed.at(i).starred) {
                    target = i;
                    break;
                }
            }
            if (target >= 0) {
                const QString uuid = sandboxed.at(target).uuid;
                auto flagIs = [&](const QString &u) {
                    for (const Entry &e : readHistory()) {
                        if (e.uuid == u) {
                            return e.starred;
                        }
                    }
                    return false;
                };

                deck->reload();
                deck->setStar(uuid, true);
                check(flagIs(uuid), "star button sets starred=true (UI path)");

                deck->setStar(uuid, false);
                check(!flagIs(uuid), "star button clears starred (UI path)");

                // and it must survive a reload the way the popup sees it
                deck->reload();
                check(deck->starredCount() == starred, "starred count unchanged after round-trip");
            }

            // Real mouse clicks on the painted hover buttons. Row-body clicks are
            // deliberately NOT tested here: that path ends in a ydotool key
            // injection, which must never run against the live desktop.
            deck->showForTest();
            // Make the fixture deterministic: guarantee an unstarred row exists.
            if (deck->firstNonStarredRow() < 0 && deck->testList()->count() > 0) {
                deck->setStar(deck->uuidAtRow(0), false);
                deck->showForTest();
            }
            const int row = deck->firstNonStarredRow();
            const QString rowUuid = deck->uuidAtRow(row);
            check(row >= 0 && !rowUuid.isEmpty(), "found a visible non-starred row to click");

            if (row >= 0) {
                auto flagIs = [&](const QString &u) {
                    for (const Entry &e : readHistory()) {
                        if (e.uuid == u) {
                            return e.starred;
                        }
                    }
                    return false;
                };
                const bool rowStarred = flagIs(rowUuid);

                const QRect rect = deck->rowRectFor(row);
                QWidget *vp = deck->testList()->viewport();

                // hover first, exactly as a user would
                Deck::hoverAt(vp, rect.center());
                check(deck->hoverRow() == row, "hovering a row reveals its action buttons");

                Deck::clickAt(vp, starButtonRect(rect).center());
                check(flagIs(rowUuid) != rowStarred, "clicking the STAR button toggles starred");

                // Geometry contract: the star keeps its verified position and
                // the two buttons never overlap.
                check(!starButtonRect(rect).intersects(removeButtonRect(rect)),
                      "star and remove buttons do not overlap");
                check(removeButtonRect(rect).right() == rect.right() - 11,
                      "remove stays anchored 10px from the row edge");
                check(starButtonRect(rect).right() < removeButtonRect(rect).left(),
                      "star sits inboard (left) of remove");
                check(starButtonRect(rect).width() == removeButtonRect(rect).width(),
                      "both buttons are the same size");

                Deck::clickAt(vp, removeButtonRect(rect).center());
                bool gone = true;
                for (const Entry &e : readHistory()) {
                    if (e.uuid == rowUuid) {
                        gone = false;
                    }
                }
                check(gone, "clicking the REMOVE button deletes the entry");

                // a click on the row body must not be mistaken for a button
                deck->showForTest();
                const int body = deck->firstNonStarredRow();
                if (body >= 0) {
                    const QString bodyUuid = deck->uuidAtRow(body);
                    const QRect r2 = deck->rowRectFor(body);
                    const QRect starRect = starButtonRect(r2);
                    const QPoint bodyPoint(r2.left() + 20, r2.center().y());
                    check(!starRect.contains(bodyPoint) && !removeButtonRect(r2).contains(bodyPoint),
                          "row body does not overlap either button");
                    const bool before = flagIs(bodyUuid);
                    Deck::clickAt(deck->testList()->viewport(), bodyPoint);
                    bool stillThere = false;
                    for (const Entry &e : readHistory()) {
                        if (e.uuid == bodyUuid) {
                            stillThere = true;
                        }
                    }
                    check(stillThere, "row-body click does not delete the entry");
                    check(flagIs(bodyUuid) == before, "row-body click does not change starred");
                }
                deck->hide();
            }

            // delete
            const int beforeCount = readHistory().size();
            if (beforeCount > 0) {
                deck->reload();
                deck->deleteByIndex(0);
                check(readHistory().size() == beforeCount - 1, "deleting an entry removes exactly one row");
            }

            // Clear-all-unstarred must spare starred entries.
            deck->reload();
            // Ensure something unstarred exists so the clear has work to do.
            if (deck->unstarredCount() == 0 && deck->testList()->count() > 1) {
                deck->setStar(deck->uuidAtRow(1), false);
                deck->reload();
            }
            const int starredBefore = [&] {
                int n = 0;
                for (const Entry &e : readHistory()) {
                    n += e.starred ? 1 : 0;
                }
                return n;
            }();
            const int unstarredBefore = deck->unstarredCount();
            check(unstarredBefore > 0, "there are unstarred entries to clear");

            const int removed = deck->clearUnstarred();
            check(removed == unstarredBefore, "clear removes exactly the unstarred entries");

            const QVector<Entry> after = readHistory();
            int starredAfter = 0;
            int unstarredAfter = 0;
            for (const Entry &e : after) {
                starredAfter += e.starred ? 1 : 0;
                unstarredAfter += e.starred ? 0 : 1;
            }
            check(unstarredAfter == 0, "no unstarred entries survive a clear");
            check(starredAfter == starredBefore, "clear keeps every starred entry");
            check(deck->clearUnstarred() == 0, "clearing an already-clear list is a no-op");
    }

    // The one check that must see past the sandbox. Asserted against the real
    // file directly, since readHistory() would only ever report the sandbox.
    qunsetenv("CUSTOM_CLIPBOARD_DATA");
    check(readRealHistorySize(realDbPath) == realCountAtStart,
          "real database untouched by the tests");
    qputenv("CUSTOM_CLIPBOARD_DATA", tmpDir.toLocal8Bit());

    printf("\n== click-outside dismissal ==\n");
    // Regression: the panel used to hide on ANY WindowDeactivate. Clicking a row
    // deactivates us too (transient-parent churn / focus handover), so the hide
    // fired on the press, the row never saw its release, itemClicked never ran
    // and nothing was pasted. The pointer's position is the discriminator.
    deck->showForTest();
    deck->armDismissalForTest();
    const QRect frame = deck->frameGeometry();

    deck->setCursorPosForTest(frame.center());
    deck->deactivateForTest();
    check(deck->isVisible(), "deactivation with the pointer on the panel keeps it open (row clicks still paste)");

    deck->setCursorPosForTest(frame.topLeft() + QPoint(2, 2));
    deck->deactivateForTest();
    check(deck->isVisible(), "deactivation with the pointer on a row keeps it open");

    // Well clear of the panel: that is a genuine outside click.
    deck->setCursorPosForTest(frame.bottomRight() + QPoint(400, 400));
    deck->deactivateForTest();
    check(!deck->isVisible(), "deactivation with the pointer off the panel dismisses it");

    // The confirm card is part of our window: clicking Cancel/Clear on it is not
    // an outside click either.
    deck->showForTest();
    deck->armDismissalForTest();
    deck->setCursorPosForTest(frame.bottomRight() + QPoint(400, 400));
    deck->showConfirmForTest(QStringLiteral("t"), QStringLiteral("b"));
    deck->deactivateForTest();
    check(deck->isVisible(), "an open confirm card is not dismissed by deactivation");
    deck->hideConfirmForTest();
    deck->setCursorPosForTest(QPoint());
    deck->hide();

    printf("\n== status line must not move the list ==\n");
    // Regression: the status line used to be a layout row, so showing it resized
    // the list and shifted every row. A fast second click after an action then
    // landed on a different row's body and pasted it, instead of hitting the
    // button again. The rows must not move.
    deck->showForTest();
    QApplication::processEvents();
    const QRect rowBefore = deck->rowRectFor(0);
    const QRect starBefore = starButtonRect(rowBefore);
    const QRect removeBefore = removeButtonRect(rowBefore);
    const int listHBefore = deck->testList()->height();

    deck->flashForTest(QStringLiteral("Entry deleted."));
    QApplication::processEvents();

    const QRect rowAfter = deck->rowRectFor(0);
    check(rowAfter == rowBefore, "showing the status line does not move row 0");
    check(starButtonRect(rowAfter) == starBefore, "the star button stays put when a message shows");
    check(removeButtonRect(rowAfter) == removeBefore, "the remove button stays put when a message shows");
    check(deck->testList()->height() == listHBefore, "the list is not resized by the status line");
    // The message must be reachable but never eat clicks meant for the rows.
    check(deck->statusVisibleForTest(), "the status message is shown");
    check(deck->statusIsOverlayForTest(), "the status message is mouse-transparent so rows stay clickable");
    deck->hide();

    printf("\n== fast double-click on remove ==\n");
    // Reported: clicking the X twice quickly deleted one entry and pasted
    // another. Qt sends the second press as MouseButtonDblClick, which the
    // button hit-test ignored, so it fell through to the list and pasted.
    // doubleClickRemoveForTest() replays that exact sequence.
    {
        // The write-path checks above clear the sandbox, so re-seed before
        // clicking anything.
        seedSandbox();
        deck->reload();
        deck->showForTest();
        const int r = 0;
        const Deck::DoubleClickResult report = deck->doubleClickRemoveForTest(r);
        check(!report.pasted, "double-clicking the X button never pastes an entry");
        check(report.removed >= 1, "the first click of a double-click still deletes");
        // A double-click on the row *body* is still a normal paste.
        seedSandbox();
        deck->reload();
        deck->showForTest();
        deck->resetChooseCountForTest();
        const QPoint body = QPoint(20, deck->rowRectFor(0).center().y());
        Deck::clickAt(deck->testList()->viewport(), body);
        check(deck->chooseCountForTest() == 1, "a single click on the row body still pastes");
        deck->hide();
    }

    printf("\n== confirm popup ==\n");
    // Regression: the panel widget was a constructor *local*, so the member
    // stayed null and showing the dialog segfaulted on root->rect().
    check(deck->rootWidget() != nullptr, "panel root widget is constructed");
    deck->showForTest();
    deck->showConfirmForTest(QStringLiteral("t"), QStringLiteral("b"));
    check(deck->confirmVisibleForTest(), "confirm popup can be shown without crashing");
    deck->hideConfirmForTest();
    check(!deck->confirmVisibleForTest(), "confirm popup can be dismissed");
    deck->hide();

    printf("\n== environment ==\n");
    check(haveYdotool(), "ydotool present (required for click-to-paste)");
    // The binary is installed but the daemon may be dead — that is the failure
    // that made click-to-paste silently do nothing, so assert on it explicitly.
    check(ydotoolReady(), "ydotoold reachable, so injection can actually work");
    check(QDBusInterface(QStringLiteral("org.kde.klipper"),
                         QStringLiteral("/klipper"),
                         QStringLiteral("org.kde.klipper.klipper"))
              .isValid(),
          "Klipper reachable on D-Bus");

    // Report the total rather than letting the docs quote a number. Some checks are
// conditional on fixture state, so the count legitimately varies.
    printf("\n%s  (%d/%d checks, %d failure%s)\n\n",
           failures ? "FAILURES" : "ALL TESTS PASSED",
           total - failures, total, failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    QCoreApplication::setApplicationName(QStringLiteral("custom-clipboard"));
    QCoreApplication::setOrganizationName(QStringLiteral("custom-clipboard"));
    QCoreApplication::setApplicationVersion(QStringLiteral("2.1.0"));

    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);

    const QStringList args = app.arguments();

    if (args.contains(QStringLiteral("--selftest"))) {
        const QVector<Entry> hist = readHistory();
        int stars = 0, imgs = 0;
        for (const Entry &e : hist) {
            stars += e.starred ? 1 : 0;
            imgs += e.isImage ? 1 : 0;
        }
        printf("entries: %d\nstarred: %d\nimages: %d\n", int(hist.size()), stars, imgs);
        printf("klipper db: %s\n", QFile::exists(klipperDir() + QStringLiteral("/history3.sqlite")) ? "ok" : "missing");
        printf("ydotool: %s\n", haveYdotool() ? "yes" : "NO");
        return 0;
    }

    if (args.contains(QStringLiteral("--test"))) {
        auto *deck = new Deck;
        return runTests(deck);
    }

    const int prevIdx = args.indexOf(QStringLiteral("--preview"));
    if (prevIdx != -1 && prevIdx + 1 < args.size()) {
        const QString dir = args.at(prevIdx + 1);
        QDir().mkpath(dir);
        auto *deck = new Deck;
        for (const QString &state : {QStringLiteral("all"), QStringLiteral("starred"),
                                     QStringLiteral("search"), QStringLiteral("alpha"),
                                     QStringLiteral("new")}) {
            deck->renderState(dir + QStringLiteral("/") + state + QStringLiteral(".png"), state);
            printf("wrote %s/%s.png\n", qPrintable(dir), qPrintable(state));
        }
        return 0;
    }

    const int shotIdx = args.indexOf(QStringLiteral("--screenshot"));
    if (shotIdx != -1 && shotIdx + 1 < args.size()) {
        auto *deck = new Deck;
        QString state = QStringLiteral("all");
        const int stIdx = args.indexOf(QStringLiteral("--state"));
        if (stIdx != -1 && stIdx + 1 < args.size()) {
            state = args.at(stIdx + 1);
        }
        deck->renderState(args.at(shotIdx + 1), state);
        return 0;
    }

    auto *deck = new Deck;

    auto *action = new QAction(QStringLiteral("Show Clipboard Deck"), deck);
    action->setObjectName(QStringLiteral("show-clipboard"));
    const QList<QKeySequence> keys{QKeySequence(Qt::META | Qt::Key_V)};
    KGlobalAccel::self()->setDefaultShortcut(action, keys);
    // NoAutoloading: otherwise a stale saved shortcut silently wins over this one.
    KGlobalAccel::self()->setShortcut(action, keys, KGlobalAccel::NoAutoloading);
    QObject::connect(action, &QAction::triggered, deck, [deck] { deck->toggle(); });

    return app.exec();
}