// SPDX-License-Identifier: GPL-3.0-or-later
#include "console.h"
#include <QBuffer>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QHoverEvent>
#include <QMutexLocker>
#include <QClipboard>
#include <QGuiApplication>
#include <rfb/keysym.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

rfbBool VncWorker::allocate(rfbClient *client) {
    auto self = static_cast<VncWorker *>(rfbClientGetClientData(client, nullptr));
    if (client->width <= 0 || client->height <= 0 || client->width > 8192 || client->height > 8192) return FALSE;
    self->buffer_ = QImage(client->width, client->height, QImage::Format_RGB32);
    if (self->buffer_.isNull()) return FALSE;
    self->buffer_.fill(Qt::black);
    client->frameBuffer = self->buffer_.bits();
    return TRUE;
}
void VncWorker::updated(rfbClient *client) {
    auto self = static_cast<VncWorker *>(rfbClientGetClientData(client, nullptr));
    // One pending delivery and one latest frame, even when the UI is busy.
    QMutexLocker lock(&self->mailbox_->mutex);
    self->mailbox_->latest = self->buffer_.copy();
    // RFB's unused fourth byte is not Qt's required opaque RGB32 alpha byte.
    for (int y = 0; y < self->mailbox_->latest.height(); ++y) {
        auto pixels = reinterpret_cast<QRgb *>(self->mailbox_->latest.scanLine(y));
        for (int x = 0; x < self->mailbox_->latest.width(); ++x) pixels[x] |= 0xff000000;
    }
    if (!self->mailbox_->pending) {
        self->mailbox_->pending = true;
        emit self->frame(self->mailbox_, self->generation_);
    }
}
void VncWorker::start(GraphicsHandle socket, quint64 generation) {
    stop();
    generation_ = generation;
    // LibVNCClient writes can race a guest shutdown. Keep SIGPIPE blocked on
    // this dedicated worker so writes return EPIPE and use our disconnect path.
    // Do not change the signal disposition for the UI or other libraries.
    sigset_t pipeMask;
    sigemptyset(&pipeMask);
    sigaddset(&pipeMask, SIGPIPE);
    if (pthread_sigmask(SIG_BLOCK, &pipeMask, nullptr) != 0) {
        emit status("Could not initialize console signal handling", false, generation_);
        return;
    }
    mailbox_ = std::make_shared<FrameMailbox>();
    int fd = std::exchange(socket->fd, -1);
    if (fd < 0) return;
    client_ = rfbGetClient(8, 3, 4);
    if (!client_) { ::close(fd); emit status("Could not allocate VNC client", false, generation_); return; }
    client_->sock = fd;
    client_->listenSpecified = TRUE; // already-connected, local libvirt graphics FD
    client_->format.redShift = 16;
    client_->format.greenShift = 8;
    client_->format.blueShift = 0;
    client_->format.redMax = client_->format.greenMax = client_->format.blueMax = 255;
    client_->format.depth = 24;
    client_->MallocFrameBuffer = allocate;
    client_->FinishedFrameBufferUpdate = updated;
    client_->GotXCutText = cutText;
    client_->GotXCutTextUTF8 = cutTextUtf8;
    client_->canHandleNewFBSize = TRUE;
    client_->appData.useRemoteCursor = FALSE;
    client_->readTimeout = 3;
    rfbClientSetClientData(client_, nullptr, this);
    if (!rfbInitClient(client_, nullptr, nullptr)) {
        // rfbInitClient frees its client on failure; buffer_ is owned by us.
        client_ = nullptr;
        buffer_ = {};
        emit status("Console handshake failed; reopen the console to retry", false, generation_);
        return;
    }
    notifier_ = new QSocketNotifier(client_->sock, QSocketNotifier::Read, this);
    connect(notifier_, &QSocketNotifier::activated, this, [this] { read(); });
    emit status("Connected · click display for input · Ctrl+Alt releases", true, generation_);
    // rfbInitClient may have already buffered server bytes.
    read();
}
void VncWorker::read() {
    if (!client_) return;
    int count = 0;
    // Bound each dispatch so input and disconnect requests also get serviced.
    while (client_ && count++ < 16) {
        int ready = WaitForMessage(client_, 0);
        if (ready < 0 || (ready > 0 && !HandleRFBServerMessage(client_))) {
            stop(); emit status("Console disconnected · reopen after starting the VM", false, generation_); return;
        }
        if (!ready) return;
    }
    if (client_ && client_->buffered > 0) QMetaObject::invokeMethod(this, [this] { read(); }, Qt::QueuedConnection);
}
void VncWorker::stop() {
    delete notifier_; notifier_ = nullptr;
    if (client_) { rfbClientCleanup(client_); client_ = nullptr; }
    buffer_ = {};
}
void VncWorker::key(quint32 symbol, bool down) {
    if (client_ && !SendKeyEvent(client_, symbol, down)) { stop(); emit status("Console input disconnected", false, generation_); }
}
void VncWorker::pointer(int x, int y, int buttons) {
    if (client_ && !SendPointerEvent(client_, x, y, buttons)) { stop(); emit status("Console input disconnected", false, generation_); }
}
void VncWorker::cutText(rfbClient *client, const char *text, int length) {
    auto self = static_cast<VncWorker *>(rfbClientGetClientData(client, nullptr));
    if (length >= 0 && length <= 1048576) emit self->clipboardReceived(QString::fromLatin1(text, length), self->generation_);
}
void VncWorker::cutTextUtf8(rfbClient *client, const char *text, int length) {
    auto self = static_cast<VncWorker *>(rfbClientGetClientData(client, nullptr));
    if (length >= 0 && length <= 1048576) emit self->clipboardReceived(QString::fromUtf8(text, length), self->generation_);
}
void VncWorker::clipboard(QString text) {
    if (!client_ || text.toUtf8().size() > 1048576) return;
    auto utf8 = text.toUtf8();
    if (SendClientCutTextUTF8(client_, utf8.data(), utf8.size())) return;
    auto legacy = text.toLatin1();
    if (QString::fromLatin1(legacy) != text) { emit notice("This console did not negotiate Unicode clipboard support.", generation_); return; }
    if (!SendClientCutText(client_, legacy.data(), legacy.size())) { stop(); emit status("Clipboard connection closed", false, generation_); }
}
void VncWorker::resizeGuest(int width, int height) {
    if (!client_ || width < 640 || height < 480 || width > 3840 || height > 2160) return;
    if (!SupportsClient2Server(client_, rfbSetDesktopSize)) { emit notice("This guest display does not support VNC resize requests. Change its resolution inside the guest.", generation_); return; }
    const bool sent = SendExtDesktopSize(client_, width, height);
    emit notice(sent ? "Display resize requested. The guest display driver must support the requested mode." : "The display resize request was not accepted.", generation_);
}
Console::Console(QQuickItem *parent) : QQuickPaintedItem(parent), worker_(new VncWorker) {
    qRegisterMetaType<FrameHandle>();
    setAcceptedMouseButtons(Qt::LeftButton | Qt::MiddleButton | Qt::RightButton);
    setAcceptHoverEvents(true);
    setFlag(ItemIsFocusScope, true);
    worker_->moveToThread(&thread_);
    connect(&thread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(worker_, &VncWorker::frame, this, [this](FrameHandle mailbox, quint64 generation) {
        QMutexLocker lock(&mailbox->mutex);
        mailbox->pending = false;
        if (generation != generation_) return;
        const bool firstFrame = image_.isNull();
        image_ = mailbox->latest; update(); emit frameReceived();
        if (firstFrame) emit frameChanged();
    });
    connect(worker_, &VncWorker::status, this, [this](QString text, bool connected, quint64 generation) {
        if (generation != generation_) return;
        connected_ = connected;
        if (!connected) { releaseInput(); image_ = {}; update(); emit frameChanged(); }
        status_ = text; emit statusChanged();
    });
    connect(worker_, &VncWorker::notice, this, [this](QString text, quint64 generation) { if (generation == generation_) { status_ = text; emit statusChanged(); } });
    connect(worker_, &VncWorker::clipboardReceived, this, [this](QString text, quint64 generation) {
        if (generation != generation_ || clipboardMode_ != "both") return;
        receivingClipboard_ = true; QGuiApplication::clipboard()->setText(text); receivingClipboard_ = false;
    });
    connect(QGuiApplication::clipboard(), &QClipboard::dataChanged, this, [this] {
        if (!receivingClipboard_ && clipboardMode_ != "off" && connected_) pasteClipboard();
    });
    thread_.start();
}
void Console::setClipboardMode(QString mode) {
    if (!QStringList{"off", "toGuest", "both"}.contains(mode) || clipboardMode_ == mode) return;
    clipboardMode_ = mode; emit clipboardModeChanged();
}
void Console::pasteClipboard() {
    if (!connected_ || clipboardMode_ == "off") return;
    auto text = QGuiApplication::clipboard()->text();
    QMetaObject::invokeMethod(worker_, [this, text] { worker_->clipboard(text); });
}
void Console::resizeGuest(int width, int height) { QMetaObject::invokeMethod(worker_, [=, this] { worker_->resizeGuest(width, height); }); }
void Console::sendSpecial(QString key) {
    if (!connected_) return;
    if (key == "ctrlaltdel") { sendCtrlAltDelete(); return; }
    QList<quint32> keys;
    if (key == "altf4") keys = {XK_Alt_L, XK_F4};
    else if (key == "super") keys = {XK_Super_L};
    else if (key == "ctrlaltbackspace") keys = {XK_Control_L, XK_Alt_L, XK_BackSpace};
    QMetaObject::invokeMethod(worker_, [this, keys] { for (auto key : keys) worker_->key(key, true); for (auto i = keys.rbegin(); i != keys.rend(); ++i) worker_->key(*i, false); });
}
Console::~Console() {
    QMetaObject::invokeMethod(worker_, &VncWorker::stop, Qt::BlockingQueuedConnection);
    thread_.quit(); thread_.wait();
}
void Console::attachForVm(GraphicsHandle socket, QString uuid) {
    // A previous tab's asynchronous graphics reply must never capture input here.
    if (socket && socket->uuid == uuid) attach(std::move(socket));
}
void Console::attach(GraphicsHandle socket) {
    // Clipboard sharing is chosen per VM: it never carries over to another VM's console.
    if (socket && socket->uuid != clipboardVm_) { clipboardVm_ = socket->uuid; setClipboardMode("off"); }
    releaseInput(); image_ = {}; update(); emit frameChanged();
    connected_ = false;
    auto generation = ++generation_;
    status_ = "Connecting console…"; emit statusChanged();
    QMetaObject::invokeMethod(worker_, [this, socket, generation] { worker_->start(socket, generation); });
}
void Console::disconnectConsole() {
    releaseInput();
    ++generation_; connected_ = false;
    QMetaObject::invokeMethod(worker_, &VncWorker::stop);
    image_ = {}; update(); emit frameChanged(); status_ = "Console closed · VM continues running"; emit statusChanged();
}
QRectF Console::destination() const {
    auto size = image_.size().scaled(QSize(int(width()), int(height())), Qt::KeepAspectRatio);
    return {(width() - size.width()) / 2, (height() - size.height()) / 2, double(size.width()), double(size.height())};
}
void Console::paint(QPainter *painter) {
    painter->fillRect(boundingRect(), Qt::black);
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);
    if (!image_.isNull()) painter->drawImage(destination(), image_);
}
void Console::releaseInput() {
    for (auto symbol : pressed_) QMetaObject::invokeMethod(worker_, [this, symbol] { worker_->key(symbol, false); });
    pressed_.clear();
    QMetaObject::invokeMethod(worker_, [this] { worker_->pointer(0, 0, 0); });
    captured_ = false;
    if (hasActiveFocus()) setFocus(false);
    emit statusChanged();
}
void Console::pointerAt(QPointF position, int buttons) {
    if (!captured_ || image_.isNull()) return;
    auto rect = destination();
    if (rect.width() <= 0 || rect.height() <= 0) return;
    int x = std::clamp(int((position.x() - rect.x()) * image_.width() / rect.width()), 0, image_.width() - 1);
    int y = std::clamp(int((position.y() - rect.y()) * image_.height() / rect.height()), 0, image_.height() - 1);
    QMetaObject::invokeMethod(worker_, [=, this] { worker_->pointer(x, y, buttons); });
}
void Console::sendPointer(QMouseEvent *event) {
    int buttons = (event->buttons() & Qt::LeftButton ? 1 : 0) | (event->buttons() & Qt::MiddleButton ? 2 : 0) | (event->buttons() & Qt::RightButton ? 4 : 0);
    pointerAt(event->position(), buttons);
    event->accept();
}
void Console::mousePressEvent(QMouseEvent *event) {
    if (!connected_ || image_.isNull() || !destination().contains(event->position())) { event->ignore(); return; }
    captured_ = true; forceActiveFocus(); emit statusChanged(); sendPointer(event);
}
void Console::mouseReleaseEvent(QMouseEvent *event) { sendPointer(event); }
void Console::mouseMoveEvent(QMouseEvent *event) { sendPointer(event); }
void Console::hoverMoveEvent(QHoverEvent *event) { pointerAt(event->position(), 0); }
void Console::wheelEvent(QWheelEvent *event) {
    if (!captured_) { event->ignore(); return; }
    int delta = event->angleDelta().y();
    if (delta) { pointerAt(event->position(), delta > 0 ? 8 : 16); pointerAt(event->position(), 0); }
    event->accept();
}
void Console::focusOutEvent(QFocusEvent *event) { releaseInput(); QQuickPaintedItem::focusOutEvent(event); }
void Console::sendKey(QKeyEvent *event, bool down) {
    if (!captured_ || event->isAutoRepeat()) return;
    if (down && (event->modifiers() & Qt::ControlModifier) && (event->modifiers() & Qt::AltModifier)) { releaseInput(); event->accept(); return; }
    quint32 physical = event->nativeScanCode() ? event->nativeScanCode() : quint32(event->key());
    if (!down) {
        auto symbol = pressed_.take(physical);
        if (symbol) QMetaObject::invokeMethod(worker_, [=, this] { worker_->key(symbol, false); });
        event->accept(); return;
    }
    // Wayland's nativeVirtualKey carries an XKB keysym. Fallback serves offscreen tests.
    quint32 symbol = event->nativeVirtualKey();
    if (!symbol) {
        switch (event->key()) {
        case Qt::Key_Return: symbol = XK_Return; break;
        case Qt::Key_Escape: symbol = XK_Escape; break;
        case Qt::Key_Backspace: symbol = XK_BackSpace; break;
        case Qt::Key_Tab: symbol = XK_Tab; break;
        case Qt::Key_Control: symbol = XK_Control_L; break;
        case Qt::Key_Alt: symbol = XK_Alt_L; break;
        default: if (!event->text().isEmpty()) { auto cp = event->text().toUcs4().first(); symbol = cp <= 255 ? cp : 0x01000000 | cp; }
        }
    }
    if (!symbol) return;
    pressed_.insert(physical, symbol);
    QMetaObject::invokeMethod(worker_, [=, this] { worker_->key(symbol, down); });
    event->accept();
}
void Console::keyPressEvent(QKeyEvent *event) { sendKey(event, true); }
void Console::keyReleaseEvent(QKeyEvent *event) { sendKey(event, false); }
void Console::sendCtrlAltDelete() {
    QMetaObject::invokeMethod(worker_, [this] {
        for (auto key : {XK_Control_L, XK_Alt_L, XK_Delete}) worker_->key(key, true);
        for (auto key : {XK_Delete, XK_Alt_L, XK_Control_L}) worker_->key(key, false);
    });
}

QString Console::checkpointPreview() const {
    if (image_.isNull()) return {};
    QByteArray data; QBuffer buffer(&data); buffer.open(QIODevice::WriteOnly);
    if (!image_.scaled(640, 360, Qt::KeepAspectRatio, Qt::SmoothTransformation).save(&buffer, "PNG")) return {};
    return "data:image/png;base64," + QString::fromLatin1(data.toBase64());
}
