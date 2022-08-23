/*
    This file is part of the KDE project.

    SPDX-FileCopyrightText: 2020 Stefano Crocco <stefano.crocco@alice.it>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#include "urlloader.h"
#include "konqsettings.h"
#include "konqmainwindow.h"
#include "konqview.h"
#include "konqurl.h"

#include <KIO/OpenUrlJob>
#include <KIO/JobUiDelegate>
#include <KMessageBox>
#include <KParts/ReadOnlyPart>
#include <KParts/BrowserInterface>
#include <KParts/BrowserExtension>
#include <KParts/PartLoader>
#include <KIO/FileCopyJob>
#include <KJobWidgets>
#include <KProtocolManager>
#include <KDesktopFile>
#include <KApplicationTrader>
#include <KParts/PartLoader>

#include <QDebug>
#include <QArgument>
#include <QWebEngineProfile>
#include <QMimeDatabase>
#include <QWebEngineProfile>
#include <QFileDialog>

static KPluginMetaData preferredPart(const QString &mimeType) {
    QVector<KPluginMetaData> plugins = KParts::PartLoader::partsForMimeType(mimeType);
    if (!plugins.isEmpty()) {
        return plugins.first();
    } else {
        return KPluginMetaData();
    }
}

bool UrlLoader::embedWithoutAskingToSave(const QString &mimeType)
{
    static QStringList s_mimeTypes;
    if (s_mimeTypes.isEmpty()) {
        QStringList names{QStringLiteral("kfmclient_html"), QStringLiteral("kfmclient_dir"), QStringLiteral("kfmclient_war")};
        for (const QString &name : names) {
            KPluginMetaData md = findPartById(name);
            s_mimeTypes.append(md.mimeTypes());
        }
        //The user may want to save xml files rather than embedding them
        //TODO: is there a better way to do this?
        s_mimeTypes.removeOne(QStringLiteral("application/xml"));
    }
    return s_mimeTypes.contains(mimeType);
}

bool UrlLoader::isExecutable(const QString& mimeType)
{
    return KParts::BrowserRun::isExecutable(mimeType);
}

UrlLoader::UrlLoader(KonqMainWindow *mainWindow, KonqView *view, const QUrl &url, const QString &mimeType, const KonqOpenURLRequest &req, bool trustedSource, bool dontEmbed):
    QObject(mainWindow), m_mainWindow(mainWindow), m_url(url), m_mimeType(mimeType), m_request(req), m_view(view), m_trustedSource(trustedSource), m_dontEmbed(dontEmbed)
{
    m_dontPassToWebEnginePart = m_request.args.metaData().contains("DontSendToDefaultHTMLPart");
}

UrlLoader::~UrlLoader()
{
}

QString UrlLoader::mimeType() const
{
    return m_mimeType;
}

bool UrlLoader::isMimeTypeKnown(const QString &mimeType)
{
    return !mimeType.isEmpty() && mimeType != QLatin1String("application/octet-stream");
}

void UrlLoader::setView(KonqView* view)
{
    m_view = view;
}

void UrlLoader::setOldLocationBarUrl(const QString& old)
{
    m_oldLocationBarUrl = old;
}

void UrlLoader::setNewTab(bool newTab)
{
    m_request.browserArgs.setNewTab(newTab);
}

void UrlLoader::start()
{
    if (m_url.isLocalFile()) {
        detectSettingsForLocalFiles();
    } else {
        detectSettingsForRemoteFiles();
    }

    if (isMimeTypeKnown(m_mimeType)) {
        KService::Ptr preferredService = KApplicationTrader::preferredService(m_mimeType);
        if (serviceIsKonqueror(preferredService)) {
            m_request.forceAutoEmbed = true;
        }
    }

    if (isMimeTypeKnown(m_mimeType)) {
        decideAction();
    } else {
        m_isAsync = true;
    }
}

bool UrlLoader::isViewLocked() const
{
    return m_view && m_view->isLockedLocation();
}

void UrlLoader::decideAction()
{
    m_action = decideExecute();
    switch (m_action) {
        case OpenUrlAction::Execute:
            m_ready = true;
            break;
        case OpenUrlAction::DoNothing:
            m_ready = true;
            return;
        default:
            if (isViewLocked() || shouldEmbedThis()) {
                bool success = decideEmbedOrSave();
                if (success) {
                    return;
                }
            }
            decideOpenOrSave();
    }
}

void UrlLoader::abort()
{
    if (m_openUrlJob) {
        m_openUrlJob->kill();
    }
    if (m_applicationLauncherJob) {
        m_applicationLauncherJob->kill();
    }
    deleteLater();
}


void UrlLoader::goOn()
{
    if (m_ready) {
        performAction();
    } else {
        launchOpenUrlJob(true);
    }
}

bool UrlLoader::decideEmbedOrSave()
{
    const QLatin1String webEngineName("webenginepart");

    //Use WebEnginePart for konq: URLs even if it's not the default html engine
    if (KonqUrl::hasKonqScheme(m_url)) {
        m_part = findPartById(webEngineName);
    } else {
        //Check whether the view can display the mimetype, but only if the URL hasn't been explicitly
        //typed by the user: in this case, use the preferred service. This is needed to avoid the situation
        //where m_view is a Kate part, the user enters the URL of a web page and the page is opened within
        //the Kate part because it can handle html files.
        if (m_view && m_request.typedUrl.isEmpty() && m_view->supportsMimeType(m_mimeType)) {
            m_part = m_view->service();
        } else {
            if (!m_request.serviceName.isEmpty()) {
                // If the service name has been set by the "--part" command line argument
                // (detected in handleCommandLine() in konqmain.cpp), then use it as is.
                m_part = findPartById(m_request.serviceName);
            } else {
                // Otherwise, use the preferred service for the MIME type.
                m_part = preferredPart(m_mimeType);
            }
        }
    }

    /* Corner case: webenginepart can't determine mimetype (gives application/octet-stream) but
     * OpenUrlJob determines a mimetype supported by WebEnginePart (for example application/xml):
     * if the preferred part is webenginepart, we'd get an endless loop because webenginepart will
     * call again this. To avoid this, if the preferred service is webenginepart and m_dontPassToWebEnginePart
     * is true, use the second preferred service (if any); otherwise return false. This will offer the user
     * the option to open or save, instead.
     */
    if (m_dontPassToWebEnginePart && m_part.pluginId() == webEngineName) {
        QVector<KPluginMetaData> parts = KParts::PartLoader::partsForMimeType(m_mimeType);
        auto findPart = [webEngineName](const KPluginMetaData &md){return md.pluginId() != webEngineName;};
        QVector<KPluginMetaData>::const_iterator partToUse = std::find_if(parts.constBegin(), parts.constEnd(), findPart);
        if (partToUse != parts.constEnd()) {
            m_part = *partToUse;
        } else {
            m_part = KPluginMetaData();
        }
    }

    //If we can't find a service, return false, so that the caller can use decideOpenOrSave to allow the
    //user the possibility of opening the file, since embedding wasn't possibile
    if (!m_part.isValid()) {
        return false;
    }

    //Ask whether to save or embed, except in the following cases:
    //- it's a web page: always embed
    //- it's a local file: always embed
    if (embedWithoutAskingToSave(m_mimeType) || m_url.isLocalFile()) {
        m_action = OpenUrlAction::Embed;
    } else {
        m_action = askSaveOrOpen(OpenEmbedMode::Embed).first;
    }

    if (m_action == OpenUrlAction::Embed) {
        m_request.serviceName = m_part.pluginId();
    }

    m_ready = m_part.isValid() || m_action != OpenUrlAction::Embed;
    return true;
}

void UrlLoader::decideOpenOrSave()
{
    m_ready = true;
    QString protClass = KProtocolInfo::protocolClass(m_url.scheme());
    bool isLocal = m_url.isLocalFile();
    bool alwaysOpen = isLocal || protClass == QLatin1String(":local") || KProtocolInfo::isHelperProtocol(m_url);
    OpenSaveAnswer answerWithService;
    if (!alwaysOpen) {
        answerWithService = askSaveOrOpen(OpenEmbedMode::Open);
    } else {
        answerWithService = qMakePair(OpenUrlAction::Open, nullptr);
    }

    m_action = answerWithService.first;
    m_service = answerWithService.second;
    if (m_action == OpenUrlAction::Open && !m_service) {
        m_service= KApplicationTrader::preferredService(m_mimeType);
    }
}

UrlLoader::OpenUrlAction UrlLoader::decideExecute() const {
    if (!m_url.isLocalFile() || !KRun::isExecutable(m_mimeType)) {
        return OpenUrlAction::UnknwonAction;
    }
    bool canDisplay = !KParts::PartLoader::partsForMimeType(m_mimeType).isEmpty();

    KMessageBox::ButtonCode code;
    KGuiItem executeGuiItem(i18nc("Execute an executable file", "Execute it"));
    QString dontShowAgainId(QLatin1String("AskExecuting")+m_mimeType);

    if (canDisplay) {
        code = KMessageBox::questionYesNoCancel(m_mainWindow, i18nc("The user has to decide whether to execute an executable file or display it",
                                                                                        "<tt>%1</tt> can be executed. Do you want to execute it or to display it?", m_url.path()),
                                                                    QString(), executeGuiItem, KGuiItem(i18nc("Display an executable file", "Display it")),
                                                                    KStandardGuiItem::cancel(), dontShowAgainId, KMessageBox::Dangerous);
    } else {
        code = KMessageBox::questionYesNo(m_mainWindow, i18nc("The user has to decide whether to execute an executable file or not",
                                                                                        "<tt>%1</tt> can be executed. Do you want to execute it?", m_url.path()),
                                                                    QString(), executeGuiItem, KStandardGuiItem::cancel(),
                                                                    dontShowAgainId, KMessageBox::Dangerous);}
    switch (code) {
        case KMessageBox::Yes:
            return OpenUrlAction::Execute;
        case KMessageBox::Cancel:
            return OpenUrlAction::DoNothing;
        case KMessageBox::No:
            //The "No" button actually corresponds to the "Cancel" action if the file can't be displayed
            return canDisplay ? OpenUrlAction::UnknwonAction : OpenUrlAction::DoNothing;
        default: //This is here only to avoid a compiler warning
            return OpenUrlAction::UnknwonAction;
    }
}

void UrlLoader::performAction()
{
    switch (m_action) {
        case OpenUrlAction::Embed:
            embed();
            break;
        case OpenUrlAction::Open:
            open();
            break;
        case OpenUrlAction::Execute:
            execute();
            break;
        case OpenUrlAction::Save:
            save();
            break;
        case OpenUrlAction::DoNothing:
        case OpenUrlAction::UnknwonAction: //This should never happen
            done();
            break;
    }
}

void UrlLoader::done(KJob *job)
{
    //Ensure that m_mimeType and m_request.args.mimeType are equal, since it's not clear what will be used
    m_request.args.setMimeType(m_mimeType);
    if (job) {
        jobFinished(job);
    }
    emit finished(this);
    deleteLater();
}


bool UrlLoader::serviceIsKonqueror(KService::Ptr service)
{
    return service && (service->desktopEntryName() == QLatin1String("konqueror") || service->exec().trimmed().startsWith(QLatin1String("kfmclient")));
}

void UrlLoader::launchOpenUrlJob(bool pauseOnMimeTypeDetermined)
{
    QString mimeType = isMimeTypeKnown(m_mimeType) ? m_mimeType : QString();
    m_openUrlJob = new KIO::OpenUrlJob(m_url, mimeType, this);
    m_openUrlJob->setEnableExternalBrowser(false);
    m_openUrlJob->setRunExecutables(true);
    m_openUrlJob->setUiDelegate(new KIO::JobUiDelegate(KJobUiDelegate::AutoHandlingEnabled, m_mainWindow));
    m_openUrlJob->setSuggestedFileName(m_request.suggestedFileName);
    m_openUrlJob->setDeleteTemporaryFile(m_request.tempFile);
    if (pauseOnMimeTypeDetermined) {
        connect(m_openUrlJob, &KIO::OpenUrlJob::mimeTypeFound, this, &UrlLoader::mimetypeDeterminedByJob);
    }
    connect(m_openUrlJob, &KJob::finished, this, &UrlLoader::jobFinished);
    m_openUrlJob->start();
}

void UrlLoader::mimetypeDeterminedByJob(const QString &mimeType)
{
    m_mimeType=mimeType;
    m_openUrlJob->suspend();
    decideAction();
    if (m_action != OpenUrlAction::Execute) {
        m_openUrlJob->kill();
    }
    performAction();
}

bool UrlLoader::shouldUseDefaultHttpMimeype() const
{
    const QVector<QString> webengineSchemes = {"error", "konq"};
    if (m_dontPassToWebEnginePart || isMimeTypeKnown(m_mimeType)) {
        return false;
    } else if (m_url.scheme().startsWith(QStringLiteral("http")) || webengineSchemes.contains(m_url.scheme())) {
        return true;
    } else {
        return false;
    }
}

void UrlLoader::detectSettingsForRemoteFiles()
{
    if (m_url.isLocalFile()) {
        return;
    }
    if (shouldUseDefaultHttpMimeype()) {
        m_mimeType = QLatin1String("text/html");
        m_request.args.setMimeType(QStringLiteral("text/html"));
    } else if (!m_trustedSource && isTextExecutable(m_mimeType)) {
        m_mimeType = QLatin1String("text/plain");
        m_request.args.setMimeType(QStringLiteral("text/plain"));
    }
}

void UrlLoader::detectSettingsForLocalFiles()
{
    if (!m_url.isLocalFile()) {
        return;
    }

    if (!m_mimeType.isEmpty()) {
        // Generic mechanism for redirecting to tar:/<path>/ when clicking on a tar file,
        // zip:/<path>/ when clicking on a zip file, etc.
        // The .protocol file specifies the mimetype that the kioslave handles.
        // Note that we don't use mimetype inheritance since we don't want to
        // open OpenDocument files as zip folders...
        // Also note that we do this here and not in openView anymore,
        // because in the case of foo.bz2 we don't know the final mimetype, we need a konqrun...
        const QString protocol = KProtocolManager::protocolForArchiveMimetype(m_mimeType);
        if (!protocol.isEmpty() && KonqFMSettings::settings()->shouldEmbed(m_mimeType)) {
            m_url.setScheme(protocol);
            if (m_mimeType == QLatin1String("application/x-webarchive")) {
                m_url.setPath(m_url.path() + QStringLiteral("/index.html"));
                m_mimeType = QStringLiteral("text/html");
            } else {
                if (KProtocolManager::outputType(m_url) == KProtocolInfo::T_FILESYSTEM) {
                    if (!m_url.path().endsWith('/')) {
                        m_url.setPath(m_url.path() + '/');
                    }
                    m_mimeType = QStringLiteral("inode/directory");
                } else {
                    m_mimeType.clear();
                }
            }
        }

        // Redirect to the url in Type=Link desktop files
        if (m_mimeType == QLatin1String("application/x-desktop")) {
            KDesktopFile df(m_url.toLocalFile());
            if (df.hasLinkType()) {
                m_url = QUrl(df.readUrl());
                m_mimeType.clear(); // to be determined again
            }
        }
    } else {
        QMimeDatabase db;
        m_mimeType = db.mimeTypeForFile(m_url.path()).name();
    }
}


bool UrlLoader::shouldEmbedThis() const
{
    return !m_dontEmbed && (m_request.forceAutoEmbed || KonqFMSettings::settings()->shouldEmbed(m_mimeType));
}

void UrlLoader::embed()
{
    bool embedded = m_mainWindow->openView(m_mimeType, m_url, m_view, m_request);
    if (embedded) {
        done();
    } else {
        decideOpenOrSave();
        performAction();
    }
}

void UrlLoader::save()
{
    QFileDialog *dlg = new QFileDialog(m_mainWindow);
    dlg->setAcceptMode(QFileDialog::AcceptSave);
    dlg->setWindowTitle(i18n("Save As"));
    dlg->setOption(QFileDialog::DontConfirmOverwrite, false);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    QString suggestedName = !m_request.suggestedFileName.isEmpty() ? m_request.suggestedFileName : m_url.fileName();
    dlg->selectFile(suggestedName);
    dlg->setDirectory(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
    auto savePrc = [this, dlg](){
        QUrl dest = dlg->selectedUrls().value(0);
        if (dest.isValid()) {
            saveUrlUsingKIO(m_url, dest);
        }
    };
    connect(dlg, &QDialog::accepted, dlg, savePrc);
    dlg->show();
}

void UrlLoader::saveUrlUsingKIO(const QUrl& orig, const QUrl& dest)
{
    KIO::FileCopyJob *job = KIO::file_copy(orig, dest, -1, KIO::Overwrite);
    KJobWidgets::setWindow(job, m_mainWindow);
    job->uiDelegate()->setAutoErrorHandlingEnabled(true);
    connect(job, &KJob::finished, this, [this, job](){done(job);});
    job->start();
}

void UrlLoader::open()
{
    // Prevention against user stupidity : if the associated app for this mimetype
    // is konqueror/kfmclient, then we'll loop forever.
    if (m_service && serviceIsKonqueror(m_service) && m_mainWindow->refuseExecutingKonqueror(m_mimeType)) {
        return;
    }
    KIO::ApplicationLauncherJob *job = new KIO::ApplicationLauncherJob(m_service);
    job->setUrls({m_url});
    job->setUiDelegate(new KIO::JobUiDelegate(KJobUiDelegate::AutoHandlingEnabled, m_mainWindow));
    if (m_request.tempFile) {
        job->setRunFlags(KIO::ApplicationLauncherJob::DeleteTemporaryFiles);
    }
    connect(job, &KJob::finished, this, [this, job](){done(job);});
    job->start();
}

void UrlLoader::execute()
{
    //Since only local files can be executed, m_openUrlJob should always be nullptr. However, keep this check, just in case
    if (!m_openUrlJob) {
        launchOpenUrlJob(false);
        connect(m_openUrlJob, &KJob::finished, this, [this](){done(m_openUrlJob);});
    } else {
        disconnect(m_openUrlJob, &KJob::finished, this, nullptr); //Otherwise, jobFinished will be called twice
        connect(m_openUrlJob, &KJob::finished, this, [this](){done(m_openUrlJob);});
        m_openUrlJob->resume();
    }
}

//Copied from KParts::BrowserRun::isTextExecutable
bool UrlLoader::isTextExecutable(const QString &mimeType)
{
    return ( mimeType == QLatin1String("application/x-desktop") || mimeType == QLatin1String("application/x-shellscript"));
}

UrlLoader::OpenSaveAnswer UrlLoader::askSaveOrOpen(OpenEmbedMode mode) const
{
    KParts::BrowserOpenOrSaveQuestion dlg(m_mainWindow, m_url, m_mimeType);
    dlg.setSuggestedFileName(m_request.suggestedFileName);
    dlg.setFeatures(KParts::BrowserOpenOrSaveQuestion::ServiceSelection);
    KParts::BrowserOpenOrSaveQuestion::Result ans = mode == OpenEmbedMode::Open ? dlg.askOpenOrSave() : dlg.askEmbedOrSave();
    OpenUrlAction action;
    switch (ans) {
        case KParts::BrowserOpenOrSaveQuestion::Save:
            action = OpenUrlAction::Save;
            break;
        case KParts::BrowserOpenOrSaveQuestion::Open:
            action = OpenUrlAction::Open;
            break;
        case KParts::BrowserOpenOrSaveQuestion::Embed:
            action = OpenUrlAction::Embed;
            break;
        default:
            action = OpenUrlAction::DoNothing;
    }
    return qMakePair(action, dlg.selectedService());
}

QString UrlLoader::partForLocalFile(const QString& path)
{
    QMimeDatabase db;
    QString mimetype = db.mimeTypeForFile(path).name();

    KPluginMetaData plugin = preferredPart(mimetype);
    return plugin.pluginId();
}

UrlLoader::ViewToUse UrlLoader::viewToUse() const
{
    if (m_view && m_view->isFollowActive()) {
        return ViewToUse::CurrentView;
    }

    if (!m_view && !m_request.browserArgs.newTab()) {
        return ViewToUse::CurrentView;
    } else if (!m_view && m_request.browserArgs.newTab()) {
        return ViewToUse::NewTab;
    }
    return ViewToUse::View;
}

void UrlLoader::jobFinished(KJob* job)
{
    m_jobHadError = job->error();
}

QDebug operator<<(QDebug dbg, UrlLoader::OpenUrlAction action)
{
    QDebugStateSaver saver(dbg);
    dbg.resetFormat();
    switch (action) {
        case UrlLoader::OpenUrlAction::UnknwonAction:
            dbg << "UnknownAction";
            break;
        case UrlLoader::OpenUrlAction::DoNothing:
            dbg << "DoNothing";
            break;
        case UrlLoader::OpenUrlAction::Save:
            dbg << "Save";
            break;
        case UrlLoader::OpenUrlAction::Embed:
            dbg << "Embed";
            break;
        case UrlLoader::OpenUrlAction::Open:
            dbg << "Open";
            break;
        case UrlLoader::OpenUrlAction::Execute:
            dbg << "Execute";
            break;
    }
    return dbg;
}
