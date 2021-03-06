/*
 * Copyright (C) by Klaas Freitag <freitag@owncloud.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include "servernotificationhandler.h"
#include "accountstate.h"
#include "capabilities.h"
#include "networkjobs.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace OCC {

Q_LOGGING_CATEGORY(lcServerNotification, "nextcloud.gui.servernotification", QtInfoMsg)

const QString notificationsPath = QLatin1String("ocs/v2.php/apps/notifications/api/v2/notifications");
const char propertyAccountStateC[] = "oc_account_state";
const int successStatusCode = 200;
const int notModifiedStatusCode = 304;

ServerNotificationHandler::ServerNotificationHandler(QObject *parent)
    : QObject(parent)
{
}

void ServerNotificationHandler::slotFetchNotifications(AccountState *ptr)
{
    // check connectivity and credentials
    if (!(ptr && ptr->isConnected() && ptr->account() && ptr->account()->credentials() && ptr->account()->credentials()->ready())) {
        deleteLater();
        return;
    }
    // check if the account has notifications enabled. If the capabilities are
    // not yet valid, its assumed that notifications are available.
    if (ptr->account()->capabilities().isValid()) {
        if (!ptr->account()->capabilities().notificationsAvailable()) {
            qCInfo(lcServerNotification) << "Account" << ptr->account()->displayName() << "does not have notifications enabled.";
            deleteLater();
            return;
        }
    }

    // if the previous notification job has finished, start next.
    _notificationJob = new JsonApiJob(ptr->account(), notificationsPath, this);
    QObject::connect(_notificationJob.data(), &JsonApiJob::jsonReceived,
        this, &ServerNotificationHandler::slotNotificationsReceived);
    QObject::connect(_notificationJob.data(), &JsonApiJob::etagResponseHeaderReceived,
        this, &ServerNotificationHandler::slotEtagResponseHeaderReceived);
    _notificationJob->setProperty(propertyAccountStateC, QVariant::fromValue<AccountState *>(ptr));
    _notificationJob->addRawHeader("If-None-Match", ptr->notificationsEtagResponseHeader());
    _notificationJob->start();
}

void ServerNotificationHandler::slotEtagResponseHeaderReceived(const QByteArray &value, int statusCode){
    if(statusCode == successStatusCode){
        qCWarning(lcServerNotification) << "New Notification ETag Response Header received " << value;
        AccountState *account = qvariant_cast<AccountState *>(sender()->property(propertyAccountStateC));
        account->setNotificationsEtagResponseHeader(value);
    }
}

void ServerNotificationHandler::slotNotificationsReceived(const QJsonDocument &json, int statusCode)
{
    if (statusCode != successStatusCode && statusCode != notModifiedStatusCode) {
        qCWarning(lcServerNotification) << "Notifications failed with status code " << statusCode;
        deleteLater();
        return;
    }

    if (statusCode == notModifiedStatusCode) {
        qCWarning(lcServerNotification) << "Status code " << statusCode << " Not Modified - No new notifications.";
        deleteLater();
        return;
    }

    auto notifies = json.object().value("ocs").toObject().value("data").toArray();

    AccountState *ai = qvariant_cast<AccountState *>(sender()->property(propertyAccountStateC));

    ActivityList list;

    foreach (auto element, notifies) {
        Activity a;
        auto json = element.toObject();
        a._type = Activity::NotificationType;
        a._accName = ai->account()->displayName();
        a._id = json.value("notification_id").toInt();
        a._subject = json.value("subject").toString();
        a._message = json.value("message").toString();

        QString s = json.value("link").toString();
        if (!s.isEmpty()) {
            QUrl link(s);
            if(link.host().isEmpty()){
                link.setScheme(ai->account()->url().scheme());
                link.setHost(ai->account()->url().host());
            }
            a._link = link;
        }
        a._dateTime = QDateTime::fromString(json.value("datetime").toString(), Qt::ISODate);

        auto actions = json.value("actions").toArray();
        foreach (auto action, actions) {
            auto actionJson = action.toObject();
            ActivityLink al;
            al._label = QUrl::fromPercentEncoding(actionJson.value("label").toString().toUtf8());
            al._link = actionJson.value("link").toString();
            al._verb = actionJson.value("type").toString().toUtf8();
            al._isPrimary = actionJson.value("primary").toBool();

            a._links.append(al);
        }

        // Add another action to dismiss notification on server
        // https://github.com/owncloud/notifications/blob/master/docs/ocs-endpoint-v1.md#deleting-a-notification-for-a-user
        ActivityLink al;
        al._label = tr("Dismiss");
        al._link  = Utility::concatUrlPath(ai->account()->url(), notificationsPath + "/" + QString::number(a._id)).toString();
        al._verb  = "DELETE";
        al._isPrimary = false;
        a._links.append(al);

        list.append(a);
    }
    emit newNotificationList(list);

    deleteLater();
}
}
