/*
 * 86Box    A hypervisor and IBM PC system emulator that specializes in
 *          running old operating systems and software designed for IBM
 *          PC systems and compatibles from 1981 through fairly recent
 *          system designs based on the PCI bus.
 *
 *          This file is part of the 86Box distribution.
 *
 *          Debug console window.
 *
 * Authors: Michael Pratte, <mpratte@makefox.group>
 *
 *          Copyright 2026 Michael Pratte.
 */
#include "qt_debugconsole.hpp"

#include <QEvent>
#include <QFileDialog>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QClipboard>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QVBoxLayout>

#include <QApplication>
#include <QDir>
#include <QHostAddress>
#include <QIntValidator>
#include <QTcpServer>
#include <QTcpSocket>

extern "C" {
#include <86box/86box.h>
#include <86box/plat.h>
#include <86box/debugcon.h>
}

static QPointer<DebugConsole> debugconsole_instance;

/* On the emulation thread: hand the text to the window's thread, to go to
   the console the command came from. */
static void
debugconsole_output(int source, const char *text)
{
    const QString s = QString::fromUtf8(text);

    /* The application outlives any window, so the call always has somewhere
       to go; whether the window still exists is asked on its own thread. */
    QMetaObject::invokeMethod(qApp, [source, s]() {
        if (debugconsole_instance)
            debugconsole_instance->deliver(source, s);
    }, Qt::QueuedConnection);
}

DebugConsole::DebugConsole(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(tr("Debug console"));
    resize(760, 520);

    output = new QPlainTextEdit(this);
    output->setReadOnly(true);
    output->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    output->setMaximumBlockCount(20000);

    input = new QLineEdit(this);
    input->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    input->setPlaceholderText(tr("Command (type help); running one pauses the machine"));
    input->installEventFilter(this);

    /* The menu's Paste sends pasted lines the same way the keyboard's does. */
    input->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(input, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        QMenu *menu = input->createStandardContextMenu();

        for (QAction *a : menu->actions()) {
            if (a->objectName() == QStringLiteral("edit-paste")) {
                disconnect(a, &QAction::triggered, nullptr, nullptr);
                connect(a, &QAction::triggered, this, [this]() {
                    const QString clip = QApplication::clipboard()->text();

                    if (!pasteLines(clip))
                        input->insert(clip);
                });
            }
        }
        menu->exec(input->mapToGlobal(pos));
        delete menu;
    });

    auto *dump = new QPushButton(tr("Dump Memory..."), this);
    auto *regs = new QPushButton(tr("Dump Registers..."), this);
    dump->setAutoDefault(false);
    regs->setAutoDefault(false);

    /* Bottom right: the same console over TCP, on this computer only. */
    portEdit = new QLineEdit(QStringLiteral("1995"), this);
    portEdit->setValidator(new QIntValidator(1, 65535, portEdit));
    portEdit->setMaximumWidth(70);
    portEdit->setToolTip(tr("TCP port to share this console on, 127.0.0.1 only"));
    serverButton = new QPushButton(tr("Open"), this);
    serverButton->setAutoDefault(false);

    auto *buttons = new QHBoxLayout;
    buttons->addWidget(dump);
    buttons->addWidget(regs);
    buttons->addStretch();
    buttons->addWidget(new QLabel(tr("TCP port:"), this));
    buttons->addWidget(portEdit);
    buttons->addWidget(serverButton);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(output);
    layout->addWidget(input);
    layout->addLayout(buttons);

    connect(input, &QLineEdit::returnPressed, this, [this]() {
        const QString line = input->text();
        input->clear();
        run(line);
    });
    connect(dump, &QPushButton::clicked, this, &DebugConsole::dumpMemory);
    connect(regs, &QPushButton::clicked, this, &DebugConsole::dumpRegisters);
    connect(serverButton, &QPushButton::clicked, this, &DebugConsole::toggleServer);

    debugconsole_instance = this;
    debugcon_init(debugconsole_output);

    append(tr("Type help. A command pauses the machine; continue (c) resumes it.\n"));
    input->setFocus();
}

DebugConsole::~DebugConsole()
{
    if (server) {
        server->close();
        debugcon_token_remove();
    }
    if (debugconsole_instance == this)
        debugconsole_instance = nullptr;
}

/* A command's output, wherever it was typed: the window and every network
   client share the one console. The next line can go now. */
void
DebugConsole::deliver(int source, const QString &text)
{
    (void) source;
    append(text);
    inFlight = false;
    pump();
}

/* At most this many lines wait, from all the consoles together. */
static constexpr int debugcon_pending_max = 256;

void
DebugConsole::enqueue(int source, const QString &line)
{
    if (pending.size() >= debugcon_pending_max) {
        append(tr("Too many commands waiting; dropped: %1").arg(line));
        return;
    }
    pending.append({ source, line });

    /* Something unpaused the machine while a command waited for the
       paused loop: stop it again, or nothing would ever run. */
    if (inFlight && !dopause) {
        plat_pause(1);
        append(tr("Paused the machine to run this; continue (c) resumes it."));
    }
    pump();
}

/* Hand the next line on: the console's own words are acted on here, the
   rest go to the emulation thread, which runs them from its paused loop. */
void
DebugConsole::pump()
{
    while (!inFlight && !pending.isEmpty()) {
        const Line l = pending.takeFirst();

        if (control(l.source, l.text))
            continue;

        /* A command stops the machine where it is; it stays stopped until
           continue. The emulation thread echoes it just before its output. */
        if (!dopause) {
            plat_pause(1);
            append(tr("Paused the machine to run this; continue (c) resumes it."));
        }
        if (!debugcon_submit(l.source, l.text.toUtf8().constData())) {
            append(tr("Too many commands waiting; dropped: %1").arg(l.text));
            continue;
        }
        inFlight = true;
    }
}

/* Commands the console answers itself, on this thread, when their turn
   comes: running the machine is the window's business, not the paused
   loop's, and a command that reads nothing of the machine's needs it
   neither paused nor stopped. Answers whether it was one. */
bool
DebugConsole::control(int source, const QString &line)
{
    const QString cmd = line.trimmed();
    QString       reply;

    (void) source;

    if ((cmd == QStringLiteral("help")) || (cmd == QStringLiteral("h")) || (cmd == QStringLiteral("?"))) {
        append(QStringLiteral("(86box) ") + line);
        append(QString::fromUtf8(debugcon_help_text()));
        return true;
    }

    if (cmd == QStringLiteral("pause")) {
        if (!dopause)
            plat_pause(1);
        reply = tr("Paused.");
    } else if ((cmd == QStringLiteral("continue")) || (cmd == QStringLiteral("cont")) || (cmd == QStringLiteral("c")) ||
               (cmd == QStringLiteral("unpause")) || (cmd == QStringLiteral("run"))) {
        if (dopause)
            plat_pause(0);
        reply = tr("Running.");
    } else if (cmd == QStringLiteral("hardreset")) {
        /* The Action menu's Hard Reset, without its question. */
        pc_reset_hard();
        reply = tr("Hard reset.");
    } else
        return false;

    append(QStringLiteral("(86box) ") + line);
    append(reply);
    return true;
}

void
DebugConsole::listenOn(int port)
{
    if (server || (port < 1) || (port > 65535))
        return;
    portEdit->setText(QString::number(port));
    toggleServer();
}

void
DebugConsole::toggleServer()
{
    if (server) {
        for (QTcpSocket *sock : clients) {
            disconnect(sock, nullptr, this, nullptr);
            if (sock->property("debugcon_authed").toBool())
                sock->write("Closing.\r\n");
            sock->disconnectFromHost();
        }
        clients.clear();
        server->close();
        server->deleteLater();
        server = nullptr;
        debugcon_token_remove();
        serverButton->setText(tr("Open"));
        portEdit->setEnabled(true);
        append(tr("TCP console closed."));
        return;
    }

    quint16 port = portEdit->text().toUShort();

    if (!port) {
        append(tr("Type a port from 1 to 65535."));
        return;
    }

    /* A port something else holds: try the next, and the next. */
    server = new QTcpServer(this);
    for (int tries = 0; !server->listen(QHostAddress::LocalHost, port); tries++) {
        if ((server->serverError() != QAbstractSocket::AddressInUseError) || (tries >= 99) || (port == 65535)) {
            append(tr("Could not listen on 127.0.0.1:%1: %2").arg(port).arg(server->errorString()));
            server->deleteLater();
            server = nullptr;
            return;
        }
        port++;
    }

    /* No client gets in without the token. */
    const char *token_file = debugcon_token_create();
    if (token_file == NULL) {
        append(tr("Could not write the TCP console's token file; not listening."));
        server->close();
        server->deleteLater();
        server = nullptr;
        return;
    }
    pclog("Debug console: listening on 127.0.0.1:%u, token in %s\n", port, token_file);

    portEdit->setText(QString::number(port));
    connect(server, &QTcpServer::newConnection, this, &DebugConsole::newClient);
    serverButton->setText(tr("Close"));
    portEdit->setEnabled(false);
    append(tr("TCP console open on 127.0.0.1:%1. A client's first line must be auth TOKEN, "
              "with the token from %2.")
               .arg(port)
               .arg(QString::fromUtf8(token_file)));
}

void
DebugConsole::newClient()
{
    while (server && server->hasPendingConnections()) {
        QTcpSocket *sock = server->nextPendingConnection();
        const int   id   = nextClient++;

        /* Nothing is said to it, and nothing of the console's output goes
           to it, until it has given the token. */
        clients.insert(id, sock);
        sock->setProperty("debugcon_id", id);
        sock->setProperty("debugcon_authed", false);
        connect(sock, &QTcpSocket::readyRead, this, [this, sock]() { clientData(sock); });
        connect(sock, &QTcpSocket::disconnected, this, [this, sock, id]() {
            const bool authed = sock->property("debugcon_authed").toBool();

            clients.remove(id);
            sock->deleteLater();
            if (authed)
                append(tr("TCP console %1 disconnected.").arg(id));
        });
    }
}

/* The longest line a client may send; a longer one ends the connection. */
static constexpr qint64 debugcon_line_max = 4096;

/* A line at a time, as it arrives. */
void
DebugConsole::clientData(QTcpSocket *sock)
{
    const int id = sock->property("debugcon_id").toInt();

    /* Nothing more from one that is being let go. */
    if (sock->state() != QAbstractSocket::ConnectedState)
        return;

    while (sock->canReadLine()) {
        const QByteArray bytes = sock->readLine(debugcon_line_max + 1);

        if (!bytes.endsWith('\n')) {
            sock->write("Line too long.\r\n");
            sock->disconnectFromHost();
            return;
        }

        QString line = QString::fromUtf8(bytes);

        while (line.endsWith(QLatin1Char('\n')) || line.endsWith(QLatin1Char('\r')))
            line.chop(1);

        /* The first line is auth TOKEN; anything else ends the connection. */
        if (!sock->property("debugcon_authed").toBool()) {
            if (!line.startsWith(QStringLiteral("auth ")) || !debugcon_token_matches(line.mid(5).toUtf8().constData())) {
                pclog("Debug console: rejected a TCP client without a valid token\n");
                sock->write("Authentication required.\r\n");
                sock->disconnectFromHost();
                return;
            }
            sock->setProperty("debugcon_authed", true);
            append(tr("TCP console %1 connected from %2.").arg(id).arg(sock->peerAddress().toString()));
            sock->write(QStringLiteral("86Box debug console %1. Type help; pause and continue run the machine.\r\n")
                            .arg(id)
                            .toUtf8());
            continue;
        }

        if (line.trimmed().isEmpty())
            continue;

        if ((line.trimmed() == QStringLiteral("quit")) || (line.trimmed() == QStringLiteral("exit"))) {
            sock->disconnectFromHost();
            return;
        }
        enqueue(id, line);
    }

    /* A line that has not ended yet, but is already too long. */
    if (sock->bytesAvailable() > debugcon_line_max) {
        sock->write("Line too long.\r\n");
        sock->disconnectFromHost();
    }
}

/* Everything the console says goes through here: into the window, and to
   every network client, which share it. */
void
DebugConsole::append(const QString &text)
{
    QString t = text;

    if (t.endsWith('\n'))
        t.chop(1);
    output->appendPlainText(t);
    output->verticalScrollBar()->setValue(output->verticalScrollBar()->maximum());

    if (!clients.isEmpty()) {
        const QByteArray bytes = (QString(t).replace(QStringLiteral("\n"), QStringLiteral("\r\n")) + QStringLiteral("\r\n")).toUtf8();

        for (QTcpSocket *sock : clients) {
            if (sock->property("debugcon_authed").toBool())
                sock->write(bytes);
        }
    }
}

void
DebugConsole::run(const QString &text)
{
    QStringList lines;

    for (QString l : text.split(QLatin1Char('\n'))) {
        if (l.endsWith(QLatin1Char('\r')))
            l.chop(1);
        if (!l.trimmed().isEmpty())
            lines.append(l);
    }

    for (const QString &l : lines) {
        if (history.isEmpty() || (history.last() != l))
            history.append(l);
    }
    historyPos = history.size();

    if (lines.isEmpty()) {
        append(QStringLiteral("(86box) "));
        return;
    }

    for (const QString &l : lines)
        enqueue(0, l);
}

void
DebugConsole::dumpMemory()
{
    const QString path = QFileDialog::getSaveFileName(this, tr("Dump Memory"),
                                                      lastDumpPath.isEmpty() ? QStringLiteral("memory.bin") : lastDumpPath,
                                                      tr("Raw memory image (*.bin);;All files (*)"));
    if (path.isEmpty())
        return;

    lastDumpPath = path;
    run(QStringLiteral("dump ram \"%1\"").arg(QDir::toNativeSeparators(path)));
}

void
DebugConsole::dumpRegisters()
{
    const QString path = QFileDialog::getSaveFileName(this, tr("Dump Registers"),
                                                      lastRegsPath.isEmpty() ? QStringLiteral("registers.txt") : lastRegsPath,
                                                      tr("Text file (*.txt);;All files (*)"));
    if (path.isEmpty())
        return;

    lastRegsPath = path;
    run(QStringLiteral("dump registers \"%1\"").arg(QDir::toNativeSeparators(path)));
}

/* Pasted text with line breaks is sent at once, each line a command, the
   first joined to what is already typed; answers whether it was. */
bool
DebugConsole::pasteLines(const QString &clip)
{
    if (!clip.contains(QLatin1Char('\n')))
        return false;

    const QString text = input->text().left(input->cursorPosition()) + clip;

    input->clear();
    run(text);
    return true;
}

/* Up and down walk the command history, a line at a time. */
bool
DebugConsole::eventFilter(QObject *obj, QEvent *event)
{
    if ((obj == input) && (event->type() == QEvent::KeyPress)) {
        const auto *ke = static_cast<QKeyEvent *>(event);

        /* A paste of several lines runs them, one command each, with the
           first joined to whatever is already typed. */
        if (ke->matches(QKeySequence::Paste) && pasteLines(QApplication::clipboard()->text()))
            return true;
        if ((ke->key() == Qt::Key_Up) && (historyPos > 0)) {
            input->setText(history.at(--historyPos));
            return true;
        }
        if (ke->key() == Qt::Key_Down) {
            if (historyPos < (history.size() - 1))
                input->setText(history.at(++historyPos));
            else {
                historyPos = history.size();
                input->clear();
            }
            return true;
        }
    }

    return QDialog::eventFilter(obj, event);
}
