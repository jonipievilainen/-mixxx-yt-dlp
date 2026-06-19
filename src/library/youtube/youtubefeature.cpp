#include "library/youtube/youtubefeature.h"

#include <QDir>
#include <QDebug>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QStandardPaths>

#include "library/baseexternaltrackmodel.h"
#include "library/basetrackcache.h"
#include "library/dao/trackschema.h"
#include "library/library.h"
#include "library/treeitem.h"
#include "library/treeitemmodel.h"
#include "moc_youtubefeature.cpp"
#include "track/trackref.h"
#include "util/assert.h"
#include "widget/wlibrary.h"
#include "widget/wlibrarytextbrowser.h"

namespace {

constexpr int kMaxSearchResults = 10;
constexpr int kSearchTimeoutMillis = 30000;
constexpr int kDownloadTimeoutMillis = 15 * 60 * 1000;
const QString kHomeViewName = QStringLiteral("YOUTUBEHOME");
const QString kSidebarDownloadedData = QStringLiteral("downloaded");

QString formatDuration(int totalSeconds) {
    if (totalSeconds <= 0) {
        return QString();
    }
    const int hours = totalSeconds / 3600;
    const int minutes = (totalSeconds % 3600) / 60;
    const int seconds = totalSeconds % 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
                .arg(hours)
                .arg(minutes, 2, 10, QChar('0'))
                .arg(seconds, 2, 10, QChar('0'));
    }
    return QStringLiteral("%1:%2")
            .arg(minutes)
            .arg(seconds, 2, 10, QChar('0'));
}

class YouTubeSearchView final : public WLibraryTextBrowser {
  public:
    explicit YouTubeSearchView(YouTubeFeature* pFeature, QWidget* parent)
            : WLibraryTextBrowser(parent),
              m_pFeature(pFeature) {
    }

    void onSearch(const QString& text) override {
        VERIFY_OR_DEBUG_ASSERT(m_pFeature) {
            return;
        }
        m_pFeature->searchYouTube(text);
    }

  private:
    YouTubeFeature* m_pFeature;
};

} // namespace

YouTubeFeature::YouTubeFeature(Library* pLibrary, UserSettingsPointer pConfig)
        : LibraryFeature(pLibrary, pConfig, QStringLiteral("youtube")),
          m_pSidebarModel(make_parented<TreeItemModel>(this)),
          m_pDownloadedTrackModel(nullptr),
          m_pSearchView(nullptr),
          m_title(tr("YouTube")) {
    QStringList columns = {
            LIBRARYTABLE_ID,
            LIBRARYTABLE_ARTIST,
            LIBRARYTABLE_TITLE,
            LIBRARYTABLE_ALBUM,
            LIBRARYTABLE_YEAR,
            LIBRARYTABLE_GENRE,
            LIBRARYTABLE_TRACKNUMBER,
            TRACKLOCATIONSTABLE_LOCATION,
            LIBRARYTABLE_COMMENT,
            LIBRARYTABLE_DURATION,
            LIBRARYTABLE_BITRATE,
            LIBRARYTABLE_BPM,
            LIBRARYTABLE_RATING,
            LIBRARYTABLE_FILETYPE,
            LIBRARYTABLE_DATETIMEADDED};
    QStringList searchColumns = {
            LIBRARYTABLE_ARTIST,
            LIBRARYTABLE_TITLE,
            LIBRARYTABLE_ALBUM,
            TRACKLOCATIONSTABLE_LOCATION,
            LIBRARYTABLE_COMMENT};

    m_downloadedTrackSource = QSharedPointer<BaseTrackCache>::create(
            m_pLibrary->trackCollectionManager()->internalCollection(),
            QStringLiteral("youtube_library"),
            LIBRARYTABLE_ID,
            std::move(columns),
            std::move(searchColumns),
            false);

    m_pDownloadedTrackModel = new BaseExternalTrackModel(this,
            m_pLibrary->trackCollectionManager(),
            "mixxx.db.model.youtube.downloaded",
            QStringLiteral("youtube_library"),
            m_downloadedTrackSource);

    auto pRootItem = TreeItem::newRoot(this);
    pRootItem->appendChild(tr("Downloaded Tracks"), kSidebarDownloadedData);
    m_pSidebarModel->setRootItem(std::move(pRootItem));

    refreshDownloadedModel();
}

YouTubeFeature::~YouTubeFeature() {
    delete m_pDownloadedTrackModel;
}

QVariant YouTubeFeature::title() {
    return m_title;
}

bool YouTubeFeature::isSupported() {
    return true;
}

void YouTubeFeature::bindLibraryWidget(WLibrary* libraryWidget,
        KeyboardEventFilter* keyboard) {
    Q_UNUSED(keyboard);
    auto* pSearchView = new YouTubeSearchView(this, libraryWidget);
    pSearchView->setOpenLinks(false);
    connect(pSearchView,
            &WLibraryTextBrowser::anchorClicked,
            this,
            [this](const QUrl& link) {
                if (link.scheme() == QStringLiteral("download")) {
                    const QString videoId = link.path().trimmed();
                    if (!videoId.isEmpty()) {
                        downloadTrackById(videoId);
                    }
                }
            });
    m_pSearchView = pSearchView;
    libraryWidget->registerView(kHomeViewName, pSearchView);
    renderSearchView();
}

TreeItemModel* YouTubeFeature::sidebarModel() const {
    return m_pSidebarModel;
}

void YouTubeFeature::activate() {
    emit switchToView(kHomeViewName);
    emit restoreSearch(m_lastQuery);
    renderSearchView();
    emit enableCoverArtDisplay(false);
}

void YouTubeFeature::activateChild(const QModelIndex& index) {
    if (!index.isValid()) {
        return;
    }
    auto* pItem = static_cast<TreeItem*>(index.internalPointer());
    VERIFY_OR_DEBUG_ASSERT(pItem) {
        return;
    }

    if (pItem->getData().toString() == kSidebarDownloadedData) {
        cleanupMissingDownloadedTracks();
        refreshDownloadedModel();
        emit showTrackModel(m_pDownloadedTrackModel);
        emit enableCoverArtDisplay(false);
    }
}

void YouTubeFeature::searchYouTube(const QString& query) {
    m_lastQuery = query;
    if (query.trimmed().isEmpty()) {
        m_searchResults.clear();
        renderSearchView();
        return;
    }

    QVector<SearchResult> results;
    QString error;
    if (!runYtDlpSearch(query, &results, &error)) {
        m_searchResults.clear();
        renderSearchView(error, true);
        return;
    }

    m_searchResults = std::move(results);
    renderSearchView();
}

void YouTubeFeature::downloadTrackById(const QString& videoId) {
    QString location;
    QString downloadedVideoId;
    QString title;
    QString uploader;
    int durationSeconds = 0;
    QString error;
    const QString sourceUrl = QStringLiteral("https://www.youtube.com/watch?v=%1").arg(videoId);

    if (!runYtDlpDownload(sourceUrl,
                &location,
                &downloadedVideoId,
                &title,
                &uploader,
                &durationSeconds,
                &error)) {
        renderSearchView(error, true);
        return;
    }

    QString upsertError;
    if (!upsertDownloadedTrack(downloadedVideoId,
                sourceUrl,
                title,
                uploader,
                durationSeconds,
                location,
                &upsertError)) {
        renderSearchView(upsertError, true);
        return;
    }

    const auto downloadedTrack =
            m_pLibrary->trackCollectionManager()->getOrAddTrack(TrackRef::fromFilePath(location));
    if (!downloadedTrack) {
        renderSearchView(tr("Download completed, but Mixxx could not load the file."), true);
        return;
    }
    refreshDownloadedModel();
    renderSearchView(tr("Download complete: %1").arg(title));
}

void YouTubeFeature::renderSearchView(const QString& message, bool isError) {
    if (!m_pSearchView) {
        return;
    }

    QSet<QString> downloadedIds;
    QSqlQuery downloadedQuery(m_pLibrary->trackCollectionManager()->internalCollection()->database());
    downloadedQuery.prepare(QStringLiteral("SELECT youtube_id FROM youtube_library"));
    if (downloadedQuery.exec()) {
        while (downloadedQuery.next()) {
            downloadedIds.insert(downloadedQuery.value(0).toString());
        }
    } else {
        qWarning() << "Failed to query downloaded YouTube IDs";
    }

    QString html;
    html += QStringLiteral("<h2>%1</h2>").arg(tr("YouTube"));
    html += QStringLiteral("<p>%1</p>").arg(
            tr("Use the Library search field to search YouTube. Click a result to download audio."));

    if (!message.isEmpty()) {
        const QString color = isError ? QStringLiteral("#d0021b") : QStringLiteral("#0496FF");
        html += QStringLiteral("<p><span style=\"color:%1;\">%2</span></p>")
                        .arg(color, message.toHtmlEscaped());
    }

    if (!m_lastQuery.trimmed().isEmpty()) {
        html += QStringLiteral("<p><b>%1</b> %2</p>")
                        .arg(tr("Query:"), m_lastQuery.toHtmlEscaped());
    }

    if (m_searchResults.isEmpty()) {
        html += QStringLiteral("<p>%1</p>")
                        .arg(tr("No search results yet."));
    } else {
        html += QStringLiteral("<ol>");
        for (const auto& result : std::as_const(m_searchResults)) {
            const QString escapedTitle = result.title.toHtmlEscaped();
            const QString escapedUploader = result.uploader.toHtmlEscaped();
            const QString duration = formatDuration(result.durationSeconds);
            const bool isDownloaded = downloadedIds.contains(result.id);
            html += QStringLiteral("<li><b>%1</b>").arg(escapedTitle);
            if (!escapedUploader.isEmpty()) {
                html += QStringLiteral(" — %1").arg(escapedUploader);
            }
            if (!duration.isEmpty()) {
                html += QStringLiteral(" (%1)").arg(duration);
            }
            if (isDownloaded) {
                html += QStringLiteral(" — %1").arg(tr("Already downloaded"));
            } else {
                html += QStringLiteral(" — <a style=\"color:#0496FF;\" href=\"download:%1\">%2</a>")
                                .arg(result.id.toHtmlEscaped(), tr("Download audio").toHtmlEscaped());
            }
            html += QStringLiteral("</li>");
        }
        html += QStringLiteral("</ol>");
    }

    m_pSearchView->setHtml(html);
}

bool YouTubeFeature::locateYtDlp(QString* pExecutablePath, QString* pError) const {
    const QString executable = QStandardPaths::findExecutable(QStringLiteral("yt-dlp"));
    if (executable.isEmpty()) {
        if (pError) {
            *pError = tr("yt-dlp was not found in PATH. Install yt-dlp to use the YouTube feature.");
        }
        return false;
    }
    if (pExecutablePath) {
        *pExecutablePath = executable;
    }
    return true;
}

bool YouTubeFeature::runYtDlpSearch(const QString& query,
        QVector<SearchResult>* pResults,
        QString* pError) const {
    VERIFY_OR_DEBUG_ASSERT(pResults) {
        return false;
    }

    QString executable;
    if (!locateYtDlp(&executable, pError)) {
        return false;
    }

    QProcess process;
    QStringList args = {
            QStringLiteral("--flat-playlist"),
            QStringLiteral("--dump-json"),
            QStringLiteral("--skip-download"),
            QStringLiteral("--no-warnings"),
            QStringLiteral("ytsearch%1:%2").arg(kMaxSearchResults).arg(query)};

    process.start(executable, args);
    if (!process.waitForStarted()) {
        if (pError) {
            *pError = tr("Failed to start yt-dlp search process.");
        }
        return false;
    }
    if (!process.waitForFinished(kSearchTimeoutMillis)) {
        process.kill();
        if (pError) {
            *pError = tr("yt-dlp search timed out.");
        }
        return false;
    }

    const QString stdOut = QString::fromUtf8(process.readAllStandardOutput());
    const QString stdErr = QString::fromUtf8(process.readAllStandardError()).trimmed();

    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (pError) {
            *pError = stdErr.isEmpty() ? tr("yt-dlp search failed.") : stdErr;
        }
        return false;
    }

    QVector<SearchResult> results;
    const QStringList lines = stdOut.split(QRegularExpression(QStringLiteral("[\r\n]+")),
            Qt::SkipEmptyParts);
    results.reserve(lines.size());
    for (const QString& line : lines) {
        const QJsonDocument doc = QJsonDocument::fromJson(line.toUtf8());
        if (!doc.isObject()) {
            continue;
        }
        const QJsonObject obj = doc.object();
        const QString id = obj.value(QStringLiteral("id")).toString().trimmed();
        if (id.isEmpty()) {
            continue;
        }
        SearchResult result;
        result.id = id;
        result.title = obj.value(QStringLiteral("title")).toString().trimmed();
        result.uploader = obj.value(QStringLiteral("uploader")).toString().trimmed();
        result.durationSeconds = obj.value(QStringLiteral("duration")).toInt();
        QString webpageUrl = obj.value(QStringLiteral("webpage_url")).toString().trimmed();
        if (webpageUrl.isEmpty()) {
            webpageUrl = QStringLiteral("https://www.youtube.com/watch?v=%1").arg(id);
        }
        result.sourceUrl = webpageUrl;
        results.push_back(result);
    }

    *pResults = std::move(results);
    return true;
}

bool YouTubeFeature::runYtDlpDownload(const QString& sourceUrl,
        QString* pLocation,
        QString* pVideoId,
        QString* pTitle,
        QString* pUploader,
        int* pDurationSeconds,
        QString* pError) const {
    QString executable;
    if (!locateYtDlp(&executable, pError)) {
        return false;
    }

    const QString outputDirectory = downloadDirectory();
    if (outputDirectory.isEmpty()) {
        if (pError) {
            *pError = tr("Could not determine a download directory for YouTube audio.");
        }
        return false;
    }

    QProcess process;
    QStringList args = {
            QStringLiteral("--no-playlist"),
            QStringLiteral("--extract-audio"),
            QStringLiteral("--audio-format"),
            QStringLiteral("m4a"),
            QStringLiteral("--audio-quality"),
            QStringLiteral("0"),
            QStringLiteral("--no-warnings"),
            QStringLiteral("--print"),
            QStringLiteral("after_move:filepath=%(filepath)s"),
            QStringLiteral("--print"),
            QStringLiteral("id=%(id)s"),
            QStringLiteral("--print"),
            QStringLiteral("title=%(title)s"),
            QStringLiteral("--print"),
            QStringLiteral("uploader=%(uploader)s"),
            QStringLiteral("--print"),
            QStringLiteral("duration=%(duration)s"),
            QStringLiteral("-o"),
            outputDirectory + QStringLiteral("/%(id)s.%(ext)s"),
            sourceUrl};

    process.start(executable, args);
    if (!process.waitForStarted()) {
        if (pError) {
            *pError = tr("Failed to start yt-dlp download process.");
        }
        return false;
    }
    if (!process.waitForFinished(kDownloadTimeoutMillis)) {
        process.kill();
        if (pError) {
            *pError = tr("yt-dlp download timed out.");
        }
        return false;
    }

    const QString stdOut = QString::fromUtf8(process.readAllStandardOutput());
    const QString stdErr = QString::fromUtf8(process.readAllStandardError()).trimmed();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (pError) {
            *pError = stdErr.isEmpty() ? tr("yt-dlp download failed.") : stdErr;
        }
        return false;
    }

    QString filePath;
    QString videoId;
    QString title;
    QString uploader;
    int durationSeconds = 0;

    const QStringList lines = stdOut.split(QRegularExpression(QStringLiteral("[\r\n]+")),
            Qt::SkipEmptyParts);
    for (const QString& line : lines) {
        if (line.startsWith(QStringLiteral("filepath="))) {
            filePath = line.mid(QStringLiteral("filepath=").size()).trimmed();
        } else if (line.startsWith(QStringLiteral("id="))) {
            videoId = line.mid(QStringLiteral("id=").size()).trimmed();
        } else if (line.startsWith(QStringLiteral("title="))) {
            title = line.mid(QStringLiteral("title=").size()).trimmed();
        } else if (line.startsWith(QStringLiteral("uploader="))) {
            uploader = line.mid(QStringLiteral("uploader=").size()).trimmed();
        } else if (line.startsWith(QStringLiteral("duration="))) {
            durationSeconds = line.mid(QStringLiteral("duration=").size()).trimmed().toInt();
        }
    }

    if (filePath.isEmpty() || videoId.isEmpty()) {
        if (pError) {
            *pError = tr("yt-dlp returned incomplete metadata for the downloaded track.");
        }
        return false;
    }

    const QString normalizedLocation = QDir::fromNativeSeparators(filePath);
    if (!QFileInfo::exists(normalizedLocation)) {
        if (pError) {
            *pError = tr("yt-dlp reported a file path but no file was found: %1")
                              .arg(normalizedLocation);
        }
        return false;
    }

    if (pLocation) {
        *pLocation = normalizedLocation;
    }
    if (pVideoId) {
        *pVideoId = videoId;
    }
    if (pTitle) {
        *pTitle = title;
    }
    if (pUploader) {
        *pUploader = uploader;
    }
    if (pDurationSeconds) {
        *pDurationSeconds = durationSeconds;
    }
    return true;
}

bool YouTubeFeature::upsertDownloadedTrack(const QString& videoId,
        const QString& sourceUrl,
        const QString& title,
        const QString& uploader,
        int durationSeconds,
        const QString& location,
        QString* pError) const {
    QSqlDatabase db = m_pLibrary->trackCollectionManager()->internalCollection()->database();
    QSqlQuery query(db);
    query.prepare(
            QStringLiteral(
                    "INSERT INTO youtube_library ("
                    "youtube_id, source_url, title, artist, album, year, genre, tracknumber, "
                    "location, comment, duration, bitrate, bpm, rating, filetype"
                    ") VALUES ("
                    ":youtube_id, :source_url, :title, :artist, :album, :year, :genre, :tracknumber, "
                    ":location, :comment, :duration, :bitrate, :bpm, :rating, :filetype"
                    ") ON CONFLICT(youtube_id) DO UPDATE SET "
                    "source_url = excluded.source_url, "
                    "title = excluded.title, "
                    "artist = excluded.artist, "
                    "album = excluded.album, "
                    "location = excluded.location, "
                    "duration = excluded.duration, "
                    "filetype = excluded.filetype"));

    query.bindValue(QStringLiteral(":youtube_id"), videoId);
    query.bindValue(QStringLiteral(":source_url"), sourceUrl);
    query.bindValue(QStringLiteral(":title"), title);
    query.bindValue(QStringLiteral(":artist"), uploader);
    query.bindValue(QStringLiteral(":album"), QStringLiteral("YouTube"));
    query.bindValue(QStringLiteral(":year"), QString());
    query.bindValue(QStringLiteral(":genre"), QString());
    query.bindValue(QStringLiteral(":tracknumber"), QString());
    query.bindValue(QStringLiteral(":location"), location);
    query.bindValue(QStringLiteral(":comment"), sourceUrl);
    query.bindValue(QStringLiteral(":duration"), durationSeconds);
    query.bindValue(QStringLiteral(":bitrate"), 0);
    query.bindValue(QStringLiteral(":bpm"), 0.0);
    query.bindValue(QStringLiteral(":rating"), 0);
    query.bindValue(QStringLiteral(":filetype"), QFileInfo(location).suffix().toLower());

    if (!query.exec()) {
        if (pError) {
            *pError = tr("Failed to store downloaded YouTube track in the library database.");
        }
        return false;
    }

    return true;
}

void YouTubeFeature::cleanupMissingDownloadedTracks() {
    QSqlDatabase db = m_pLibrary->trackCollectionManager()->internalCollection()->database();
    QSqlQuery selectQuery(db);
    selectQuery.prepare(QStringLiteral("SELECT id, location FROM youtube_library"));
    if (!selectQuery.exec()) {
        return;
    }

    QList<int> idsToDelete;
    while (selectQuery.next()) {
        const int id = selectQuery.value(0).toInt();
        const QString location = selectQuery.value(1).toString();
        if (location.isEmpty()) {
            idsToDelete.append(id);
            continue;
        }
        if (!QFileInfo::exists(location)) {
            idsToDelete.append(id);
        }
    }

    if (idsToDelete.isEmpty()) {
        return;
    }

    QSqlQuery deleteQuery(db);
    QStringList placeholders;
    placeholders.reserve(idsToDelete.size());
    for (int i = 0; i < idsToDelete.size(); ++i) {
        placeholders.append(QStringLiteral("?"));
    }
    deleteQuery.prepare(QStringLiteral("DELETE FROM youtube_library WHERE id IN (%1)")
                                .arg(placeholders.join(QStringLiteral(","))));
    for (int id : std::as_const(idsToDelete)) {
        deleteQuery.addBindValue(id);
    }
    if (!deleteQuery.exec()) {
        qWarning() << "Failed to remove missing YouTube tracks from youtube_library";
    }
}

void YouTubeFeature::refreshDownloadedModel() {
    m_downloadedTrackSource->buildIndex();
    m_pDownloadedTrackModel->select();
}

QString YouTubeFeature::downloadDirectory() const {
    // Prefer app-local storage for files managed by this feature. If unavailable,
    // fall back to the user's music directory.
    QString baseDirectory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (baseDirectory.isEmpty()) {
        baseDirectory = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    }
    if (baseDirectory.isEmpty()) {
        return QString();
    }

    QDir dir(baseDirectory);
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        return QString();
    }
    const QString relativePath = QStringLiteral("youtube-audio");
    if (!dir.mkpath(relativePath)) {
        return QString();
    }
    return dir.filePath(relativePath);
}
