#pragma once

#include <QSharedPointer>
#include <QString>
#include <QVector>

#include "library/libraryfeature.h"
#include "util/parented_ptr.h"

class BaseExternalTrackModel;
class BaseTrackCache;
class TreeItemModel;
class WLibrary;
class WLibraryTextBrowser;

class YouTubeFeature final : public LibraryFeature {
    Q_OBJECT
  public:
    YouTubeFeature(Library* pLibrary, UserSettingsPointer pConfig);
    ~YouTubeFeature() override;

    QVariant title() override;
    static bool isSupported();
    void bindLibraryWidget(WLibrary* libraryWidget,
            KeyboardEventFilter* keyboard) override;
    TreeItemModel* sidebarModel() const override;

  public slots:
    void activate() override;
    void activateChild(const QModelIndex& index) override;
    void searchYouTube(const QString& query);

  private:
    struct SearchResult {
        QString id;
        QString title;
        QString uploader;
        int durationSeconds;
        QString sourceUrl;
    };

    void downloadTrackById(const QString& videoId);
    void renderSearchView(const QString& message = QString(),
            bool isError = false);
    bool locateYtDlp(QString* pExecutablePath, QString* pError) const;
    bool runYtDlpSearch(const QString& query,
            QVector<SearchResult>* pResults,
            QString* pError) const;
    bool runYtDlpDownload(const QString& sourceUrl,
            QString* pLocation,
            QString* pVideoId,
            QString* pTitle,
            QString* pUploader,
            int* pDurationSeconds,
            QString* pError) const;
    bool upsertDownloadedTrack(const QString& videoId,
            const QString& sourceUrl,
            const QString& title,
            const QString& uploader,
            int durationSeconds,
            const QString& location,
            QString* pError) const;
    void cleanupMissingDownloadedTracks();
    void refreshDownloadedModel();
    QString downloadDirectory() const;

    parented_ptr<TreeItemModel> m_pSidebarModel;
    BaseExternalTrackModel* m_pDownloadedTrackModel;
    QSharedPointer<BaseTrackCache> m_downloadedTrackSource;
    WLibraryTextBrowser* m_pSearchView;
    QVector<SearchResult> m_searchResults;
    QString m_lastQuery;
    QString m_title;
};
