/**
 * telepathy-hanging
 *
 * Copyright (C) 2015 Tiago Salem Herrmann, All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License version 3, as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranties of MERCHANTABILITY,
 * SATISFACTORY QUALITY, or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "hangingtextchannel.h"

HangingTextChannel::HangingTextChannel(HangingConnection *conn, const QString &conversationId, const QString chatId, QStringList participants, QObject *parent):
    QObject(parent),
    mConnection(conn),
    mMessageCounter(1),
    mConversationId(conversationId),
    mUserChatId(chatId)
{
    Tp::BaseChannelPtr baseChannel;

    if (!chatId.isEmpty()) {
        baseChannel = Tp::BaseChannel::create(mConnection,
                                              TP_QT_IFACE_CHANNEL_TYPE_TEXT,
                                              Tp::HandleTypeContact,
                                              mConnection->ensureContactHandle(chatId));
        baseChannel->setTargetID(chatId);
    } else {
        baseChannel = Tp::BaseChannel::create(mConnection,
                                              TP_QT_IFACE_CHANNEL_TYPE_TEXT,
                                              Tp::HandleTypeRoom,
                                              mConnection->ensureRoomHandle(conversationId));
        baseChannel->setTargetID(conversationId);
    }
    Tp::BaseChannelTextTypePtr textType = Tp::BaseChannelTextType::create(baseChannel.data());
    baseChannel->plugInterface(Tp::AbstractChannelInterfacePtr::dynamicCast(textType));

    QStringList supportedContentTypes = QStringList() << "text/plain";

    Tp::UIntList messageTypes = Tp::UIntList() <<
                                Tp::ChannelTextMessageTypeNormal <<
                                Tp::ChannelTextMessageTypeDeliveryReport;
    uint messagePartSupportFlags = 0;
    uint deliveryReportingSupport = Tp::DeliveryReportingSupportFlagReceiveSuccesses;
    mMessagesIface = Tp::BaseChannelMessagesInterface::create(textType.data(),
                     supportedContentTypes,
                     messageTypes,
                     messagePartSupportFlags,
                     deliveryReportingSupport);

    mMessagesIface->setSendMessageCallback(Tp::memFun(this,&HangingTextChannel::sendMessage));

    baseChannel->plugInterface(Tp::AbstractChannelInterfacePtr::dynamicCast(mMessagesIface));

    if (chatId.isEmpty()) {
        Tp::ChannelGroupFlags groupFlags = Tp::ChannelGroupFlagHandleOwnersNotAvailable |
                Tp::ChannelGroupFlagMembersChangedDetailed |
                Tp::ChannelGroupFlagProperties;
        mGroupIface = Tp::BaseChannelGroupInterface::create();
        mGroupIface->setAddMembersCallback(Tp::memFun(this,&HangingTextChannel::onAddMembers));
        mGroupIface->setRemoveMembersCallback(Tp::memFun(this,&HangingTextChannel::onRemoveMembers));

        baseChannel->plugInterface(Tp::AbstractChannelInterfacePtr::dynamicCast(mGroupIface));

        mGroupIface->setGroupFlags(groupFlags);
        mGroupIface->setSelfHandle(conn->selfHandle());
        Tp::UIntList members;
        Q_FOREACH(const QString &participant, participants) {
            members << mConnection->ensureContactHandle(participant);
        }
        mGroupIface->setMembers(members, QVariantMap());


        ClientConversationState c = mConnection->getConversations()[conversationId];
        QString creatorId = c.conversation().selfconversationstate().inviterid().chatid().c_str();
        mRoomIface = Tp::BaseChannelRoomInterface::create(c.conversation().name().c_str(), QString(), creatorId, mConnection->ensureContactHandle(creatorId), QDateTime()/*QDateTime::fromMSecsSinceEpoch(c.conversation().selfconversationstate().invitetimestamp())*/);
        baseChannel->plugInterface(Tp::AbstractChannelInterfacePtr::dynamicCast(mRoomIface));

        mRoomConfigIface = Tp::BaseChannelRoomConfigInterface::create();
        mRoomConfigIface->setTitle(c.conversation().name().c_str());
        mRoomConfigIface->setConfigurationRetrieved(true);
        baseChannel->plugInterface(Tp::AbstractChannelInterfacePtr::dynamicCast(mRoomConfigIface));
    }

    mChatStateIface = Tp::BaseChannelChatStateInterface::create();
    mChatStateIface->setSetChatStateCallback(Tp::memFun(this, &HangingTextChannel::setChatState));
    baseChannel->plugInterface(Tp::AbstractChannelInterfacePtr::dynamicCast(mChatStateIface));

    mBaseChannel = baseChannel;
    mTextChannel = Tp::BaseChannelTextTypePtr::dynamicCast(mBaseChannel->interface(TP_QT_IFACE_CHANNEL_TYPE_TEXT));
    QObject::connect(mBaseChannel.data(), SIGNAL(closed()), this, SLOT(deleteLater()));
    QObject::connect(&mTypingTimer, SIGNAL(timeout()), SLOT(onTypingTimeout()));
}

void HangingTextChannel::onAddMembers(const Tp::UIntList& handles, const QString& message, Tp::DBusError* error)
{
    Q_UNUSED(handles)
    Q_UNUSED(message)
    Q_UNUSED(error)
}

void HangingTextChannel::onRemoveMembers(const Tp::UIntList& handles, const QString& message, uint reason, Tp::DBusError* error)
{
    Q_UNUSED(handles)
    Q_UNUSED(message)
    Q_UNUSED(error)
    Q_UNUSED(reason)
}

HangingTextChannel::~HangingTextChannel()
{
}

Tp::BaseChannelPtr HangingTextChannel::baseChannel()
{
    return mBaseChannel;
}

void HangingTextChannel::sendDeliveryReport(const QString &messageId, QString identifier, Tp::DeliveryStatus status)
{
    Tp::MessagePartList partList;
    Tp::MessagePart header;

    header["message-sender"] = QDBusVariant(mConnection->ensureContactHandle(identifier));
    header["message-sender-id"] = QDBusVariant(identifier);
    header["message-type"] = QDBusVariant(Tp::ChannelTextMessageTypeDeliveryReport);
    header["delivery-status"] = QDBusVariant(status);
    header["delivery-token"] = QDBusVariant(messageId);

    partList << header;

    mTextChannel->addReceivedMessage(partList);
}

QString HangingTextChannel::sendMessage(Tp::MessagePartList message, uint flags, Tp::DBusError* error)
{
    Q_UNUSED(flags)
    Q_UNUSED(error)
    Tp::MessagePart body = message.at(1);
    QString id;

    ClientSendChatMessageRequest clientSendChatMessageRequest;
    ClientEventRequestHeader *clientEventRequestHeader = new ClientEventRequestHeader();
    ClientMessageContentList *clientMessageContentList = new ClientMessageContentList();
    ClientMessageContent *clientMessageContent = clientMessageContentList->add_messagecontent();
    ClientConversationId *clientConversationId = new ClientConversationId();

    clientConversationId->set_id(mConversationId.toLatin1().data());
    clientEventRequestHeader->set_allocated_conversationid(clientConversationId);
    uint generatedId = (uint)(QDateTime::currentMSecsSinceEpoch() % qint64(4294967295u));
    clientEventRequestHeader->set_clientgeneratedid(generatedId);
    clientEventRequestHeader->set_expectedotr(ON_THE_RECORD);
    QStringList segments = body["content"].variant().toString().split("\n");
    QStringListIterator it(segments);
    while (it.hasNext()) {
        QString textSegment = it.next();
        Segment *segment = clientMessageContent->add_segment();
        segment->set_type(Segment_SegmentType_TEXT);
        segment->set_text(textSegment.toStdString().c_str());
        if (it.hasNext()) {
            Segment *breakSegment = clientMessageContent->add_segment();
            breakSegment->set_type(Segment_SegmentType_LINE_BREAK);
        }
    }

    clientSendChatMessageRequest.set_allocated_eventrequestheader(clientEventRequestHeader);
    clientSendChatMessageRequest.set_allocated_messagecontentlist(clientMessageContentList);
    mConnection->hangishClient()->sendChatMessage(clientSendChatMessageRequest);
    QString messageId = QDateTime::currentDateTimeUtc().toString(Qt::ISODate) + "-" + QString::number(mMessageCounter++);
    mPendingMessages[generatedId] = messageId;
    return messageId;
}

QString HangingTextChannel::conversationId()
{
    return mConversationId;
}

void HangingTextChannel::eventReceived(ClientEvent &event, bool scrollback)
{
    Tp::MessagePartList partList;

    if (event.has_membershipchange()) {
        uint actor = mConnection->ensureContactHandle(event.senderid().chatid().c_str());
        Tp::UIntList members = mGroupIface->members();
        if (event.membershipchange().type() == LEAVE) {
            for (int i = 0; i < event.membershipchange().participantid_size(); i++) {
                QString participant = event.membershipchange().participantid(i).chatid().c_str();
                uint handle = mConnection->ensureContactHandle(participant);
                members.removeAll(handle);
            }
        } else if (event.membershipchange().type() == JOIN) {
            for (int i = 0; i < event.membershipchange().participantid_size(); i++) {
                QString participant = event.membershipchange().participantid(i).chatid().c_str();
                uint handle = mConnection->ensureContactHandle(participant);
                members << handle;
            }
        }
        QVariantMap details;
        details["actor"] = QVariant::fromValue(actor);
        mGroupIface->setMembers(members, details);
    }

    if (event.has_conversationrename()) {
        mRoomConfigIface->setTitle(event.conversationrename().newname().c_str());
    }

    if (!event.chatmessage().has_messagecontent() || event.chatmessage().messagecontent().segment_size() == 0) {
        return;
    }

    const QString fromId = event.senderid().chatid().c_str();
    if (event.has_selfeventstate() && event.selfeventstate().has_clientgeneratedid()) {
        if (mPendingMessages.contains(event.selfeventstate().clientgeneratedid())) {
            QString messageId = mPendingMessages.take(event.selfeventstate().clientgeneratedid());
            sendDeliveryReport(messageId, fromId, Tp::DeliveryStatusAccepted);
            return;
        }
    }

    Tp::MessagePart header;
    header["message-token"] = QDBusVariant(event.eventid().c_str());
    header["message-received"] = QDBusVariant(QDateTime::fromMSecsSinceEpoch(event.timestamp()/1000).toTime_t());
    header["message-sender"] = QDBusVariant(mConnection->ensureContactHandle(fromId));
    header["message-sender-id"] = QDBusVariant(fromId);
    header["message-type"] = QDBusVariant(Tp::ChannelTextMessageTypeNormal);

    if (scrollback) {
        header["scrollback"] = QDBusVariant(scrollback);
    }

    partList << header;

    for (int i = 0; i < event.chatmessage().messagecontent().segment_size(); i++) {
        Segment segment = event.chatmessage().messagecontent().segment(i);
        Tp::MessagePart body;
        body["content-type"] = QDBusVariant("text/plain");
        const QString message = segment.text().c_str();
        switch (segment.type()) {
        case Segment_SegmentType_TEXT:
        case Segment_SegmentType_LINK:
            body["content"] = QDBusVariant(message);
            break;
        default:
            qWarning() << "Segment type not supported";
        }
        partList << body;
    }

    mTextChannel->addReceivedMessage(partList);
}

void HangingTextChannel::onTypingTimeout()
{
    mTypingTimer.stop();
    mConnection->hangishClient()->setTyping(conversationId(), 3);
}

void HangingTextChannel::setChatState(uint state, Tp::DBusError *error)
{
    Q_UNUSED(error)
    if (state == Tp::ChannelChatStateComposing) {
        mTypingTimer.start(5000);
        mConnection->hangishClient()->setTyping(conversationId(), 1);
    } else {
        mTypingTimer.stop();
        mConnection->hangishClient()->setTyping(conversationId(), 3);
    }
}

void HangingTextChannel::updateTypingState(const ClientSetTypingNotification &notification)
{
    uint handle =  mConnection->ensureContactHandle(notification.senderid().chatid().c_str());
    Tp::ChannelChatState state;

    switch (notification.type()) {
    case START:
        state = Tp::ChannelChatStateComposing;
        break;
    case PAUSE:
        state = Tp::ChannelChatStatePaused;
        break;
    default:
        state = Tp::ChannelChatStateActive;
    }

    mChatStateIface->chatStateChanged(handle, state);
}
