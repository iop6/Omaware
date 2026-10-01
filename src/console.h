// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QImage>
#include <QQuickPaintedItem>
#include <QThread>
#include <QSocketNotifier>
#include <QSet>
#include <QHash>
#include <QMutex>
#include "backend.h"
#include <rfb/rfbclient.h>

struct FrameMailbox {
    QMutex mutex;
    QImage latest;
    bool pending = false;
};
using FrameHandle = std::shared_ptr<FrameMailbox>;
Q_DECLARE_METATYPE(FrameHandle)

class VncWorker : public QObject {
    Q_OBJECT
public slots:
    void start(GraphicsHandle socket, quint64 generation);
    void stop();
    void key(quint32 symbol, bool down);
    void pointer(int x, int y, int buttons);
    void clipboard(QString text);
    void resizeGuest(int width, int height);
signals:
    void frame(FrameHandle mailbox, quint64 generation);
    void status(QString text, bool connected, quint64 generation);
    void clipboardReceived(QString text, quint64 generation);
    void notice(QString text, quint64 generation);
private:
    static rfbBool allocate(rfbClient *client);
    static void updated(rfbClient *client);
    static void cutText(rfbClient *client, const char *text, int length);
    static void cutTextUtf8(rfbClient *client, const char *text, int length);
    void read();
    rfbClient *client_ = nullptr;
    QSocketNotifier *notifier_ = nullptr;
    QImage buffer_;
    FrameHandle mailbox_;
    quint64 generation_ = 0;
};

class Console : public QQuickPaintedItem {
    Q_OBJECT
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(bool captured READ captured NOTIFY statusChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY statusChanged)
    Q_PROPERTY(bool hasFrame READ hasFrame NOTIFY frameChanged)
    Q_PROPERTY(QString clipboardMode READ clipboardMode WRITE setClipboardMode NOTIFY clipboardModeChanged)
public:
    explicit Console(QQuickItem *parent = nullptr);
    ~Console() override;
    void paint(QPainter *painter) override;
    QString status() const { return status_; }
    bool captured() const { return captured_; }
    bool connected() const { return connected_; }
    bool hasFrame() const { return !image_.isNull(); }
    QString clipboardMode() const { return clipboardMode_; }
    void setClipboardMode(QString mode);
    void attach(GraphicsHandle socket);
    Q_INVOKABLE void attachForVm(GraphicsHandle socket, QString uuid);
    Q_INVOKABLE void disconnectConsole();
    Q_INVOKABLE void releaseInput();
    Q_INVOKABLE void sendSpecial(QString key);
    Q_INVOKABLE void pasteClipboard();
    Q_INVOKABLE void resizeGuest(int width, int height);
    Q_INVOKABLE QString checkpointPreview() const;
    QImage frame() const { return image_; }
signals:
    void statusChanged();
    void frameReceived();
    void frameChanged();
    void clipboardModeChanged();
protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void hoverMoveEvent(QHoverEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
private:
    void sendCtrlAltDelete();
    void sendPointer(QMouseEvent *event);
    void pointerAt(QPointF position, int buttons);
    void sendKey(QKeyEvent *event, bool down);
    QRectF destination() const;
    QThread thread_;
    VncWorker *worker_;
    QImage image_;
    QHash<quint32, quint32> pressed_;
    QString status_ = "Open a running VM’s console";
    bool captured_ = false;
    bool connected_ = false;
    quint64 generation_ = 0;
    QString clipboardMode_ = "off";
    QString clipboardVm_;   // the VM clipboardMode_ was chosen for
    bool receivingClipboard_ = false;
};
