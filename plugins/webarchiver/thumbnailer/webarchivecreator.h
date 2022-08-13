/*
    SPDX-FileCopyrightText: 2001 Malte Starostik <malte@kde.org>
    SPDX-FileCopyrightText: 2020 Jonathan Marten <jjm@keelhaul.me.uk>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef WEBARCHIVECREATOR_H
#define WEBARCHIVECREATOR_H

#include <qobject.h>
#ifdef THUMBNAIL_USE_WEBKIT
#include <qnetworkcookiejar.h>
#endif // THUMBNAIL_USE_WEBKIT

#include <kio/thumbcreator.h>


class QTemporaryDir;


class WebArchiveCreator : public QObject, public ThumbCreator
{
    Q_OBJECT

public:
    WebArchiveCreator();
    ~WebArchiveCreator() override;

    bool create(const QString &path, int width, int height, QImage &img) override;

private slots:
    void slotLoadFinished(bool ok);

    void slotProcessingTimeout();
    void slotRenderTimer();

private:
    QTemporaryDir *m_tempDir;

    bool m_rendered;
    bool m_error;
};


#ifdef THUMBNAIL_USE_WEBKIT

class WebArchiveCreatorCookieJar : public QNetworkCookieJar
{
    Q_OBJECT

public:
    WebArchiveCreatorCookieJar(QObject *parent = nullptr);
    ~WebArchiveCreatorCookieJar() override = default;

    QList<QNetworkCookie> cookiesForUrl(const QUrl &url) const override;
    bool insertCookie(const QNetworkCookie & cookie) override;
    bool setCookiesFromUrl(const QList<QNetworkCookie> &cookieList, const QUrl &url) override;
};

#endif // THUMBNAIL_USE_WEBKIT

#endif // WEBARCHIVECREATOR_H
