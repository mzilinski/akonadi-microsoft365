/*
    SPDX-FileCopyrightText: 2026 Malte Zilinski <malte@zilinski.eu>
    SPDX-License-Identifier: LGPL-2.0-or-later

    A local HTTP server that answers each request with the next scripted reply and
    records what it was asked, so request and job tests run against something that
    looks like Graph without talking to Microsoft.
*/

#pragma once

#include <QByteArray>
#include <QList>
#include <QTcpServer>
#include <QTcpSocket>

class ScriptedGraphServer : public QTcpServer
{
public:
    struct Reply {
        int status = 200;
        QByteArray body;
        QList<QPair<QByteArray, QByteArray>> headers = {};
    };
    struct Request {
        QByteArray method;
        QByteArray target; ///< path and query as sent
        QByteArray head; ///< request line and headers
        QByteArray body;
    };

    explicit ScriptedGraphServer(QList<Reply> replies, QObject *parent = nullptr)
        : QTcpServer(parent)
        , mReplies(std::move(replies))
    {
        listen(QHostAddress::LocalHost);
    }

    [[nodiscard]] QString baseUrl() const
    {
        return QStringLiteral("http://127.0.0.1:%1").arg(serverPort());
    }

    [[nodiscard]] const QList<Request> &requests() const
    {
        return mRequests;
    }

protected:
    void incomingConnection(qintptr handle) override
    {
        auto socket = new QTcpSocket(this);
        socket->setSocketDescriptor(handle);
        auto buffer = std::make_shared<QByteArray>();
        connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer] {
            *buffer += socket->readAll();
            const int headerEnd = buffer->indexOf("\r\n\r\n");
            if (headerEnd < 0) {
                return;
            }
            const QByteArray head = buffer->left(headerEnd);
            const int lengthPos = head.toLower().indexOf("content-length:");
            const int length = lengthPos < 0 ? 0 : head.mid(lengthPos + 15, head.indexOf("\r\n", lengthPos) - lengthPos - 15).trimmed().toInt();
            if (buffer->size() < headerEnd + 4 + length) {
                return;
            }
            const QByteArray requestLine = head.left(head.indexOf("\r\n"));
            const QList<QByteArray> parts = requestLine.split(' ');
            mRequests.append({parts.value(0), parts.value(1), head, buffer->mid(headerEnd + 4, length)});
            buffer->clear();

            const Reply reply = mReplies.isEmpty() ? Reply{500, QByteArrayLiteral("{\"error\":{\"code\":\"NoScript\"}}")} : mReplies.takeFirst();
            // "{BASE}" in a scripted body stands for this server, for links to follow.
            const QByteArray body = QByteArray(reply.body).replace("{BASE}", baseUrl().toUtf8());
            QByteArray out = "HTTP/1.1 " + QByteArray::number(reply.status) + " Scripted\r\nConnection: close\r\n";
            bool hasType = false;
            for (const auto &[name, value] : reply.headers) {
                hasType = hasType || name.toLower() == "content-type";
                out += name + ": " + value + "\r\n";
            }
            if (!hasType && !body.isEmpty()) {
                out += "Content-Type: application/json\r\n";
            }
            out += "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body;
            socket->write(out);
            socket->disconnectFromHost();
        });
    }

private:
    QList<Reply> mReplies;
    QList<Request> mRequests;
};
